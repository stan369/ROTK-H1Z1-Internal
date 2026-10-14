#include <iostream>
#include <sstream>
#include <fstream>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <shlobj.h>
#include "Lol2.h"
#include "Offs.h"
#include "gui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

using namespace std;

LPDIRECT3D9 d3d;
LPDIRECT3DDEVICE9 d3ddev;
HWND hWnd;
HWND tWnd;
DWORD procID;
HANDLE handle;
uint64_t g_moduleBase = 0;
uint64_t foundGameAddr = 0;  // Dynamically found game state pointer (from signature scan)
uint64_t adjustedGraphicsAddr = 0;  // moduleBase + RVA_GRAPHICS_SLOT

// No-op for the clean build - no diagnostic file on disk. Call sites throughout this file
// are left in place (harmless now) rather than stripped out individually.
void Log(const char*)
{
}

// Simple memory read template
template<typename T> inline T read(HANDLE handle, DWORD_PTR address)
{
	T t;
	SecureZeroMemory(&t, sizeof(t));
	if (ReadProcessMemory(handle, (LPCVOID)address, &t, sizeof(t), NULL))
		return t;
	return T();
}

// Simple memory write template
template<typename T> inline T write(HANDLE handle, DWORD_PTR address, int value)
{
	T t = T();
	if (NtWriteVirtualMemory(handle, (PVOID)address, &value, sizeof(t), NULL))
		return t;
	return T();
}

// Signature scanning - finds patterns in memory
uint64_t FindSignature(HANDLE hProcess, const char* signature, uint64_t searchStart, uint64_t searchEnd)
{
	// Parse signature pattern (e.g., "48 8B 05 ? ? ? ?")
	std::vector<uint8_t> pattern;
	std::vector<bool> mask;

	const char* sig = signature;
	while (*sig) {
		if (*sig == ' ') {
			sig++;
			continue;
		}
		if (*sig == '?') {
			pattern.push_back(0);
			mask.push_back(false);  // Don't compare this byte
			sig += 2;  // Skip "? "
		} else {
			uint8_t byte = (uint8_t)strtol(sig, (char**)&sig, 16);
			pattern.push_back(byte);
			mask.push_back(true);  // Do compare this byte
		}
	}

	if (pattern.empty()) return 0;

	// Search in 64 KB chunks, each read extended by the pattern length so a match straddling
	// a chunk boundary is still found. Plain arrays + first-byte skip: the old vector<bool>
	// per-byte compare made a full-module scan take over a minute in a Debug build.
	const size_t n = pattern.size();
	std::vector<uint8_t> need(n);
	for (size_t j = 0; j < n; j++) need[j] = mask[j] ? 1 : 0;
	const uint8_t* pat = pattern.data();
	const uint8_t* nd = need.data();

	const size_t chunkSize = 0x10000;
	std::vector<uint8_t> buffer(chunkSize + n);

	for (uint64_t addr = searchStart; addr < searchEnd; addr += chunkSize) {
		size_t want = (size_t)(searchEnd - addr);
		if (want > chunkSize + n - 1) want = chunkSize + n - 1;
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(hProcess, (LPCVOID)addr, buffer.data(), want, &bytesRead) || bytesRead < n) {
			continue;
		}

		const uint8_t* buf = buffer.data();
		const size_t last = bytesRead - n;
		for (size_t i = 0; i <= last; i++) {
			if (nd[0] && buf[i] != pat[0]) continue;
			size_t j = 1;
			for (; j < n; j++) {
				if (nd[j] && buf[i + j] != pat[j]) break;
			}
			if (j == n) return addr + i;
		}
	}

	return 0;
}

// ============================================================
// D3D11 / DXGI Present hook - H1Z1's ForgeLight engine renders
// with D3D11, so the overlay hook has to live on the swap chain's
// Present call, not a D3D9 device.
// ============================================================

typedef HRESULT(APIENTRY* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

// This DLL's own module handle (set in DllMain on DLL_PROCESS_ATTACH) and the real swap
// chain pointer (cached on the first hkPresent call) - both needed by the "Unload" menu
// action to unhook everything and free the DLL from the process cleanly, without waiting
// for the whole game to close.
HMODULE g_hModule = nullptr;
IDXGISwapChain* g_pSwapChainCached = nullptr;
bool g_unloadRequested = false;

// ImGui menu - subclasses the game's own window (not a separate overlay window) so
// ImGui_ImplWin32 can receive real mouse/keyboard input. Installed once, lazily, the
// first time InitD3D11Resources runs (that's when we first have a swap chain to pull
// the real HWND from).
Gui g_gui;
bool g_guiInitialized = false;
HWND g_gameHwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_menuOpen = false;  // declared here (not with the other menu state, further down)
                          // since hkWndProc/hkSetCursorPos/hkClipCursor all need it and
                          // are defined before that point in the file

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (g_menuOpen) {
		ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
		// Swallow mouse/keyboard while the menu is open so clicks/typing go to the menu
		// instead of also reaching the game (camera look, shooting, etc.) underneath it.
		switch (msg) {
		case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
			return true;
		}
	}
	return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}

// H1Z1 almost certainly re-centers/re-confines the OS cursor every frame for its own
// raw-input camera control (standard FPS technique - keeps the pointer from hitting a
// screen edge and clamping, which would break relative-delta calculation). That fights
// our menu's cursor: showing it and swallowing input messages isn't enough if the game
// keeps yanking it back to center underneath us. Fix: IAT-hook SetCursorPos/ClipCursor
// in the game's own import table and no-op them while the menu is open.
typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
SetCursorPos_t oSetCursorPos = nullptr;
ClipCursor_t oClipCursor = nullptr;

BOOL WINAPI hkSetCursorPos(int x, int y)
{
	if (g_menuOpen) return TRUE;  // pretend it worked; don't actually move the cursor
	return oSetCursorPos ? oSetCursorPos(x, y) : TRUE;
}

BOOL WINAPI hkClipCursor(const RECT* rect)
{
	if (g_menuOpen) return TRUE;  // don't let the game re-confine the cursor while we want free movement
	return oClipCursor ? oClipCursor(rect) : TRUE;
}

// Walks the PE import table of a loaded module looking for a specific imported function
// and swaps its IAT slot to point at hookFunc, returning the original function pointer.
// Safer than an inline hook here - we're only overwriting a data-table entry (a function
// pointer slot), never patching machine code bytes, so there's no prologue-length/
// re-entrancy risk to worry about.
void* HookIAT(uint64_t moduleBase, const char* dllName, const char* funcName, void* hookFunc)
{
	auto dosHeader = (PIMAGE_DOS_HEADER)moduleBase;
	auto ntHeaders = (PIMAGE_NT_HEADERS)(moduleBase + dosHeader->e_lfanew);
	auto importDesc = (PIMAGE_IMPORT_DESCRIPTOR)(moduleBase + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	for (; importDesc->Name; importDesc++) {
		char* modName = (char*)(moduleBase + importDesc->Name);
		if (_stricmp(modName, dllName) != 0) continue;

		auto thunk = (PIMAGE_THUNK_DATA)(moduleBase + importDesc->FirstThunk);
		auto origThunk = (PIMAGE_THUNK_DATA)(moduleBase + importDesc->OriginalFirstThunk);
		for (; origThunk->u1.AddressOfData; thunk++, origThunk++) {
			if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
			auto importByName = (PIMAGE_IMPORT_BY_NAME)(moduleBase + origThunk->u1.AddressOfData);
			if (strcmp((char*)importByName->Name, funcName) != 0) continue;

			void* original = (void*)thunk->u1.Function;
			DWORD oldProtect;
			VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect);
			thunk->u1.Function = (ULONGLONG)hookFunc;
			VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
			return original;
		}
	}
	return nullptr;
}

ID3D11Device* g_pDevice = nullptr;
ID3D11DeviceContext* g_pContext = nullptr;
ID3D11RenderTargetView* g_pRTV = nullptr;
ID3D11VertexShader* g_pVS = nullptr;
ID3D11PixelShader* g_pPS = nullptr;
ID3D11InputLayout* g_pInputLayout = nullptr;
ID3D11Buffer* g_pVertexBuffer = nullptr;
ID3D11BlendState* g_pBlendState = nullptr;
ID3D11DepthStencilState* g_pNoDepthState = nullptr;
bool g_d3d11Ready = false;
const UINT g_maxVerts = 2048;

struct NdcVertex {
	float x, y, z;
	float r, g, b, a;
};

static const char* g_vsSrc =
	"struct VS_IN { float3 pos : POSITION; float4 color : COLOR; };\n"
	"struct PS_IN { float4 pos : SV_POSITION; float4 color : COLOR; };\n"
	"PS_IN main(VS_IN input) {\n"
	"    PS_IN output;\n"
	"    output.pos = float4(input.pos, 1.0);\n"
	"    output.color = input.color;\n"
	"    return output;\n"
	"}\n";

static const char* g_psSrc =
	"struct PS_IN { float4 pos : SV_POSITION; float4 color : COLOR; };\n"
	"float4 main(PS_IN input) : SV_TARGET {\n"
	"    return input.color;\n"
	"}\n";

// ============================================================
// Text rendering - D3D11 has no fixed-function font support like
// D3DX9's ID3DXFont, so glyphs are rasterized once via GDI into a
// texture atlas, then drawn as textured quads (one draw call per
// frame covering every character queued that frame).
// ============================================================

struct TexVertex {
	float x, y, z;
	float u, v;
	float r, g, b, a;
};

struct FontChar {
	float u0, v0, u1, v1;
};

const int FONT_FIRST_CHAR = 32;
const int FONT_LAST_CHAR = 126;
const int FONT_CHAR_COUNT = FONT_LAST_CHAR - FONT_FIRST_CHAR + 1;
const int FONT_PIXEL_HEIGHT = 16;
FontChar g_fontChars[FONT_CHAR_COUNT];
int g_fontCellW = 0, g_fontCellH = 0;
int g_fontAtlasW = 0, g_fontAtlasH = 0;
float g_fontAdvanceW = 0.0f;  // how far the pen moves per character - narrower than the
                               // glyph quad itself, since the quad is padded to avoid clipping

ID3D11ShaderResourceView* g_pFontSRV = nullptr;
ID3D11SamplerState* g_pFontSampler = nullptr;
ID3D11VertexShader* g_pTextVS = nullptr;
ID3D11PixelShader* g_pTextPS = nullptr;
ID3D11InputLayout* g_pTextInputLayout = nullptr;
ID3D11Buffer* g_pTextVertexBuffer = nullptr;
bool g_fontReady = false;
const UINT g_maxTextVerts = 6 * 512;  // 512 characters/frame

static const char* g_textVsSrc =
	"struct VS_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };\n"
	"struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };\n"
	"PS_IN main(VS_IN input) {\n"
	"    PS_IN output;\n"
	"    output.pos = float4(input.pos, 1.0);\n"
	"    output.uv = input.uv;\n"
	"    output.color = input.color;\n"
	"    return output;\n"
	"}\n";

static const char* g_textPsSrc =
	"Texture2D tex0 : register(t0);\n"
	"SamplerState samp0 : register(s0);\n"
	"struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };\n"
	"float4 main(PS_IN input) : SV_TARGET {\n"
	"    float glyphAlpha = tex0.Sample(samp0, input.uv).a;\n"
	"    return float4(input.color.rgb, input.color.a * glyphAlpha);\n"
	"}\n";

// Rasterizes ASCII 32-126 into a texture atlas via GDI, since D3D11 has no built-in
// font support. Text pixels come out as white RGB with alpha = glyph coverage, so the
// pixel shader can tint them by the per-draw vertex color.
bool InitFontAtlas()
{
	int cols = 16;
	int rows = (FONT_CHAR_COUNT + cols - 1) / cols;
	g_fontCellW = FONT_PIXEL_HEIGHT;
	g_fontCellH = FONT_PIXEL_HEIGHT + 4;
	g_fontAdvanceW = g_fontCellW * 0.6f;  // real glyph width is narrower than the padded cell
	g_fontAtlasW = cols * g_fontCellW;
	g_fontAtlasH = rows * g_fontCellH;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = g_fontAtlasW;
	bmi.bmiHeader.biHeight = -g_fontAtlasH;  // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* pBits = nullptr;
	HDC screenDC = GetDC(NULL);
	HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
	ReleaseDC(NULL, screenDC);
	if (!hBitmap || !pBits) {
		Log("InitFontAtlas: CreateDIBSection failed");
		return false;
	}
	memset(pBits, 0, (size_t)g_fontAtlasW * g_fontAtlasH * 4);

	HDC memDC = CreateCompatibleDC(NULL);
	HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBitmap);

	HFONT hFont = CreateFontA(-FONT_PIXEL_HEIGHT, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
		FIXED_PITCH | FF_MODERN, "Consolas");
	HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

	SetBkMode(memDC, TRANSPARENT);
	SetTextColor(memDC, RGB(255, 255, 255));

	for (int i = 0; i < FONT_CHAR_COUNT; i++) {
		char ch = (char)(FONT_FIRST_CHAR + i);
		int cellX = (i % cols) * g_fontCellW;
		int cellY = (i / cols) * g_fontCellH;
		TextOutA(memDC, cellX, cellY, &ch, 1);

		g_fontChars[i].u0 = (float)cellX / g_fontAtlasW;
		g_fontChars[i].v0 = (float)cellY / g_fontAtlasH;
		g_fontChars[i].u1 = (float)(cellX + g_fontCellW) / g_fontAtlasW;
		g_fontChars[i].v1 = (float)(cellY + g_fontCellH) / g_fontAtlasH;
	}

	SelectObject(memDC, oldFont);
	DeleteObject(hFont);
	SelectObject(memDC, oldBmp);
	DeleteDC(memDC);

	// GDI blended white-on-black anti-aliased text into RGB (no alpha channel written).
	// Use that grayscale intensity as our alpha, and force RGB to solid white so the
	// pixel shader's vertex-color tint isn't muddied by GDI's blending.
	uint8_t* bytes = (uint8_t*)pBits;
	size_t pixelCount = (size_t)g_fontAtlasW * g_fontAtlasH;
	for (size_t p = 0; p < pixelCount; p++) {
		uint8_t intensity = bytes[p * 4 + 0];  // B channel (== G == R under grayscale AA)
		bytes[p * 4 + 0] = 255;
		bytes[p * 4 + 1] = 255;
		bytes[p * 4 + 2] = 255;
		bytes[p * 4 + 3] = intensity;
	}

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = g_fontAtlasW;
	texDesc.Height = g_fontAtlasH;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData = {};
	initData.pSysMem = pBits;
	initData.SysMemPitch = g_fontAtlasW * 4;

	ID3D11Texture2D* pTexture = nullptr;
	HRESULT hr = g_pDevice->CreateTexture2D(&texDesc, &initData, &pTexture);
	DeleteObject(hBitmap);

	if (FAILED(hr)) {
		Log("InitFontAtlas: CreateTexture2D failed");
		return false;
	}

	hr = g_pDevice->CreateShaderResourceView(pTexture, NULL, &g_pFontSRV);
	pTexture->Release();
	if (FAILED(hr)) {
		Log("InitFontAtlas: CreateShaderResourceView failed");
		return false;
	}

	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	g_pDevice->CreateSamplerState(&sampDesc, &g_pFontSampler);

	Log("InitFontAtlas: success");
	return true;
}

// Sets up the shaders/buffers used to draw ESP boxes. Runs once, on the first hooked frame.
bool InitD3D11Resources(IDXGISwapChain* pSwapChain)
{
	if (g_d3d11Ready) return true;

	if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pDevice))) {
		Log("InitD3D11Resources: GetDevice failed");
		return false;
	}
	g_pDevice->GetImmediateContext(&g_pContext);

	// Subclass the game's own window (not a separate overlay window) so ImGui can get
	// real mouse/keyboard input, then init ImGui against the game's own device/context -
	// no second D3D11 device, no separate window, matching how the rest of this cheat is
	// internal (injected into and rendering through the game's own pipeline).
	if (!g_guiInitialized) {
		DXGI_SWAP_CHAIN_DESC scDescForHwnd;
		pSwapChain->GetDesc(&scDescForHwnd);
		g_gameHwnd = scDescForHwnd.OutputWindow;
		if (g_gameHwnd) {
			g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
			g_gui.Init(g_gameHwnd, g_pDevice, g_pContext);
			g_guiInitialized = true;
			Log("InitD3D11Resources: ImGui menu initialized");

			if (g_moduleBase) {
				oSetCursorPos = (SetCursorPos_t)HookIAT(g_moduleBase, "user32.dll", "SetCursorPos", (void*)hkSetCursorPos);
				oClipCursor = (ClipCursor_t)HookIAT(g_moduleBase, "user32.dll", "ClipCursor", (void*)hkClipCursor);
				char cursorHookBuf[128];
				sprintf_s(cursorHookBuf, "InitD3D11Resources: cursor hooks - SetCursorPos=%p ClipCursor=%p", (void*)oSetCursorPos, (void*)oClipCursor);
				Log(cursorHookBuf);
			}
		} else {
			Log("InitD3D11Resources: could not get game HWND - ImGui menu unavailable");
		}
	}

	ID3D11Texture2D* pBackBuffer = nullptr;
	pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
	g_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_pRTV);
	pBackBuffer->Release();

	ID3DBlob* vsBlob = nullptr;
	ID3DBlob* psBlob = nullptr;
	ID3DBlob* errBlob = nullptr;

	if (FAILED(D3DCompile(g_vsSrc, strlen(g_vsSrc), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &vsBlob, &errBlob))) {
		Log("InitD3D11Resources: vertex shader compile failed");
		if (errBlob) { Log((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(g_psSrc, strlen(g_psSrc), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &psBlob, &errBlob))) {
		Log("InitD3D11Resources: pixel shader compile failed");
		if (errBlob) { Log((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
		vsBlob->Release();
		return false;
	}

	g_pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &g_pVS);
	g_pDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &g_pPS);

	D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	g_pDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_pInputLayout);

	vsBlob->Release();
	psBlob->Release();

	D3D11_BUFFER_DESC vbDesc = {};
	vbDesc.Usage = D3D11_USAGE_DYNAMIC;
	vbDesc.ByteWidth = sizeof(NdcVertex) * g_maxVerts;
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	g_pDevice->CreateBuffer(&vbDesc, NULL, &g_pVertexBuffer);

	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	g_pDevice->CreateBlendState(&blendDesc, &g_pBlendState);

	D3D11_DEPTH_STENCIL_DESC depthDesc = {};
	depthDesc.DepthEnable = FALSE;
	depthDesc.StencilEnable = FALSE;
	g_pDevice->CreateDepthStencilState(&depthDesc, &g_pNoDepthState);

	ID3DBlob* textVsBlob = nullptr;
	ID3DBlob* textPsBlob = nullptr;
	if (FAILED(D3DCompile(g_textVsSrc, strlen(g_textVsSrc), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &textVsBlob, &errBlob))) {
		Log("InitD3D11Resources: text vertex shader compile failed");
		if (errBlob) { Log((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(g_textPsSrc, strlen(g_textPsSrc), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &textPsBlob, &errBlob))) {
		Log("InitD3D11Resources: text pixel shader compile failed");
		if (errBlob) { Log((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
		textVsBlob->Release();
		return false;
	}

	g_pDevice->CreateVertexShader(textVsBlob->GetBufferPointer(), textVsBlob->GetBufferSize(), NULL, &g_pTextVS);
	g_pDevice->CreatePixelShader(textPsBlob->GetBufferPointer(), textPsBlob->GetBufferSize(), NULL, &g_pTextPS);

	D3D11_INPUT_ELEMENT_DESC textLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	g_pDevice->CreateInputLayout(textLayout, 3, textVsBlob->GetBufferPointer(), textVsBlob->GetBufferSize(), &g_pTextInputLayout);

	textVsBlob->Release();
	textPsBlob->Release();

	D3D11_BUFFER_DESC textVbDesc = {};
	textVbDesc.Usage = D3D11_USAGE_DYNAMIC;
	textVbDesc.ByteWidth = sizeof(TexVertex) * g_maxTextVerts;
	textVbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	textVbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	g_pDevice->CreateBuffer(&textVbDesc, NULL, &g_pTextVertexBuffer);

	g_fontReady = InitFontAtlas();

	g_d3d11Ready = true;
	Log("InitD3D11Resources: success");
	return true;
}

// Projects a world position into NDC space (x/w, y/w) using the game's view-projection matrix.
// ForgeLight's matrix uses an inverted convention on the Y column vs. the standard D3D layout
// (confirmed against a known-working H1Z1 external cheat's GM()/W2S() - it transposes the
// matrix then negates the whole second row, which works out to exactly this sign flip on Y).
bool WorldToNDC(const D3DXMATRIX& m, const D3DXVECTOR3& world, D3DXVECTOR2& outNdc)
{
	float w = m.m[0][3] * world.x + m.m[1][3] * world.y + m.m[2][3] * world.z + m.m[3][3];
	if (w < 0.1f) return false;  // Behind the camera

	float x = m.m[0][0] * world.x + m.m[1][0] * world.y + m.m[2][0] * world.z + m.m[3][0];
	float y = -(m.m[0][1] * world.x + m.m[1][1] * world.y + m.m[2][1] * world.z + m.m[3][1]);

	outNdc.x = x / w;
	outNdc.y = y / w;
	return true;
}

vector<NdcVertex> g_frameVerts;

// Separate from g_frameVerts because the box draw loop assumes every entry is a fixed
// 5-vertex strip (see the "Draw(5, i)" loop below) - a circle needs many more segments
// than that, so it gets its own vector and its own single Draw call.
vector<NdcVertex> g_circleVerts;

// Skeleton bone lines - each is its own independent 2-vertex segment (not a connected
// strip like g_frameVerts' boxes), drawn the same way the box loop is: Draw(2, i) per
// pair, so unrelated bones/players never get a stray line connecting them.
vector<NdcVertex> g_skeletonVerts;

void QueueSkeletonLineNdc(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
	g_skeletonVerts.push_back({ x0, y0, 0, r, g, b, a });
	g_skeletonVerts.push_back({ x1, y1, 0, r, g, b, a });
}

bool g_showPlayerBoxes = true;
bool g_showItemESP = true;
bool g_showPlayerNames = true;
bool g_showVehicleESP = true;
bool g_showLootbags = false;
bool g_showGrenades = false;
bool g_showGroundWeapons = false;  // dropped/world weapon pickups (weaponEntityType)
bool g_showHealthBars = true;
bool g_showDistance = false;  // distance-in-meters text under player/vehicle ESP boxes
bool g_showHeadDot = false;  // a dot at the same feet+1.9m position the ESP box top uses,
                              // upgraded below to the real skeleton head bone when resolvable
bool g_showSkeleton = false;  // stride corrected to 0x10, see Offs.h

// NoRecoil (retry): instead of fighting recoil by rewriting a memory value every
// frame (0x534 turned out not to be the field that actually drives camera kick - see
// the earlier attempt), this NOPs out the actual CALL instruction that applies aim
// punch. Fully reversible: toggling back off restores the original bytes.
static const uint8_t AIMPUNCH_ORIGINAL_BYTES[5] = { 0xE8, 0x27, 0xA4, 0x5B, 0xFE };
static const uint8_t AIMPUNCH_NOP_BYTES[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
uint64_t g_aimPunchPatchAddr = 0;  // found via signature scan in DllMain
bool g_noRecoil = false;           // desired state (menu toggle)
bool g_aimPunchPatched = false;    // actual current state of the bytes in memory

// Applies or reverts the 5-byte NOP patch at the aim-punch call site. Safe to call
// repeatedly - only writes when the desired state doesn't match reality.
void ApplyAimPunchPatch(bool enable)
{
	if (!g_aimPunchPatchAddr || enable == g_aimPunchPatched) return;

	const uint8_t* bytesToWrite = enable ? AIMPUNCH_NOP_BYTES : AIMPUNCH_ORIGINAL_BYTES;
	DWORD oldProtect;
	if (VirtualProtect((LPVOID)g_aimPunchPatchAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		memcpy((void*)g_aimPunchPatchAddr, bytesToWrite, 5);
		DWORD temp;
		VirtualProtect((LPVOID)g_aimPunchPatchAddr, 5, oldProtect, &temp);
		g_aimPunchPatched = enable;
		Log(enable ? "ApplyAimPunchPatch: NOPed out aim punch call" : "ApplyAimPunchPatch: restored original bytes");
	} else {
		Log("ApplyAimPunchPatch: VirtualProtect failed");
	}
}

// Aimbot: predictive aim toward the closest-to-crosshair target while holding the
// configurable activation key. Head position uses the same feet+height-offset
// approximation our ESP boxes already rely on (validated), not the skeleton/bone system -
// that data turned out to be garbage when we tried it for the skeleton overlay, so this
// avoids rebuilding on top of a foundation we already know doesn't work for this build.
bool g_aimbot = false;
const float AIMBOT_HEAD_HEIGHT = 1.65f;  // approximate head height above feet, standing
float g_aimbotFov = 30.0f;  // pixel radius from screen center a target must be within -
                             // adjustable via the menu slider, was a fixed const
bool g_showFovCircle = false;  // draws a ring of this radius around the crosshair so the
                                // FOV value can be seen/tuned without opening the menu
float g_aimbotSmoothness = 1.0f;  // divides the per-frame mouse move - 1 = instant snap to
                                   // the target, higher = the move is spread over more
                                   // frames (each frame only closes part of the remaining
                                   // distance, since the target screen position is
                                   // recomputed fresh every frame)
bool g_weaponProfiles = false;  // when on, FOV/smoothness follow the held weapon instead
                                 // of the sliders above - the sliders still apply as the
                                 // fallback for any weapon not in the table below
bool g_weaponProfilesEditorOpen = false;  // GUI-only: whether the per-weapon editor panel
                                           // is showing, independent of g_weaponProfiles
                                           // itself - separate bools so closing the editor
                                           // (to go back to the normal Combat tab) doesn't
                                           // also turn the feature off. Not persisted; the
                                           // editor should start closed on every menu open.
float g_effectiveAimbotFov = 30.0f;
float g_effectiveAimbotSmoothness = 1.0f;

// Friend List: aim-at-a-player-and-press-a-key exclusion list for Aimbot,
// matched by name (case-insensitive) rather than any team/group ID - no client-side field
// for "is this entity on my team" appears to exist for arbitrary nearby players (the
// server that owns the local test setup never broadcasts one; group membership is only
// ever sent as your own roster, not stamped per-entity), so name-based opt-in is the
// practical alternative. Persisted as one name per line in a plain text file next to the
// config. Fails closed: if the list can't be loaded, or a candidate's name can't be read
// this frame, it's treated as excluded rather than risk targeting an actual friend.
bool g_friendListEnabled = false;
vector<string> g_friendNames;
bool g_friendListLoaded = false;
char g_friendListPath[MAX_PATH] = {};
int g_addFriendKey = VK_F6;
bool g_addFriendKeyWasDown = false;
string g_friendNotice;
ULONGLONG g_friendNoticeUntil = 0;
int g_aimbotStanceDiagLeft = 60;  // bumped up + now only fires on non-standing targets
                                   // (see the call site) - a single standing target sitting
                                   // in view for a couple seconds was burning the whole
                                   // budget before ever catching an actual crouch sample

// Activation key, user-rebindable via the "Aimbot Key" menu row instead of a fixed
// VK_RBUTTON. Rebinding is generic (g_awaitingKeybindTarget) so it works for any of the
// rebindable keys below, not just this one: ENTER on a keybind row points the target at
// that row's variable, then the very next key pressed (checked in the menu input block,
// excluding the menu's own nav keys) gets written into it and the target clears.
int g_aimbotKey = VK_RBUTTON;
int* g_awaitingKeybindTarget = nullptr;

// Toggle hotkeys - press once to flip the feature fully on/off, independent of the menu
// being open, so they can be used mid-fight without alt-tabbing into the overlay.
int g_aimbotToggleKey = VK_F1;
int g_noRecoilToggleKey = VK_F2;
bool g_aimbotToggleKeyWasDown = false;
bool g_noRecoilToggleKeyWasDown = false;

// Friendly name for the menu display - covers mouse buttons, letters/digits, F-keys and
// a handful of common named keys; anything else just falls back to its raw VK hex code.
const char* GetKeyName(int vk)
{
	static char buf[16];
	switch (vk) {
		case VK_LBUTTON: return "MOUSE1";
		case VK_RBUTTON: return "MOUSE2";
		case VK_MBUTTON: return "MOUSE3";
		case VK_XBUTTON1: return "MOUSE4";
		case VK_XBUTTON2: return "MOUSE5";
		case VK_SHIFT: return "SHIFT";
		case VK_CONTROL: return "CTRL";
		case VK_MENU: return "ALT";
		case VK_SPACE: return "SPACE";
		case VK_TAB: return "TAB";
		case VK_CAPITAL: return "CAPSLOCK";
	}
	if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = '\0'; return buf; }
	if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = '\0'; return buf; }
	if (vk >= VK_F1 && vk <= VK_F12) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
	sprintf_s(buf, "VK 0x%02X", vk);
	return buf;
}

// Per-weapon bullet speed (m/s), re-resolved once per frame from the local player's held
// weapon's internal asset name (e.g. "Weapons_AK47_3P.adr") instead of one fixed constant -
// prediction lead is way off if a shotgun's slow pellets get treated like a sniper round.
// Defaults to the AR-15/AK-47 speed if the name can't be resolved.
float g_aimbotBulletSpeed = 375.0f;
int g_bulletSpeedDiagLeft = 10;

// Live readout for the "WEAPON:" on-screen indicator below (see hkPresent) - lets the
// user confirm, without digging through rotk_log.txt, whether the weapon-name offset
// chain is actually resolving a name at all and whether it matched a Weapon Profiles
// entry, instead of just observing "the FOV/smoothness didn't seem to change" and not
// knowing which of the two possible causes (name never resolves vs. resolves but this
// particular weapon has no profile row) is responsible. Only updates while the aimbot
// activation key is held, since that's the only time UpdateAimbotBulletSpeed runs.
string g_weaponDiagName = "(hold aim key)";
string g_weaponDiagProfile = "-";

// Reads an entity's currently-held weapon's internal model/asset name (e.g.
// "Weapons_AK47_3P.adr") - entity -> weapon container -> active weapon object -> a
// direct pointer to a null-terminated C string. Confirmed via a classmate's live debugger
// captures against the running game across 4 different weapons (AR15/AK47/Magnum/
// RiotShotgun), all resolving cleanly - replaces two earlier guessed-and-broken chains.
// Returns an empty string if any hop in the chain can't be resolved.
string ReadEntityWeaponName(uint64_t ent, string* diagOut = nullptr)
{
	uint64_t weaponContainer = read<uint64_t>(handle, ent + entityWeaponContainer);
	if (!weaponContainer) { if (diagOut) *diagOut = "entityWeaponContainer null"; return ""; }
	uint64_t activeWeapon = read<uint64_t>(handle, weaponContainer + weaponContainerActive);
	if (!activeWeapon) { if (diagOut) *diagOut = "weaponContainerActive null"; return ""; }
	uint64_t namePtr = read<uint64_t>(handle, activeWeapon + weaponModelNamePtr);
	if (!namePtr) { if (diagOut) *diagOut = "weaponModelNamePtr null"; return ""; }

	char wepBuf[64] = {};
	ReadProcessMemory(handle, (LPCVOID)namePtr, wepBuf, sizeof(wepBuf) - 1, NULL);
	return string(wepBuf);
}

// Maps the raw internal asset name to a short, human-readable label for ESP display -
// same substring set UpdateAimbotBulletSpeed already matches on, so anything it recognizes
// for bullet-speed purposes gets a clean name here too. Falls back to the raw name
// (truncated) for anything unrecognized rather than showing nothing.
string GetWeaponDisplayName(const string& wepStr)
{
	if (wepStr.empty()) return "";
	if (wepStr.find("AR15") != string::npos) return "AR-15";
	if (wepStr.find("AK47") != string::npos) return "AK-47";
	if (wepStr.find("M40Sniper") != string::npos) return "M40 Sniper";
	if (wepStr.find("RanchRifle") != string::npos) return "Ranch Rifle";
	if (wepStr.find("MK46") != string::npos) return "MK46";
	if (wepStr.find("RiotShotgun") != string::npos) return "Riot Shotgun";
	if (wepStr.find("M9") != string::npos) return "M9";
	if (wepStr.find("45Auto") != string::npos) return "1911";
	if (wepStr.find("Magnum") != string::npos) return "Magnum";
	if (wepStr.find("Crossbow") != string::npos) return "Crossbow";
	if (wepStr.find("Bow01") != string::npos) return "Bow";
	string trimmed = wepStr.substr(0, 20);
	return trimmed;
}

// Per-weapon Aimbot FOV/Smoothness, user-editable from the menu's Weapon Profiles panel
// (each row here is one entry in that list) instead of a fixed canned override - keyword
// is the same substring UpdateAimbotBulletSpeed/GetWeaponDisplayName already match the
// internal asset name against, displayName is what the panel shows (independent of
// GetWeaponDisplayName's own label - "308 Hunting Rifle" here vs "M40 Sniper" on the ESP
// box, same weapon). Defaults match the previous hardcoded profile values. MK46, Ranch
// Rifle, Crossbow and Bow have no row here (no per-weapon override to configure) but are
// still full weapons everywhere else (bullet speed, ESP, ground-weapon detection).
struct WeaponProfile {
	const char* keyword;
	const char* displayName;
	float fov;
	float smoothness;
};
WeaponProfile g_weaponProfileTable[] = {
	{ "AR15",        "AR-15",             40.0f, 3.0f },
	{ "AK47",        "AK-47",             40.0f, 3.0f },
	{ "M40Sniper",   "308 Hunting Rifle", 15.0f, 6.0f },
	{ "RiotShotgun", "Riot Shotgun",      70.0f, 1.0f },
	{ "M9",          "M9",                35.0f, 2.0f },
	{ "45Auto",      "1911",              35.0f, 2.0f },
	{ "Magnum",      "Magnum",            35.0f, 2.0f },
};
const int g_weaponProfileCount = sizeof(g_weaponProfileTable) / sizeof(g_weaponProfileTable[0]);

// Forward declaration - defined below with the other name-keyword filters
// (IsArmorName/IsHeGrenadeName), but this needs it earlier: matching the held weapon's
// internal asset name case-sensitively (the old wepStr.find(...) here) missed real
// matches whenever the client's actual casing didn't match the assumed keyword, which
// looked identical to the weapon-name offset simply not resolving. Case-insensitive
// matches this function's own keyword table against, same as every other name filter in
// this file.
bool ContainsIgnoreCase(const char* haystack, const char* needle);

void UpdateAimbotBulletSpeed(uint64_t localPlayerEnt)
{
	g_aimbotBulletSpeed = 375.0f;
	g_effectiveAimbotFov = g_aimbotFov;
	g_effectiveAimbotSmoothness = g_aimbotSmoothness;
	string wepDiag;
	string wepStr = ReadEntityWeaponName(localPlayerEnt, &wepDiag);
	g_weaponDiagName = wepStr.empty() ? wepDiag : wepStr;
	if (wepStr.empty()) { g_weaponDiagProfile = "-"; return; }
	const char* wep = wepStr.c_str();

	if (ContainsIgnoreCase(wep, "AR15")) g_aimbotBulletSpeed = 375.0f;
	else if (ContainsIgnoreCase(wep, "AK47")) g_aimbotBulletSpeed = 375.0f;
	else if (ContainsIgnoreCase(wep, "M40Sniper")) g_aimbotBulletSpeed = 659.0f;
	else if (ContainsIgnoreCase(wep, "RanchRifle")) g_aimbotBulletSpeed = 350.0f;
	else if (ContainsIgnoreCase(wep, "MK46")) g_aimbotBulletSpeed = 250.0f;
	else if (ContainsIgnoreCase(wep, "RiotShotgun")) g_aimbotBulletSpeed = 175.0f;
	else if (ContainsIgnoreCase(wep, "M9")) g_aimbotBulletSpeed = 251.0f;
	else if (ContainsIgnoreCase(wep, "45Auto")) g_aimbotBulletSpeed = 251.0f;
	else if (ContainsIgnoreCase(wep, "Magnum")) g_aimbotBulletSpeed = 251.0f;
	else if (ContainsIgnoreCase(wep, "Crossbow")) g_aimbotBulletSpeed = 120.0f;
	else if (ContainsIgnoreCase(wep, "Bow01")) g_aimbotBulletSpeed = 75.0f;
	// Grenades/fists/binoculars/etc. - aimbot is irrelevant, stays at the AR default.

	// Per-weapon FOV/smoothing, from the user-editable table above instead of a fixed
	// canned override. Left at the slider values (set above) for anything not listed there.
	if (g_weaponProfiles) {
		g_weaponDiagProfile = "none (no match - using sliders)";
		for (int i = 0; i < g_weaponProfileCount; i++) {
			if (ContainsIgnoreCase(wep, g_weaponProfileTable[i].keyword)) {
				g_effectiveAimbotFov = g_weaponProfileTable[i].fov;
				g_effectiveAimbotSmoothness = g_weaponProfileTable[i].smoothness;
				g_weaponDiagProfile = g_weaponProfileTable[i].displayName;
				break;
			}
		}
	} else {
		g_weaponDiagProfile = "disabled";
	}

	if (g_bulletSpeedDiagLeft > 0) {
		char buf[192];
		sprintf_s(buf, "BulletSpeed diag: name=\"%s\" speed=%.1f", wepStr.c_str(), g_aimbotBulletSpeed);
		Log(buf);
		g_bulletSpeedDiagLeft--;
	}
}


// Per-category box colors, freely chosen via an RGB picker in the menu (gui.h's
// ColorPicker widget) instead of cycling a fixed palette - each is just a plain RGB
// triple now, not an index into a shared table.
float g_playerColor[3] = { 0.0f, 1.0f, 0.0f };     // green
float g_itemColor[3] = { 1.0f, 1.0f, 0.0f };       // yellow
float g_offRoaderColor[3] = { 0.1f, 0.6f, 1.0f };  // blue
float g_car2Color[3] = { 1.0f, 0.5f, 0.0f };       // orange
float g_car3Color[3] = { 1.0f, 0.0f, 1.0f };       // magenta
float g_atvColor[3] = { 0.0f, 1.0f, 0.0f };        // green
float g_lootbagColor[3] = { 1.0f, 0.0f, 1.0f };    // magenta
float g_grenadeColor[3] = { 1.0f, 0.4f, 0.0f };    // orange
float g_weaponEspColor[3] = { 0.0f, 0.8f, 1.0f };  // light blue
float g_headDotColor[3] = { 1.0f, 0.0f, 0.0f };    // red
float g_skeletonColor[3] = { 0.2f, 1.0f, 1.0f };   // cyan

void SaveConfigAction();  // defined near SaveConfig(), below
void RequestUnloadAction();  // defined near hkPresent, below

// SafeMode: off by default (matches all prior behavior out of the box). When ON, the
// three riskiest/most detectable features (NoClip, SpeedHack, GhostMode) can't be turned
// on via the menu - ENTER on those rows is a no-op instead, with an on-screen message
// saying why. Turning SafeMode ON also force-disables any of the three that are already
// running, so it actually means "off" rather than just "can't be turned on from here."
bool g_safeMode = false;

// INSERT-toggled menu state (g_menuOpen itself is declared near the top of the file,
// with the other early D3D11/input hook globals - hkWndProc/hkSetCursorPos/hkClipCursor
// all need it and are defined before this point). The menu itself is drawn by g_gui
// (ImGui, see gui.h/gui.cpp) instead of the old hand-rolled DrawMenu()/MenuItem list -
// each toggle/color/keybind is wired directly to its widget in gui.cpp.
bool g_insertWasDown = false;
int g_savedFlashFrames = 0;

bool IsGroundItemType(int type)
{
	for (int t : groundItemEntityTypes) {
		if (t == type) return true;
	}
	return false;
}

bool IsVehicleType(int type)
{
	for (int t : vehicleEntityTypes) {
		if (t == type) return true;
	}
	return false;
}

// Per-vehicle-type color, customizable via the menu's OffRoader/Car2/Car3/ATV rows
// (vehicleEntityTypes: OffRoader, CAR_2, CAR_3, ATV).
void GetVehicleColor(int type, float& r, float& g, float& b)
{
	const float* c;
	switch (type) {
	case 17:  c = g_offRoaderColor; break;
	case 114: c = g_car2Color; break;
	case 118: c = g_car3Color; break;
	case 204: c = g_atvColor; break;
	default:  r = 1.0f; g = 1.0f; b = 1.0f; return;  // fallback - white
	}
	r = c[0]; g = c[1]; b = c[2];
}

// Case-insensitive substring check, since the reference offsets have no distinct
// entity Type for "body armor" specifically - it's a generic TYPE_ITEM like everything
// else, so armor has to be identified by its name string instead.
bool ContainsIgnoreCase(const char* haystack, const char* needle)
{
	if (!haystack || !needle) return false;
	size_t hLen = strlen(haystack), nLen = strlen(needle);
	if (nLen == 0 || nLen > hLen) return false;
	for (size_t i = 0; i + nLen <= hLen; i++) {
		size_t j = 0;
		for (; j < nLen; j++) {
			if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) break;
		}
		if (j == nLen) return true;
	}
	return false;
}

string ToLowerName(const string& name)
{
	string result = name;
	for (char& c : result) {
		if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	}
	return result;
}

bool IsFriendName(const string& name)
{
	if (name.empty()) return false;
	string key = ToLowerName(name);
	for (const auto& f : g_friendNames) {
		if (ToLowerName(f) == key) return true;
	}
	return false;
}

void LoadFriendList()
{
	g_friendNames.clear();
	if (g_friendListPath[0] != '\0') {
		std::ifstream in(g_friendListPath);
		string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty() && !IsFriendName(line)) g_friendNames.push_back(line);
		}
	}
	g_friendListLoaded = true;

	char buf[64];
	sprintf_s(buf, "LoadFriendList: loaded %zu name(s)", g_friendNames.size());
	Log(buf);
}

void SaveFriendList()
{
	if (g_friendListPath[0] == '\0') return;
	std::ofstream out(g_friendListPath, std::ios::trunc);
	for (const auto& n : g_friendNames) out << n << "\n";
}

// Fails closed - an empty/unread name is treated as excluded rather than risk targeting
// an actual friend whose name just didn't resolve this particular frame.
bool AddFriendName(const string& name)
{
	if (name.empty() || IsFriendName(name)) return false;
	g_friendNames.push_back(name);
	SaveFriendList();

	g_friendNotice = "Added " + name + " - Aimbot/Magic Bullet will ignore them";
	g_friendNoticeUntil = GetTickCount64() + 5000;
	char buf[192];
	sprintf_s(buf, "FriendList: added \"%s\"", name.c_str());
	Log(buf);
	return true;
}

bool RemoveFriendName(const string& name)
{
	string key = ToLowerName(name);
	for (size_t i = 0; i < g_friendNames.size(); i++) {
		if (ToLowerName(g_friendNames[i]) == key) {
			g_friendNotice = "Removed " + g_friendNames[i] + " from Friend List";
			g_friendNoticeUntil = GetTickCount64() + 5000;
			char buf[192];
			sprintf_s(buf, "FriendList: removed \"%s\"", g_friendNames[i].c_str());
			Log(buf);
			g_friendNames.erase(g_friendNames.begin() + i);
			SaveFriendList();
			return true;
		}
	}
	return false;
}

bool IsArmorName(const char* name)
{
	return ContainsIgnoreCase(name, "Armor") || ContainsIgnoreCase(name, "Vest") || ContainsIgnoreCase(name, "Kevlar");
}

// Entities at grenadeEntityType cover every grenade type (HE, smoke, flash, etc.), not
// just HE - narrowing to HE specifically by name, same pattern as IsArmorName. Exact
// asset-name substrings not independently verified.
bool IsHeGrenadeName(const char* name)
{
	return ContainsIgnoreCase(name, "HEGrenade") || ContainsIgnoreCase(name, "Grenade_HE")
		|| ContainsIgnoreCase(name, "FragGrenade") || ContainsIgnoreCase(name, "M67")
		|| (ContainsIgnoreCase(name, "Grenade") && ContainsIgnoreCase(name, "HE"));
}

// Bone indices for the fixed-size array below - matches the order/selection in Offs.h's
// kHeadBoneIdx/etc, just renumbered 0..15 for local array indexing instead of the raw
// joint-array indices those constants hold.
enum SkelBone {
	SKEL_HEAD, SKEL_NECK, SKEL_CHEST, SKEL_SPINE, SKEL_PELVIS,
	SKEL_L_SHOULDER, SKEL_L_ELBOW, SKEL_L_HAND,
	SKEL_R_SHOULDER, SKEL_R_ELBOW, SKEL_R_HAND,
	SKEL_L_HIP, SKEL_L_KNEE, SKEL_L_FOOT,
	SKEL_R_HIP, SKEL_R_KNEE, SKEL_R_FOOT,
	SKEL_BONE_COUNT
};

// Which two SkelBone entries each drawn line connects.
static const int kSkeletonBoneConnections[][2] = {
	{ SKEL_PELVIS, SKEL_SPINE }, { SKEL_SPINE, SKEL_CHEST }, { SKEL_CHEST, SKEL_NECK }, { SKEL_NECK, SKEL_HEAD },
	{ SKEL_CHEST, SKEL_L_SHOULDER }, { SKEL_L_SHOULDER, SKEL_L_ELBOW }, { SKEL_L_ELBOW, SKEL_L_HAND },
	{ SKEL_CHEST, SKEL_R_SHOULDER }, { SKEL_R_SHOULDER, SKEL_R_ELBOW }, { SKEL_R_ELBOW, SKEL_R_HAND },
	{ SKEL_PELVIS, SKEL_L_HIP }, { SKEL_L_HIP, SKEL_L_KNEE }, { SKEL_L_KNEE, SKEL_L_FOOT },
	{ SKEL_PELVIS, SKEL_R_HIP }, { SKEL_R_HIP, SKEL_R_KNEE }, { SKEL_R_KNEE, SKEL_R_FOOT },
};

D3DXVECTOR3 GetBoneWorld(uint64_t jointData, size_t boneIdx, const D3DXVECTOR3& feetPos, float yaw)
{
	D3DXVECTOR3 local = read<D3DXVECTOR3>(handle, jointData + boneIdx * skeletonBoneStride);

	// rotate the bone's local-space offset into world space by the player's yaw - skip
	// this and bones break on any facing direction other than whatever they were
	// recorded at
	float s = sinf(yaw), c = cosf(yaw);
	return D3DXVECTOR3(
		feetPos.x + local.x * c - local.z * s,
		feetPos.y + local.y,
		feetPos.z + local.x * s + local.z * c
	);
}

// Distance-from-feet instead of a flat |v|<15000 check on the raw world coordinate: this
// game's world coordinates already sit in the thousands, so a bone 50+ meters from the
// player's own body would still pass a flat magnitude check. A real bone has to be
// within a couple meters of the player's feet. isfinite() added on top to reject NaN/Inf.
bool IsValidBone(const D3DXVECTOR3& v, const D3DXVECTOR3& feet)
{
	if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return false;
	D3DXVECTOR3 d = v - feet;
	float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
	return distSq < (3.0f * 3.0f);
}

// Converts an NDC point back to pixel coordinates, for positioning text near a box
// that was placed via world-to-NDC projection.
void NdcToPixel(float ndcX, float ndcY, float screenW, float screenH, float& outPxX, float& outPxY)
{
	outPxX = (ndcX + 1.0f) * 0.5f * screenW;
	outPxY = (1.0f - ndcY) * 0.5f * screenH;
}

// Aimbot FOV ring - a fixed pixel radius around the crosshair, converted to NDC so it
// still draws a circle (not an ellipse) despite the non-square NDC-to-pixel scale.
void QueueFovCirclePx(float radiusPx, float r, float g, float b, float a, float screenW, float screenH)
{
	const int segments = 48;
	for (int i = 0; i <= segments; i++) {
		float angle = (float)i / (float)segments * 2.0f * (float)M_PI;
		float pxX = screenW / 2.0f + cosf(angle) * radiusPx;
		float pxY = screenH / 2.0f + sinf(angle) * radiusPx;
		float ndcX = (pxX / screenW) * 2.0f - 1.0f;
		float ndcY = 1.0f - (pxY / screenH) * 2.0f;
		g_circleVerts.push_back({ ndcX, ndcY, 0, r, g, b, a });
	}
}

void QueueESPBoxNdc(float cx, float cy, float halfW, float halfH, float r, float g, float b, float a)
{
	float x0 = cx - halfW, x1 = cx + halfW;
	float y0 = cy - halfH, y1 = cy + halfH;
	NdcVertex corners[5] = {
		{ x0, y0, 0, r, g, b, a },
		{ x1, y0, 0, r, g, b, a },
		{ x1, y1, 0, r, g, b, a },
		{ x0, y1, 0, r, g, b, a },
		{ x0, y0, 0, r, g, b, a },
	};
	for (int i = 0; i < 5; i++) g_frameVerts.push_back(corners[i]);
}

// Filled quads (menu panel background/highlight) - TRIANGLELIST, separate from the
// LINESTRIP box outlines above, so they get their own draw call with different topology.
vector<NdcVertex> g_fillVerts;

void QueueFilledQuadNdc(float cx, float cy, float halfW, float halfH, float r, float g, float b, float a)
{
	float x0 = cx - halfW, x1 = cx + halfW;
	float y0 = cy - halfH, y1 = cy + halfH;
	NdcVertex tris[6] = {
		{ x0, y0, 0, r, g, b, a },
		{ x1, y0, 0, r, g, b, a },
		{ x1, y1, 0, r, g, b, a },
		{ x0, y0, 0, r, g, b, a },
		{ x1, y1, 0, r, g, b, a },
		{ x0, y1, 0, r, g, b, a },
	};
	for (int i = 0; i < 6; i++) g_fillVerts.push_back(tris[i]);
}

// Pixel-space (like the FOV circle helper) rather than NDC-space - a fixed NDC radius
// would draw as an ellipse, not a circle, since NDC x/y map to width/height independently.
void QueueFilledCirclePx(float pxX, float pxY, float radiusPx, float r, float g, float b, float a, float screenW, float screenH)
{
	const int segments = 16;
	float centerNdcX = (pxX / screenW) * 2.0f - 1.0f;
	float centerNdcY = 1.0f - (pxY / screenH) * 2.0f;
	for (int i = 0; i < segments; i++) {
		float angle0 = (float)i / (float)segments * 2.0f * (float)M_PI;
		float angle1 = (float)(i + 1) / (float)segments * 2.0f * (float)M_PI;
		float p0x = (pxX + cosf(angle0) * radiusPx) / screenW * 2.0f - 1.0f;
		float p0y = 1.0f - (pxY + sinf(angle0) * radiusPx) / screenH * 2.0f;
		float p1x = (pxX + cosf(angle1) * radiusPx) / screenW * 2.0f - 1.0f;
		float p1y = 1.0f - (pxY + sinf(angle1) * radiusPx) / screenH * 2.0f;
		NdcVertex tri[3] = {
			{ centerNdcX, centerNdcY, 0, r, g, b, a },
			{ p0x, p0y, 0, r, g, b, a },
			{ p1x, p1y, 0, r, g, b, a },
		};
		for (int j = 0; j < 3; j++) g_fillVerts.push_back(tri[j]);
	}
}

// Same as QueueFilledQuadNdc but takes a pixel-space top-left rect, matching how
// QueueText positions things, instead of NDC center+half-extent.
void QueueFilledRectPx(float pxX, float pxY, float pxW, float pxH, float r, float g, float b, float a, float screenW, float screenH)
{
	float cxPx = pxX + pxW / 2.0f;
	float cyPx = pxY + pxH / 2.0f;
	float ndcCx = (cxPx / screenW) * 2.0f - 1.0f;
	float ndcCy = 1.0f - (cyPx / screenH) * 2.0f;
	float ndcHalfW = (pxW / screenW);
	float ndcHalfH = (pxH / screenH);
	QueueFilledQuadNdc(ndcCx, ndcCy, ndcHalfW, ndcHalfH, r, g, b, a);
}

vector<TexVertex> g_textVerts;

// Queues a string at a pixel-space top-left position (x,y). Uses fixed monospace
// advance (the atlas font is FF_MODERN/FIXED_PITCH), converting each glyph quad from
// pixel coordinates to NDC as it goes.
void QueueText(float topLeftX, float topLeftY, const char* text, float r, float g, float b, float a, float screenW, float screenH)
{
	if (!g_fontReady) return;

	float penX = topLeftX;
	for (const char* c = text; *c; c++) {
		if (*c == ' ') {
			penX += g_fontAdvanceW;
			continue;
		}
		if (*c < FONT_FIRST_CHAR || *c > FONT_LAST_CHAR) continue;

		const FontChar& fc = g_fontChars[*c - FONT_FIRST_CHAR];
		float px0 = penX, px1 = penX + g_fontCellW;
		float py0 = topLeftY, py1 = topLeftY + g_fontCellH;

		float x0 = (px0 / screenW) * 2.0f - 1.0f;
		float x1 = (px1 / screenW) * 2.0f - 1.0f;
		float y0 = 1.0f - (py0 / screenH) * 2.0f;
		float y1 = 1.0f - (py1 / screenH) * 2.0f;

		TexVertex quad[6] = {
			{ x0, y0, 0, fc.u0, fc.v0, r, g, b, a },
			{ x1, y0, 0, fc.u1, fc.v0, r, g, b, a },
			{ x1, y1, 0, fc.u1, fc.v1, r, g, b, a },
			{ x0, y0, 0, fc.u0, fc.v0, r, g, b, a },
			{ x1, y1, 0, fc.u1, fc.v1, r, g, b, a },
			{ x0, y1, 0, fc.u0, fc.v1, r, g, b, a },
		};
		for (int i = 0; i < 6; i++) g_textVerts.push_back(quad[i]);

		penX += g_fontAdvanceW;
	}
}

// ============================================================
// Config persistence - saves/loads menu settings to named .ini files under
// Documents\Stan\Configs\ so they survive between injections, and so more than one
// loadout can be kept around (e.g. a Combat-only config, a stream-safe ESP-only config)
// instead of everything overwriting the same single file. Documents\Stan\lastconfig.txt
// remembers which one was active, so the same config loads back in on the next launch.
// Placed before the gui.cpp include below (rather than with SaveConfig/LoadConfig's full
// definitions, further down) because the Settings tab references these globals/functions
// directly, same reasoning as the "needs direct access" comment on that include.
// ============================================================

char g_configPath[MAX_PATH] = {};             // full path to the currently ACTIVE config file
char g_configFolder[MAX_PATH] = {};           // Documents\Stan\Configs
char g_lastConfigPointerPath[MAX_PATH] = {};  // Documents\Stan\lastconfig.txt
char g_activeConfigName[64] = "default";
vector<string> g_configNames;                 // refreshed by RefreshConfigList()
char g_newConfigNameBuf[64] = {};             // GUI text-input scratch buffer, see gui.cpp

void SaveConfig();  // forward decl - full definition is below the gui.cpp include, since
void LoadConfig();  // it also needs g_configPath/etc.; forward decl is enough for callers here

void BuildConfigPath(const char* name, char* outPath, size_t outSize)
{
	sprintf_s(outPath, outSize, "%s\\%s.ini", g_configFolder, name);
}

// Filesystem-invalid characters replaced with '_' so a typed config name can never
// produce a bad path or accidentally escape the Configs folder (e.g. "..").
void SanitizeConfigName(char* name)
{
	for (char* p = name; *p; p++) {
		if (strchr("\\/:*?\"<>|", *p)) *p = '_';
	}
}

void RefreshConfigList()
{
	g_configNames.clear();
	char searchPath[MAX_PATH];
	sprintf_s(searchPath, "%s\\*.ini", g_configFolder);
	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPath, &findData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				string fname = findData.cFileName;
				size_t dot = fname.rfind(".ini");
				if (dot != string::npos) fname = fname.substr(0, dot);
				if (!fname.empty()) g_configNames.push_back(fname);
			}
		} while (FindNextFileA(hFind, &findData));
		FindClose(hFind);
	}
	std::sort(g_configNames.begin(), g_configNames.end());
}

void WriteLastConfigPointer()
{
	std::ofstream out(g_lastConfigPointerPath, std::ios::trunc);
	if (out.is_open()) out << g_activeConfigName;
}

// Switches the active config and loads it - used both by the config list's "load" click
// and internally (e.g. falling back to "default" after deleting the active config).
void SwitchToConfig(const char* name)
{
	strcpy_s(g_activeConfigName, name);
	BuildConfigPath(name, g_configPath, sizeof(g_configPath));
	LoadConfig();
	WriteLastConfigPointer();
}

// "Save As New": saves the CURRENT live settings under a new name and switches to it,
// rather than starting the new config blank - matches how most games' loadout/profile
// systems work (duplicate-and-rename), and means creating a variant of your current
// setup doesn't mean reconfiguring everything from scratch.
void SaveConfigAsNew(const char* name)
{
	if (!name || !name[0]) return;
	strcpy_s(g_activeConfigName, name);
	BuildConfigPath(name, g_configPath, sizeof(g_configPath));
	SaveConfig();
	WriteLastConfigPointer();
	RefreshConfigList();
}

void DeleteConfigByName(const char* name)
{
	char path[MAX_PATH];
	BuildConfigPath(name, path, sizeof(path));
	DeleteFileA(path);
	bool wasActive = strcmp(g_activeConfigName, name) == 0;
	RefreshConfigList();
	// Deleting the active config would otherwise leave g_configPath pointing at a file
	// that no longer exists - fall back to "default" (created fresh on next Save if it
	// doesn't already exist) instead of silently saving nothing on the next Save Config.
	if (wasActive) SwitchToConfig("default");
}

void InitConfigPath()
{
	char documentsPath[MAX_PATH] = {};
	if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath) != S_OK) {
		Log("InitConfigPath: SHGetFolderPathA failed");
		return;
	}
	char folderPath[MAX_PATH] = {};
	sprintf_s(folderPath, "%s\\Stan", documentsPath);
	CreateDirectoryA(folderPath, NULL);  // fine if it already exists
	sprintf_s(g_configFolder, "%s\\Configs", folderPath);
	CreateDirectoryA(g_configFolder, NULL);
	sprintf_s(g_friendListPath, "%s\\friends.txt", folderPath);
	sprintf_s(g_lastConfigPointerPath, "%s\\lastconfig.txt", folderPath);

	// One-time migration: a pre-existing single stan.ini (from before named configs
	// existed) becomes Configs\default.ini instead of being silently orphaned/ignored.
	{
		char oldSingleConfigPath[MAX_PATH];
		sprintf_s(oldSingleConfigPath, "%s\\stan.ini", folderPath);
		char defaultConfigPath[MAX_PATH];
		BuildConfigPath("default", defaultConfigPath, sizeof(defaultConfigPath));
		if (GetFileAttributesA(oldSingleConfigPath) != INVALID_FILE_ATTRIBUTES &&
			GetFileAttributesA(defaultConfigPath) == INVALID_FILE_ATTRIBUTES) {
			CopyFileA(oldSingleConfigPath, defaultConfigPath, FALSE);
		}
	}

	strcpy_s(g_activeConfigName, "default");
	std::ifstream lastConfigIn(g_lastConfigPointerPath);
	if (lastConfigIn.is_open()) {
		string lastName;
		std::getline(lastConfigIn, lastName);
		if (!lastName.empty() && lastName.size() < sizeof(g_activeConfigName)) {
			strcpy_s(g_activeConfigName, lastName.c_str());
		}
	}
	BuildConfigPath(g_activeConfigName, g_configPath, sizeof(g_configPath));
	RefreshConfigList();

	char buf[256];
	sprintf_s(buf, "InitConfigPath: active=%s path=%s", g_activeConfigName, g_configPath);
	Log(buf);

	LoadFriendList();
}

// The ImGui menu (Gui class, gui.h) needs direct access to every g_show*/g_aimbot/color
// toggle above plus GetKeyName()/SaveConfigAction()/RequestUnloadAction() - all already
// declared by this point - so it's #include'd as part of this same translation unit
// rather than compiled as its own separate .cpp with an extern boundary.
#include "gui.cpp"

void SaveConfig()
{
	if (g_configPath[0] == '\0') return;

	auto writeBool = [](const char* key, bool value) {
		WritePrivateProfileStringA("Settings", key, value ? "1" : "0", g_configPath);
	};
	auto writeInt = [](const char* key, int value) {
		char buf[16];
		sprintf_s(buf, "%d", value);
		WritePrivateProfileStringA("Settings", key, buf, g_configPath);
	};
	auto writeFloat = [](const char* key, float value) {
		char buf[32];
		sprintf_s(buf, "%g", value);
		WritePrivateProfileStringA("Settings", key, buf, g_configPath);
	};
	auto writeColor = [](const char* keyPrefix, const float* color) {
		char key[64], buf[32];
		for (int i = 0; i < 3; i++) {
			sprintf_s(key, "%s%c", keyPrefix, "RGB"[i]);
			sprintf_s(buf, "%g", color[i]);
			WritePrivateProfileStringA("Settings", key, buf, g_configPath);
		}
	};

	writeBool("ShowPlayerBoxes", g_showPlayerBoxes);
	writeBool("ShowItemESP", g_showItemESP);
	writeBool("ShowPlayerNames", g_showPlayerNames);
	writeBool("ShowVehicleESP", g_showVehicleESP);
	writeBool("ShowLootbags", g_showLootbags);
	writeBool("ShowGrenades", g_showGrenades);
	writeBool("ShowGroundWeapons", g_showGroundWeapons);
	writeBool("ShowHeadDot", g_showHeadDot);
	writeBool("ShowSkeleton", g_showSkeleton);
	writeBool("ShowHealthBars", g_showHealthBars);
	writeBool("ShowDistance", g_showDistance);
	writeBool("SafeMode", g_safeMode);
	writeBool("NoRecoil", g_noRecoil);
	writeBool("Aimbot", g_aimbot);
	writeInt("AimbotKey", g_aimbotKey);
	writeInt("AimbotToggleKey", g_aimbotToggleKey);
	writeInt("NoRecoilToggleKey", g_noRecoilToggleKey);
	writeFloat("AimbotFov", g_aimbotFov);
	writeBool("ShowFovCircle", g_showFovCircle);
	writeFloat("AimbotSmoothness", g_aimbotSmoothness);
	writeBool("WeaponProfiles", g_weaponProfiles);
	writeBool("FriendListEnabled", g_friendListEnabled);
	writeInt("AddFriendKey", g_addFriendKey);
	writeColor("PlayerColor", g_playerColor);
	writeColor("ItemColor", g_itemColor);
	writeColor("OffRoaderColor", g_offRoaderColor);
	writeColor("Car2Color", g_car2Color);
	writeColor("Car3Color", g_car3Color);
	writeColor("AtvColor", g_atvColor);
	writeColor("LootbagColor", g_lootbagColor);
	writeColor("GrenadeColor", g_grenadeColor);
	writeColor("WeaponEspColor", g_weaponEspColor);
	writeColor("HeadDotColor", g_headDotColor);
	writeColor("SkeletonColor", g_skeletonColor);

	for (int i = 0; i < g_weaponProfileCount; i++) {
		char fovKey[48], smKey[48];
		sprintf_s(fovKey, "WeaponProfileFov_%s", g_weaponProfileTable[i].keyword);
		sprintf_s(smKey, "WeaponProfileSm_%s", g_weaponProfileTable[i].keyword);
		writeFloat(fovKey, g_weaponProfileTable[i].fov);
		writeFloat(smKey, g_weaponProfileTable[i].smoothness);
	}

	char saveLogBuf[256];
	sprintf_s(saveLogBuf, "SaveConfig: saved to %s", g_configPath);
	Log(saveLogBuf);
}

// Menu-row wrapper for SaveConfig() - also flashes an on-screen confirmation
void SaveConfigAction()
{
	SaveConfig();
	g_savedFlashFrames = 90;
}

void LoadConfig()
{
	if (g_configPath[0] == '\0') return;

	// GetPrivateProfileInt returns the default (our current value) if the file or key
	// doesn't exist yet, so this is safe to call even before the file has ever been saved.
	g_showPlayerBoxes = GetPrivateProfileIntA("Settings", "ShowPlayerBoxes", g_showPlayerBoxes ? 1 : 0, g_configPath) != 0;
	g_showItemESP = GetPrivateProfileIntA("Settings", "ShowItemESP", g_showItemESP ? 1 : 0, g_configPath) != 0;
	g_showPlayerNames = GetPrivateProfileIntA("Settings", "ShowPlayerNames", g_showPlayerNames ? 1 : 0, g_configPath) != 0;
	g_showVehicleESP = GetPrivateProfileIntA("Settings", "ShowVehicleESP", g_showVehicleESP ? 1 : 0, g_configPath) != 0;
	g_showLootbags = GetPrivateProfileIntA("Settings", "ShowLootbags", g_showLootbags ? 1 : 0, g_configPath) != 0;
	g_showGrenades = GetPrivateProfileIntA("Settings", "ShowGrenades", g_showGrenades ? 1 : 0, g_configPath) != 0;
	g_showGroundWeapons = GetPrivateProfileIntA("Settings", "ShowGroundWeapons", g_showGroundWeapons ? 1 : 0, g_configPath) != 0;
	g_showHeadDot = GetPrivateProfileIntA("Settings", "ShowHeadDot", g_showHeadDot ? 1 : 0, g_configPath) != 0;
	g_showSkeleton = GetPrivateProfileIntA("Settings", "ShowSkeleton", g_showSkeleton ? 1 : 0, g_configPath) != 0;
	g_showHealthBars = GetPrivateProfileIntA("Settings", "ShowHealthBars", g_showHealthBars ? 1 : 0, g_configPath) != 0;
	g_showDistance = GetPrivateProfileIntA("Settings", "ShowDistance", g_showDistance ? 1 : 0, g_configPath) != 0;
	g_safeMode = GetPrivateProfileIntA("Settings", "SafeMode", g_safeMode ? 1 : 0, g_configPath) != 0;
	g_noRecoil = GetPrivateProfileIntA("Settings", "NoRecoil", g_noRecoil ? 1 : 0, g_configPath) != 0;
	g_aimbot = GetPrivateProfileIntA("Settings", "Aimbot", g_aimbot ? 1 : 0, g_configPath) != 0;
	g_showFovCircle = GetPrivateProfileIntA("Settings", "ShowFovCircle", g_showFovCircle ? 1 : 0, g_configPath) != 0;
	g_weaponProfiles = GetPrivateProfileIntA("Settings", "WeaponProfiles", g_weaponProfiles ? 1 : 0, g_configPath) != 0;
	g_friendListEnabled = GetPrivateProfileIntA("Settings", "FriendListEnabled", g_friendListEnabled ? 1 : 0, g_configPath) != 0;
	g_addFriendKey = GetPrivateProfileIntA("Settings", "AddFriendKey", g_addFriendKey, g_configPath);
	{
		char buf[32], defaultBuf[32];
		sprintf_s(defaultBuf, "%g", g_aimbotFov);
		GetPrivateProfileStringA("Settings", "AimbotFov", defaultBuf, buf, sizeof(buf), g_configPath);
		float v = (float)atof(buf);
		if (v >= 5.0f && v <= 150.0f) g_aimbotFov = v;  // ignore a corrupted/out-of-range ini value
	}
	{
		char buf[32], defaultBuf[32];
		sprintf_s(defaultBuf, "%g", g_aimbotSmoothness);
		GetPrivateProfileStringA("Settings", "AimbotSmoothness", defaultBuf, buf, sizeof(buf), g_configPath);
		float v = (float)atof(buf);
		if (v >= 1.0f && v <= 15.0f) g_aimbotSmoothness = v;  // ignore a corrupted/out-of-range ini value
	}
	auto readColor = [](const char* keyPrefix, float* color) {
		char key[64], buf[32], defaultBuf[32];
		for (int i = 0; i < 3; i++) {
			sprintf_s(key, "%s%c", keyPrefix, "RGB"[i]);
			sprintf_s(defaultBuf, "%g", color[i]);
			GetPrivateProfileStringA("Settings", key, defaultBuf, buf, sizeof(buf), g_configPath);
			float v = (float)atof(buf);
			if (v >= 0.0f && v <= 1.0f) color[i] = v;  // ignore a corrupted/out-of-range ini value
		}
	};

	g_aimbotKey = GetPrivateProfileIntA("Settings", "AimbotKey", g_aimbotKey, g_configPath);
	g_aimbotToggleKey = GetPrivateProfileIntA("Settings", "AimbotToggleKey", g_aimbotToggleKey, g_configPath);
	g_noRecoilToggleKey = GetPrivateProfileIntA("Settings", "NoRecoilToggleKey", g_noRecoilToggleKey, g_configPath);

	for (int i = 0; i < g_weaponProfileCount; i++) {
		char fovKey[48], smKey[48], buf[32], defaultBuf[32];

		sprintf_s(fovKey, "WeaponProfileFov_%s", g_weaponProfileTable[i].keyword);
		sprintf_s(defaultBuf, "%g", g_weaponProfileTable[i].fov);
		GetPrivateProfileStringA("Settings", fovKey, defaultBuf, buf, sizeof(buf), g_configPath);
		float fovVal = (float)atof(buf);
		if (fovVal >= 5.0f && fovVal <= 150.0f) g_weaponProfileTable[i].fov = fovVal;

		sprintf_s(smKey, "WeaponProfileSm_%s", g_weaponProfileTable[i].keyword);
		sprintf_s(defaultBuf, "%g", g_weaponProfileTable[i].smoothness);
		GetPrivateProfileStringA("Settings", smKey, defaultBuf, buf, sizeof(buf), g_configPath);
		float smVal = (float)atof(buf);
		if (smVal >= 1.0f && smVal <= 15.0f) g_weaponProfileTable[i].smoothness = smVal;
	}

	// If a saved config has SafeMode on alongside one of the gated features also on (e.g.
	// an older config saved before SafeMode existed), enforce the gate immediately on load
	// rather than leaving a contradictory state until the user next touches the menu.
	if (g_safeMode) {
		g_noRecoil = false;
	}

	readColor("PlayerColor", g_playerColor);
	readColor("ItemColor", g_itemColor);
	readColor("OffRoaderColor", g_offRoaderColor);
	readColor("Car2Color", g_car2Color);
	readColor("Car3Color", g_car3Color);
	readColor("AtvColor", g_atvColor);
	readColor("LootbagColor", g_lootbagColor);
	readColor("GrenadeColor", g_grenadeColor);
	readColor("WeaponEspColor", g_weaponEspColor);
	readColor("HeadDotColor", g_headDotColor);
	readColor("SkeletonColor", g_skeletonColor);

	char loadLogBuf[256];
	sprintf_s(loadLogBuf, "LoadConfig: loaded from %s", g_configPath);
	Log(loadLogBuf);
}

// Unload: cleanly unhooks everything and frees this DLL from the process, without
// waiting for the whole game to close. All the unhooking/cleanup below runs synchronously
// on the game's own render thread - we're already on it here, inside hkPresent, called
// from the menu's ENTER handler - which is safe, since the DLL is still fully loaded and
// mapped for the rest of this function's execution. Only the actual FreeLibrary call has
// to be deferred to a separate thread with a grace period: unloading while our own code
// is still on the call stack (this very hkPresent invocation, which still has to return
// through oPresent() below) would unmap the code we're currently executing.
DWORD WINAPI FinishUnloadThread(LPVOID)
{
	Sleep(500);  // let this last hkPresent call fully return on the render thread first
	Log("Unload: freeing library");
	if (g_hModule) {
		FreeLibraryAndExitThread(g_hModule, 0);
	}
	return 0;
}

void PerformUnload()
{
	Log("Unload: restoring hooks and patches...");

	if (g_pSwapChainCached && oPresent) {
		void** vTable = *reinterpret_cast<void***>(g_pSwapChainCached);
		DWORD oldProtect;
		VirtualProtect(&vTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
		vTable[8] = (void*)oPresent;
		VirtualProtect(&vTable[8], sizeof(void*), oldProtect, &oldProtect);
	}

	// Restore the game's own WndProc and shut down ImGui - same reasoning as the vtable
	// unhooks above: nothing else will call into hkWndProc/g_gui after this point, so
	// it's safe to tear down now, synchronously, before the deferred FreeLibrary.
	if (g_guiInitialized) {
		g_gui.Shutdown();
		g_guiInitialized = false;
	}
	if (g_gameHwnd && g_originalWndProc) {
		SetWindowLongPtr(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
	}
	if (g_moduleBase) {
		if (oSetCursorPos) HookIAT(g_moduleBase, "user32.dll", "SetCursorPos", (void*)oSetCursorPos);
		if (oClipCursor) HookIAT(g_moduleBase, "user32.dll", "ClipCursor", (void*)oClipCursor);
	}
	if (g_menuOpen) {
		ShowCursor(FALSE);  // don't leave the cursor stuck visible after unload
	}

	if (g_aimPunchPatched) {
		ApplyAimPunchPatch(false);
	}

	Log("Unload: hooks restored, scheduling library free");
	CreateThread(NULL, 0, FinishUnloadThread, NULL, 0, NULL);
}

void RequestUnloadAction()
{
	g_unloadRequested = true;
}

HRESULT APIENTRY hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	static bool loggedFirstFrame = false;
	if (!loggedFirstFrame) {
		Log("hkPresent: first hooked frame!");
		loggedFirstFrame = true;
	}

	g_pSwapChainCached = pSwapChain;

	if (!g_d3d11Ready) {
		InitD3D11Resources(pSwapChain);
	}

	static int diagFramesLeft = 5;
	char diagBuf[256];

	DXGI_SWAP_CHAIN_DESC menuScDesc;
	pSwapChain->GetDesc(&menuScDesc);
	float menuScreenW = (float)menuScDesc.BufferDesc.Width;
	float menuScreenH = (float)menuScDesc.BufferDesc.Height;

	if (g_d3d11Ready) {
		g_frameVerts.clear();
		g_circleVerts.clear();
		g_skeletonVerts.clear();
		g_textVerts.clear();
		g_fillVerts.clear();

		// Edge-detected input (not a blocking Sleep()-based debounce like the reference
		// used, since this runs on the render thread every frame)
		bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
		if (insertDown && !g_insertWasDown) {
			g_menuOpen = !g_menuOpen;
			ShowCursor(g_menuOpen);  // usable mouse for the menu; hidden again once closed
			if (!g_menuOpen) g_awaitingKeybindTarget = nullptr;  // closing cancels any pending rebind
		}
		g_insertWasDown = insertDown;

		// Toggle hotkeys - work whether the menu is open or closed, and skipped while
		// actively rebinding a key so the keypress that finishes a rebind can't also
		// double as a toggle this same frame.
		if (!g_awaitingKeybindTarget) {
			bool aimbotToggleDown = (GetAsyncKeyState(g_aimbotToggleKey) & 0x8000) != 0;
			if (aimbotToggleDown && !g_aimbotToggleKeyWasDown) g_aimbot = !g_aimbot;
			g_aimbotToggleKeyWasDown = aimbotToggleDown;

			bool noRecoilToggleDown = (GetAsyncKeyState(g_noRecoilToggleKey) & 0x8000) != 0;
			if (noRecoilToggleDown && !g_noRecoilToggleKeyWasDown && !g_safeMode) g_noRecoil = !g_noRecoil;
			g_noRecoilToggleKeyWasDown = noRecoilToggleDown;
		}

		// Key-rebind capture: only while the menu's open and a KeyBindButton armed it.
		// INSERT (menu toggle) and ESC (cancel) are reserved/excluded from capture. The
		// click that armed this is itself a VK_LBUTTON press still physically held down
		// on this same/next frame - scanning immediately would capture that same click
		// as "MOUSE1" instead of waiting for a deliberate key press, so nothing gets
		// captured until the mouse button that clicked the button has been released.
		if (g_menuOpen && g_awaitingKeybindTarget) {
			bool armingClickStillDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
				g_awaitingKeybindTarget = nullptr;
			} else if (!armingClickStillDown) {
				for (int vk = 1; vk < 255; vk++) {
					if (vk == VK_INSERT || vk == VK_ESCAPE) continue;
					if (GetAsyncKeyState(vk) & 0x8000) {
						*g_awaitingKeybindTarget = vk;
						g_awaitingKeybindTarget = nullptr;
						break;
					}
				}
			}
		}

		if (g_menuOpen && g_guiInitialized) {
			g_gui.Render(750, 620);  // was 520 - Player ESP grew to 9 rows (Head Dot,
			                         // Skeleton) and Loot ESP to 6 (Grenades), same
			                         // overflow the external build hit
		}

		// Watermark - top center, offset slightly right so it doesn't collide with the
		// menu (left)
		const char* watermarkText = "ROTK Menu - Stan369 - Unknowncheats.me - 10-05-2026";
		float watermarkWidth = (float)strlen(watermarkText) * g_fontAdvanceW;
		float watermarkX = menuScreenW * 0.58f - watermarkWidth / 2.0f;
		QueueText(watermarkX, 60.0f, watermarkText, 1.0f, 0.15f, 0.15f, 0.9f, menuScreenW, menuScreenH);

		// Active-feature indicators - always visible (menu open or closed) so a toggle
		// hotkey's effect is confirmable without alt-tabbing into the overlay. Stacked
		// bottom-left, each one advancing the running Y offset upward.
		float indicatorY = menuScreenH - 40.0f;
		if (g_noRecoil) {
			QueueText(20.0f, indicatorY, "NORECOIL: ON", 0.2f, 1.0f, 0.2f, 1.0f, menuScreenW, menuScreenH);
			indicatorY -= g_fontCellH + 4.0f;
		}
		if (g_aimbot) {
			QueueText(20.0f, indicatorY, "AIMBOT: ON", 0.2f, 1.0f, 0.2f, 1.0f, menuScreenW, menuScreenH);
			indicatorY -= g_fontCellH + 4.0f;
		}
		// Live weapon-name/profile-match readout - directly answers "does it know what
		// gun I'm holding" and "is a profile actually overriding the sliders" without
		// digging through rotk_log.txt. Only updates while the aim key is held (that's
		// the only time UpdateAimbotBulletSpeed runs); see g_weaponDiagName's comment.
		if (g_weaponProfiles) {
			char wepDiagBuf[200];
			sprintf_s(wepDiagBuf, "PROFILE: %s", g_weaponDiagProfile.c_str());
			QueueText(20.0f, indicatorY, wepDiagBuf, 1.0f, 0.85f, 0.2f, 1.0f, menuScreenW, menuScreenH);
		}

		// FOV ring - independent of holding the aimbot activation key, so the radius can
		// be seen/tuned via the slider without needing a target in view.
		if (g_showFovCircle) {
			QueueFovCirclePx(g_aimbotFov, 1.0f, 1.0f, 1.0f, 0.6f, menuScreenW, menuScreenH);
		}

		// Friend List add/remove feedback - a temporary on-screen notice instead of only a
		// log line, so confirmation is visible without opening the menu. Dead center of
		// the screen and red so it's impossible to miss.
		if (!g_friendNotice.empty() && GetTickCount64() < g_friendNoticeUntil) {
			float noticeWidth = (float)g_friendNotice.size() * g_fontAdvanceW;
			float noticeX = menuScreenW / 2.0f - noticeWidth / 2.0f;
			float noticeY = menuScreenH / 2.0f - g_fontCellH / 2.0f;
			QueueText(noticeX, noticeY, g_friendNotice.c_str(), 1.0f, 0.0f, 0.0f, 1.0f, menuScreenW, menuScreenH);
		}
	}

	if (g_d3d11Ready && foundGameAddr && handle) {

		// New dump chain: cgame slot -> +game_manager. Falls back to the signature-scanned
		// legacy slot if either hop is null.
		uint64_t legacyGame = read<uint64_t>(handle, foundGameAddr);
		uint64_t engineObj = read<uint64_t>(handle, g_moduleBase + RVA_CGAME_DUMP);
		uint64_t managerGame = engineObj ? read<uint64_t>(handle, engineObj + dumpGameManager) : 0;
		uint64_t cGame = managerGame ? managerGame : legacyGame;
		uint64_t cGraphics = read<uint64_t>(handle, adjustedGraphicsAddr);

		if (diagFramesLeft > 0) {
			sprintf_s(diagBuf, "diag: cGame=0x%llX (manager=0x%llX legacy=0x%llX) cGraphics=0x%llX", cGame, managerGame, legacyGame, cGraphics);
			Log(diagBuf);
		}

		// Pure code patch, independent of any game-state reads - just keep the actual
		// patched bytes in sync with the desired toggle state.
		if (g_noRecoil != g_aimPunchPatched) {
			ApplyAimPunchPatch(g_noRecoil);
		}

		// Friend List add - aim at a player and press this to add their name to the
		// exclusion list. Target selection (closest-to-crosshair alive player, by name
		// this time instead of position) happens in the entity loop below, same pattern.
		bool addFriendKeyDown = (GetAsyncKeyState(g_addFriendKey) & 0x8000) != 0;
		bool addFriendTriggered = addFriendKeyDown && !g_addFriendKeyWasDown && !g_awaitingKeybindTarget && g_friendListEnabled;
		g_addFriendKeyWasDown = addFriendKeyDown;


		if (cGame && cGraphics) {
			uint64_t camera = read<uint64_t>(handle, cGraphics + graphicsCamera);
			uint64_t matrixObj = camera ? read<uint64_t>(handle, camera + cameraMatrixObject) : 0;

			if (diagFramesLeft > 0) {
				sprintf_s(diagBuf, "diag: camera=0x%llX matrixObj=0x%llX", camera, matrixObj);
				Log(diagBuf);
			}

			if (matrixObj) {
				// Guard against torn reads: the render thread may be mid-update to this
				// matrix when we read it, producing a mix of old/new rows that projects
				// inconsistently (visible as boxes drifting when the camera rotates).
				D3DXMATRIX viewProj = {};
				D3DXMATRIX viewProjRetry = {};
				bool matrixOk = false;
				for (int attempt = 0; attempt < 4; attempt++) {
					if (!ReadProcessMemory(handle, (LPCVOID)(matrixObj + matrixViewProjection), &viewProj, sizeof(viewProj), NULL)) break;
					if (!ReadProcessMemory(handle, (LPCVOID)(matrixObj + matrixViewProjection), &viewProjRetry, sizeof(viewProjRetry), NULL)) break;
					if (memcmp(&viewProj, &viewProjRetry, sizeof(viewProj)) == 0) {
						matrixOk = true;
						break;
					}
				}
				if (matrixOk) {
					DXGI_SWAP_CHAIN_DESC scDesc;
					pSwapChain->GetDesc(&scDesc);
					float screenW = (float)scDesc.BufferDesc.Width;
					float screenH = (float)scDesc.BufferDesc.Height;

					int EntNumber = read<int>(handle, cGame + gameEntityCount);
					uint64_t currentEnt = read<uint64_t>(handle, cGame + gameFirstEntity);
					uint64_t localPlayer = read<uint64_t>(handle, cGame + gameLocalPlayer);

					// Aimbot setup - read "my" own feet once per frame (not per target)
					// and track the single closest-to-crosshair valid candidate across
					// the whole entity loop, applying one mouse move after it ends
					// (rather than the reference's approach of moving toward whichever
					// target happened to be processed last, which caused jerkiness with
					// multiple targets in FOV).
					bool aimbotActive = g_aimbot && (GetAsyncKeyState(g_aimbotKey) & 0x8000);
					D3DXVECTOR3 myFeet(0, 0, 0);
					bool aimbotHasTarget = false;
					float aimbotBestFovDist = 1e9f;
					D3DXVECTOR2 aimbotBestScreenPos;
					D3DXVECTOR3 aimbotBestWorldPos;  // same predicted point as aimbotBestScreenPos,
					                                  // kept in world space for Magic Bullet below

					// Friend List add target - same closest-to-crosshair approach, tracking
					// a name instead of a position/destination.
					bool addFriendHasTarget = false;
					float addFriendBestFovDist = 1e9f;
					string addFriendBestName;

					// myFeet is also needed for the distance-in-meters ESP option and for
					// Magic Bullet (both the owner-GUID learning heuristic and the target
					// prediction math), not just aimbot, so this reads it whenever any of
					// them need it instead of only while aimbot is actively held.
					if ((aimbotActive || g_showDistance) && localPlayer) {
						uint64_t myPlayerBase = read<uint64_t>(handle, localPlayer + entityPlayerBase);
						if (myPlayerBase) {
							myFeet = read<D3DXVECTOR3>(handle, myPlayerBase + playerBasePosition);
						}
						if (aimbotActive) {
							UpdateAimbotBulletSpeed(localPlayer);
						}
					}

					// The entity list holds everything (items, corpses, vehicles, structures),
					// not just players, so a low cap here can cut off real players that just
					// happen to be later in the list. 2000 is a generous safety ceiling against
					// a malformed/circular linked list, not a realistic scene size.
					for (int i = 0; i < EntNumber && currentEnt && i < 2000; i++) {
						int Type = read<int>(handle, currentEnt + entityType);
						if (Type == playerEntityType && currentEnt != localPlayer) {
							uint64_t playerBase = read<uint64_t>(handle, currentEnt + entityPlayerBase);
							if (playerBase) {
								// Use feet (playerBasePosition) as the box bottom and a standing-height
								// offset as the top, instead of pelvis (mid-body), which only covered
								// the upper half of the character.
								D3DXVECTOR3 feet = read<D3DXVECTOR3>(handle, playerBase + playerBasePosition);
								D3DXVECTOR3 head = feet + D3DXVECTOR3(0.0f, 1.9f, 0.0f);

								// Skip ESP rendering on corpses - health can read stale/nonzero for a
								// frame or two during the death animation, but this field flips
								// immediately.
								bool alive = read<int>(handle, playerBase + playerIsAlive) != 0;

								if (addFriendTriggered && alive) {
									D3DXVECTOR2 afNdc;
									if (WorldToNDC(viewProj, feet, afNdc)) {
										float afPxX, afPxY;
										NdcToPixel(afNdc.x, afNdc.y, screenW, screenH, afPxX, afPxY);
										float afDx = afPxX - screenW / 2.0f;
										float afDy = afPxY - screenH / 2.0f;
										float afFovDist = sqrtf(afDx * afDx + afDy * afDy);
										if (afFovDist < addFriendBestFovDist) {
											uint64_t afNamePtr = read<uint64_t>(handle, currentEnt + entityName);
											if (afNamePtr) {
												PN afPn = read<PN>(handle, afNamePtr);
												afPn.name[31] = '\0';
												if (afPn.name[0] != '\0') {
													addFriendBestFovDist = afFovDist;
													addFriendBestName = afPn.name;
													addFriendHasTarget = true;
												}
											}
										}
									}
								}

								if (aimbotActive) {
									// Friend List exclusion - checked before anything else in this
									// block, so an excluded player can never end up as
									// aimbotBestWorldPos for Aimbot's mouse-move. Fails closed: an
									// unreadable name this frame is treated as excluded.
									bool friendExcluded = false;
									if (g_friendListEnabled) {
										uint64_t abNamePtr = read<uint64_t>(handle, currentEnt + entityName);
										string abName;
										if (abNamePtr) {
											PN abPn = read<PN>(handle, abNamePtr);
											abPn.name[31] = '\0';
											abName = abPn.name;
										}
										friendExcluded = abName.empty() || IsFriendName(abName);
									}

									uint64_t healthBase = friendExcluded ? 0 : read<uint64_t>(handle, currentEnt + charHealthBase);
									int health = healthBase ? read<int>(handle, healthBase + charHealthValue) / 100 : 0;
									if (health > 0) {
										D3DXVECTOR3 toTarget = feet - myFeet;
										float distPly = sqrtf(toTarget.x * toTarget.x + toTarget.y * toTarget.y + toTarget.z * toTarget.z);

										// Just the target's own velocity, not relative to ours - targetPos
										// gets projected through viewProj every frame, which is already
										// rebuilt from our current live position, so subtracting our own
										// velocity here double-counted our movement and skewed the aim
										// point sideways in whichever direction we were moving.
										D3DXVECTOR3 targetVelocity = read<D3DXVECTOR3>(handle, currentEnt + entityVelocity);
										float predictionTime = distPly / g_aimbotBulletSpeed;

										// Head height above feet changes with stance - a fixed offset aims
										// into empty air above a crouched/prone target. Our own diagnostics
										// never got a clean read on entityStance (same value gave both good
										// and bad results depending on which of several nearby players
										// actually got measured), so this was previously a broad "any
										// non-zero/non-4 stance = crouch" guess. A public offset dump for
										// this exact field (0x954) states 0=stand 1=crouch 6/7=prone,
										// which is more specific than anything we confirmed ourselves -
										// using it directly instead of the broad guess.
										int targetStance = read<int>(handle, currentEnt + entityStance);
										float headHeight = AIMBOT_HEAD_HEIGHT;
										if (targetStance == 6 || targetStance == 7) headHeight = 0.35f;
										else if (targetStance == 1) headHeight = 1.0f;

										D3DXVECTOR3 targetPos = feet + D3DXVECTOR3(0.0f, headHeight, 0.0f);
										targetPos.x += targetVelocity.x * predictionTime;
										targetPos.y += targetVelocity.y * predictionTime;
										targetPos.z += targetVelocity.z * predictionTime;

										if (distPly > 100.0f) {
											// Real gravity drop (0.5 * g * t^2) using the same time-of-flight
											// already computed above, instead of a linear distance formula -
											// that one topped out at a flat 1m cap regardless of range, so
											// anything past ~350m was getting the same tiny compensation as
											// a 350m shot. This scales quadratically with distance instead.
											float drop = 0.5f * 9.8f * predictionTime * predictionTime;
											if (drop > 15.0f) drop = 15.0f;  // sanity bound, not a real limiter
											targetPos.y += drop;
										}

										D3DXVECTOR2 aimNdc;
										if (WorldToNDC(viewProj, targetPos, aimNdc)) {
											float aimPxX, aimPxY;
											NdcToPixel(aimNdc.x, aimNdc.y, screenW, screenH, aimPxX, aimPxY);
											float fovDx = aimPxX - screenW / 2.0f;
											float fovDy = aimPxY - screenH / 2.0f;
											float fovDist = sqrtf(fovDx * fovDx + fovDy * fovDy);

											// None of entityStance/entityFlags/entityAnimFlags actually flip
											// for crouch - all three came back identical between confirmed
											// standing and confirmed crouching samples. Rather than guess a
											// fourth field blind, this directly SOLVES for the real head
											// height: project two known reference heights (feet+0, feet+2m)
											// to screen space and linearly interpolate/extrapolate for
											// whatever world-space height would land exactly on your
											// crosshair - valid because perspective projection is affine in
											// world Y for a fixed X/Z depth, and because you're the one
											// centering the crosshair on their real head during this test.
											if (targetStance != 0 && distPly < 50.0f && g_aimbotStanceDiagLeft > 0) {
												D3DXVECTOR2 ndcLow, ndcHigh;
												bool okLow = WorldToNDC(viewProj, feet, ndcLow);
												bool okHigh = WorldToNDC(viewProj, feet + D3DXVECTOR3(0.0f, 2.0f, 0.0f), ndcHigh);
												if (okLow && okHigh) {
													float pxLowX, pxLowY, pxHighX, pxHighY;
													NdcToPixel(ndcLow.x, ndcLow.y, screenW, screenH, pxLowX, pxLowY);
													NdcToPixel(ndcHigh.x, ndcHigh.y, screenW, screenH, pxHighX, pxHighY);
													float pxPerMeter = (pxHighY - pxLowY) / 2.0f;
													float neededHeight = (screenH / 2.0f - pxLowY) / pxPerMeter;

													char stanceBuf[220];
													sprintf_s(stanceBuf, "Aimbot stance diag: targetStance=%d headHeight=%.2f fovDist=%.1f passed=%d neededHeight=%.2f",
														targetStance, headHeight, fovDist, fovDist <= g_aimbotFov ? 1 : 0, neededHeight);
													Log(stanceBuf);
													g_aimbotStanceDiagLeft--;
												}
											}

											if (fovDist <= g_effectiveAimbotFov && fovDist < aimbotBestFovDist) {
												aimbotBestFovDist = fovDist;
												aimbotBestScreenPos = D3DXVECTOR2(aimPxX, aimPxY);
												aimbotBestWorldPos = targetPos;
												aimbotHasTarget = true;
											}
										}
									}
								}

								D3DXVECTOR2 feetNdc, headNdc;
								bool okBottom = WorldToNDC(viewProj, feet, feetNdc);
								bool okTop = WorldToNDC(viewProj, head, headNdc);
								if (okBottom && okTop) {
									// Box height comes from projecting two real 3D points, so it
									// scales correctly with distance instead of a fixed pixel size
									float ndcHeight = fabsf(headNdc.y - feetNdc.y);
									float ndcWidth = ndcHeight / 4.5f;
									float centerX = (headNdc.x + feetNdc.x) / 2.0f;
									float centerY = (headNdc.y + feetNdc.y) / 2.0f;

									// Health/name/distance only need the box's computed NDC rect, not
									// the box itself - each was previously nested inside
									// g_showPlayerBoxes, so turning boxes off silently killed all
									// three even when their own toggles were on.
									if (g_showPlayerBoxes && alive) {
										const float* playerColor = g_playerColor;
										QueueESPBoxNdc(centerX, centerY, ndcWidth / 2.0f, ndcHeight / 2.0f, playerColor[0], playerColor[1], playerColor[2], 1.0f);
									}

									if (g_showHealthBars && alive) {
										uint64_t healthBase = read<uint64_t>(handle, currentEnt + charHealthBase);
										if (healthBase) {
											int health = read<int>(handle, healthBase + charHealthValue) / 100;
											char healthBuf[8];
											sprintf_s(healthBuf, "%d", health);

											float sidePxX, sidePxY;
											NdcToPixel(centerX + ndcWidth / 2.0f, centerY, screenW, screenH, sidePxX, sidePxY);
											float healthTextX = sidePxX + 6.0f;
											float healthTextY = sidePxY - g_fontCellH / 2.0f;

											float hr = 1.0f - (health / 100.0f), hg = (health / 100.0f), hb = 0.0f;
											QueueText(healthTextX, healthTextY, healthBuf, hr, hg, hb, 1.0f, screenW, screenH);
										}
									}

									if (g_showPlayerNames && alive) {
										uint64_t namePtr = read<uint64_t>(handle, currentEnt + entityName);
										if (namePtr) {
											PN pn = read<PN>(handle, namePtr);
											pn.name[31] = '\0';
											if (pn.name[0] != '\0') {
												float boxTopPxX, boxTopPxY;
												NdcToPixel(centerX, centerY + ndcHeight / 2.0f, screenW, screenH, boxTopPxX, boxTopPxY);
												float textX = boxTopPxX - (float)strlen(pn.name) * g_fontAdvanceW / 2.0f;
												float textY = boxTopPxY - g_fontCellH - 4.0f;
												QueueText(textX, textY, pn.name, 1.0f, 1.0f, 1.0f, 1.0f, screenW, screenH);
											}
										}
									}

									if (g_showDistance && alive) {
										D3DXVECTOR3 toPlayer = feet - myFeet;
										float distM = sqrtf(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y + toPlayer.z * toPlayer.z);
										char distBuf[16];
										sprintf_s(distBuf, "%.0fm", distM);

										float boxBotPxX, boxBotPxY;
										NdcToPixel(centerX, centerY - ndcHeight / 2.0f, screenW, screenH, boxBotPxX, boxBotPxY);
										float distTextX = boxBotPxX - (float)strlen(distBuf) * g_fontAdvanceW / 2.0f;
										float distTextY = boxBotPxY + 4.0f;
										QueueText(distTextX, distTextY, distBuf, 1.0f, 1.0f, 1.0f, 1.0f, screenW, screenH);
									}

									// Uses the real skeleton head bone when resolvable (same
									// pointer chain as Skeleton below), falling back to the
									// feet+1.9m box-top approximation otherwise.
									if ((g_showHeadDot || g_showSkeleton) && alive) {
										uint64_t actor = read<uint64_t>(handle, currentEnt + skeletonActors);
										uint64_t skelStart = actor ? read<uint64_t>(handle, actor + skeletonStarts) : 0;
										uint64_t pose = skelStart ? read<uint64_t>(handle, skelStart + skeletonInfos) : 0;
										uint64_t jointData = pose ? read<uint64_t>(handle, pose + skeletonBoneInfo) : 0;
										if (jointData) {
											float yaw = read<float>(handle, currentEnt + entityYaw);

											if (g_showHeadDot) {
												D3DXVECTOR3 realHead = GetBoneWorld(jointData, kHeadBoneIdx, feet, yaw);
												D3DXVECTOR3 dotPos = IsValidBone(realHead, feet) ? realHead : head;
												D3DXVECTOR2 dotNdc;
												if (WorldToNDC(viewProj, dotPos, dotNdc)) {
													float dotPxX, dotPxY;
													NdcToPixel(dotNdc.x, dotNdc.y, screenW, screenH, dotPxX, dotPxY);
													QueueFilledCirclePx(dotPxX, dotPxY, 6.0f, 0.0f, 0.0f, 0.0f, 1.0f, screenW, screenH);
													QueueFilledCirclePx(dotPxX, dotPxY, 4.5f, g_headDotColor[0], g_headDotColor[1], g_headDotColor[2], 1.0f, screenW, screenH);
												}
											}

											if (g_showSkeleton) {
												static const size_t kJointIndices[SKEL_BONE_COUNT] = {
													kHeadBoneIdx, kNeckBoneIdx, kChestBoneIdx, kSpineBoneIdx, kPelvisBoneIdx,
													kLShoulderIdx, kLElbowBoneIdx, kLHandIdx,
													kRShoulderIdx, kRElbowBoneIdx, kRHandIdx,
													kLHipIdx, kLKneeBoneIdx, kLFootBoneIdx,
													kRHipIdx, kRKneeBoneIdx, kRFootBoneIdx,
												};
												D3DXVECTOR3 bones[SKEL_BONE_COUNT];
												for (int b = 0; b < SKEL_BONE_COUNT; b++) {
													bones[b] = GetBoneWorld(jointData, kJointIndices[b], feet, yaw);
												}
												for (const auto& conn : kSkeletonBoneConnections) {
													const D3DXVECTOR3& boneA = bones[conn[0]];
													const D3DXVECTOR3& boneB = bones[conn[1]];
													if (!IsValidBone(boneA, feet) || !IsValidBone(boneB, feet)) continue;

													D3DXVECTOR2 a, b;
													if (!WorldToNDC(viewProj, boneA, a)) continue;
													if (!WorldToNDC(viewProj, boneB, b)) continue;
													QueueSkeletonLineNdc(a.x, a.y, b.x, b.y, g_skeletonColor[0], g_skeletonColor[1], g_skeletonColor[2], 1.0f);
												}
											}
										}
									}
								}
							}
						} else if (g_showItemESP && IsGroundItemType(Type)) {
							uint64_t itemNamePtr = read<uint64_t>(handle, currentEnt + entityName);
							if (itemNamePtr) {
								PN itemPn = read<PN>(handle, itemNamePtr);
								itemPn.name[31] = '\0';
								if (IsArmorName(itemPn.name)) {
									// Items use their own position offset (itemPosition), not the
									// generic entity transform, matching the reference's OFFSET_ITEMPOS.
									D3DXVECTOR3 itemPos = read<D3DXVECTOR3>(handle, currentEnt + itemPosition);
									D3DXVECTOR2 itemNdc;
									if (WorldToNDC(viewProj, itemPos, itemNdc)) {
										const float* itemColor = g_itemColor;
										QueueESPBoxNdc(itemNdc.x, itemNdc.y, 0.01f, 0.01f, itemColor[0], itemColor[1], itemColor[2], 1.0f);
									}
								}
							}
						} else if (g_showLootbags && Type == lootbagEntityType) {
							// Loot bags use the same item position offset as regular ground
							// items - unlike Kevlars, drawn unconditionally regardless of the
							// bag's own internal name, since the bag itself (not its contents)
							// is what we're marking.
							D3DXVECTOR3 bagPos = read<D3DXVECTOR3>(handle, currentEnt + itemPosition);
							D3DXVECTOR2 bagNdc;
							if (WorldToNDC(viewProj, bagPos, bagNdc)) {
								const float* bagColor = g_lootbagColor;
								QueueESPBoxNdc(bagNdc.x, bagNdc.y, 0.012f, 0.012f, bagColor[0], bagColor[1], bagColor[2], 1.0f);

								float bagPxX, bagPxY;
								NdcToPixel(bagNdc.x, bagNdc.y, screenW, screenH, bagPxX, bagPxY);
								float bagTextX = bagPxX - 3.0f * g_fontAdvanceW;
								float bagTextY = bagPxY + 8.0f;
								QueueText(bagTextX, bagTextY, "Lootbag", bagColor[0], bagColor[1], bagColor[2], 1.0f, screenW, screenH);
							}
						} else if (g_showGrenades && Type == grenadeEntityType) {
							uint64_t grenadeNamePtr = read<uint64_t>(handle, currentEnt + entityName);
							if (grenadeNamePtr) {
								PN grenadePn = read<PN>(handle, grenadeNamePtr);
								grenadePn.name[31] = '\0';
								if (IsHeGrenadeName(grenadePn.name)) {
									D3DXVECTOR3 nadePos = read<D3DXVECTOR3>(handle, currentEnt + itemPosition);
									D3DXVECTOR2 nadeNdc;
									if (WorldToNDC(viewProj, nadePos, nadeNdc)) {
										const float* nadeColor = g_grenadeColor;
										QueueESPBoxNdc(nadeNdc.x, nadeNdc.y, 0.012f, 0.012f, nadeColor[0], nadeColor[1], nadeColor[2], 1.0f);

										float nadePxX, nadePxY;
										NdcToPixel(nadeNdc.x, nadeNdc.y, screenW, screenH, nadePxX, nadePxY);
										float nadeTextX = nadePxX - 5.0f * g_fontAdvanceW;
										float nadeTextY = nadePxY + 8.0f;
										QueueText(nadeTextX, nadeTextY, "HE Grenade", nadeColor[0], nadeColor[1], nadeColor[2], 1.0f, screenW, screenH);
									}
								}
							}
						} else if (g_showGroundWeapons && Type == weaponEntityType) {
							// Drawn unconditionally on Type alone, like Lootbags above - Type ==
							// weaponEntityType already IS the "this is a weapon pickup" signal (see
							// its definition in Offs.h), so gating on a name-keyword match on top of
							// that was the bug: any weapon whose internal asset name didn't contain
							// one of the recognized substrings (all but Magnum, in practice) got
							// filtered out here despite being a real weapon pickup.
							D3DXVECTOR3 weaponPos = read<D3DXVECTOR3>(handle, currentEnt + itemPosition);
							D3DXVECTOR2 weaponNdc;
							if (WorldToNDC(viewProj, weaponPos, weaponNdc)) {
								const float* weaponColor = g_weaponEspColor;
								QueueESPBoxNdc(weaponNdc.x, weaponNdc.y, 0.012f, 0.012f, weaponColor[0], weaponColor[1], weaponColor[2], 1.0f);

								string weaponLabel = "Weapon";
								uint64_t weaponNamePtr = read<uint64_t>(handle, currentEnt + entityName);
								if (weaponNamePtr) {
									PN weaponPn = read<PN>(handle, weaponNamePtr);
									weaponPn.name[31] = '\0';
									weaponLabel = GetWeaponDisplayName(weaponPn.name);
								}
								float weaponPxX, weaponPxY;
								NdcToPixel(weaponNdc.x, weaponNdc.y, screenW, screenH, weaponPxX, weaponPxY);
								float weaponTextX = weaponPxX - (float)weaponLabel.size() * g_fontAdvanceW / 2.0f;
								float weaponTextY = weaponPxY + 8.0f;
								QueueText(weaponTextX, weaponTextY, weaponLabel.c_str(), weaponColor[0], weaponColor[1], weaponColor[2], 1.0f, screenW, screenH);
							}
						} else if (g_showVehicleESP && IsVehicleType(Type)) {
							// Cars use their own position offset (vehiclePosition), not the
							// generic entity transform, matching the reference cheat's OFFSET_CARPOS.
							D3DXVECTOR3 vehPos = read<D3DXVECTOR3>(handle, currentEnt + vehiclePosition);
							D3DXVECTOR2 vehNdc;
							if (WorldToNDC(viewProj, vehPos, vehNdc)) {
								float vehPxX, vehPxY;
								NdcToPixel(vehNdc.x, vehNdc.y, screenW, screenH, vehPxX, vehPxY);

								if (g_showVehicleESP) {
									float vr, vg, vb;
									GetVehicleColor(Type, vr, vg, vb);
									QueueESPBoxNdc(vehNdc.x, vehNdc.y, 0.025f, 0.025f, vr, vg, vb, 1.0f);

									if (g_showDistance) {
										D3DXVECTOR3 toVeh = vehPos - myFeet;
										float distM = sqrtf(toVeh.x * toVeh.x + toVeh.y * toVeh.y + toVeh.z * toVeh.z);
										char distBuf[16];
										sprintf_s(distBuf, "%.0fm", distM);

										float distTextX = vehPxX - (float)strlen(distBuf) * g_fontAdvanceW / 2.0f;
										float distTextY = vehPxY + 10.0f;
										QueueText(distTextX, distTextY, distBuf, vr, vg, vb, 1.0f, screenW, screenH);
									}
								}
							}
						}
						currentEnt = read<uint64_t>(handle, currentEnt + entityNext);
					}

					// aimbotHasTarget can now be true from Magic Bullet alone (it reuses this
					// same targeting pass) - gate the actual mouse-snap on aimbotActive
					// specifically so Magic Bullet on its own never moves your mouse.
					if (aimbotHasTarget && aimbotActive) {
						// Dividing by smoothness instead of moving the full delta means each
						// frame only closes part of the remaining distance - since the target
						// screen position is recomputed fresh next frame too, this naturally
						// eases in rather than snapping, with no extra state to track.
						float smooth = g_effectiveAimbotSmoothness < 1.0f ? 1.0f : g_effectiveAimbotSmoothness;
						INPUT input = {};
						input.type = INPUT_MOUSE;
						input.mi.dx = (LONG)((aimbotBestScreenPos.x - screenW / 2.0f) / smooth);
						input.mi.dy = (LONG)((aimbotBestScreenPos.y - screenH / 2.0f) / smooth);
						input.mi.dwFlags = MOUSEEVENTF_MOVE;
						SendInput(1, &input, sizeof(INPUT));
					}

					if (addFriendHasTarget) {
						AddFriendName(addFriendBestName);
					} else if (addFriendTriggered) {
						g_friendNotice = "No player found under crosshair to add";
						g_friendNoticeUntil = GetTickCount64() + 5000;
					}

					if (diagFramesLeft > 0) {
						sprintf_s(diagBuf, "diag: EntNumber=%d boxesQueued=%zu screenW=%.0f screenH=%.0f", EntNumber, g_frameVerts.size() / 5, screenW, screenH);
						Log(diagBuf);
					}
				}
			}
		}

		if (!g_frameVerts.empty() || !g_circleVerts.empty() || !g_skeletonVerts.empty() || !g_textVerts.empty() || !g_fillVerts.empty()) {
			D3D11_VIEWPORT vp = {};
			vp.Width = menuScreenW;
			vp.Height = menuScreenH;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			vp.TopLeftX = 0;
			vp.TopLeftY = 0;
			g_pContext->RSSetViewports(1, &vp);

			g_pContext->OMSetRenderTargets(1, &g_pRTV, NULL);
			g_pContext->OMSetDepthStencilState(g_pNoDepthState, 0);
			float blendFactor[4] = { 0, 0, 0, 0 };
			g_pContext->OMSetBlendState(g_pBlendState, blendFactor, 0xffffffff);

			if (!g_frameVerts.empty()) {
				D3D11_MAPPED_SUBRESOURCE mapped;
				if (SUCCEEDED(g_pContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					size_t count = g_frameVerts.size() < (size_t)g_maxVerts ? g_frameVerts.size() : (size_t)g_maxVerts;
					memcpy(mapped.pData, g_frameVerts.data(), count * sizeof(NdcVertex));
					g_pContext->Unmap(g_pVertexBuffer, 0);

					g_pContext->IASetInputLayout(g_pInputLayout);
					UINT stride = sizeof(NdcVertex), offset = 0;
					g_pContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &stride, &offset);
					g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
					g_pContext->VSSetShader(g_pVS, NULL, 0);
					g_pContext->PSSetShader(g_pPS, NULL, 0);

					// Each box is its own 5-vertex strip, drawn separately so boxes don't connect
					for (size_t i = 0; i + 5 <= count; i += 5) {
						g_pContext->Draw(5, (UINT)i);
					}
				}
			}

			if (!g_circleVerts.empty()) {
				// One continuous strip (unlike the fixed-5 box loop above) since only a
				// single circle is ever queued per frame.
				D3D11_MAPPED_SUBRESOURCE mappedCircle;
				if (SUCCEEDED(g_pContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCircle))) {
					size_t circleCount = g_circleVerts.size() < (size_t)g_maxVerts ? g_circleVerts.size() : (size_t)g_maxVerts;
					memcpy(mappedCircle.pData, g_circleVerts.data(), circleCount * sizeof(NdcVertex));
					g_pContext->Unmap(g_pVertexBuffer, 0);

					g_pContext->IASetInputLayout(g_pInputLayout);
					UINT circleStride = sizeof(NdcVertex), circleOffset = 0;
					g_pContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &circleStride, &circleOffset);
					g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
					g_pContext->VSSetShader(g_pVS, NULL, 0);
					g_pContext->PSSetShader(g_pPS, NULL, 0);
					g_pContext->Draw((UINT)circleCount, 0);
				}
			}

			if (!g_skeletonVerts.empty()) {
				D3D11_MAPPED_SUBRESOURCE mappedSkel;
				if (SUCCEEDED(g_pContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSkel))) {
					size_t skelCount = g_skeletonVerts.size() < (size_t)g_maxVerts ? g_skeletonVerts.size() : (size_t)g_maxVerts;
					memcpy(mappedSkel.pData, g_skeletonVerts.data(), skelCount * sizeof(NdcVertex));
					g_pContext->Unmap(g_pVertexBuffer, 0);

					g_pContext->IASetInputLayout(g_pInputLayout);
					UINT skelStride = sizeof(NdcVertex), skelOffset = 0;
					g_pContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &skelStride, &skelOffset);
					g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
					g_pContext->VSSetShader(g_pVS, NULL, 0);
					g_pContext->PSSetShader(g_pPS, NULL, 0);
					// Each bone connection is its own independent 2-vertex segment, drawn
					// separately like the box loop above, so unrelated bones don't connect.
					for (size_t i = 0; i + 2 <= skelCount; i += 2) g_pContext->Draw(2, (UINT)i);
				}
			}

			if (!g_fillVerts.empty()) {
				// Reuses the box vertex buffer/shaders, remapped with menu panel quads and
				// drawn as TRIANGLELIST (filled) instead of LINESTRIP (outline).
				D3D11_MAPPED_SUBRESOURCE mappedFill;
				if (SUCCEEDED(g_pContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedFill))) {
					size_t fillCount = g_fillVerts.size() < (size_t)g_maxVerts ? g_fillVerts.size() : (size_t)g_maxVerts;
					memcpy(mappedFill.pData, g_fillVerts.data(), fillCount * sizeof(NdcVertex));
					g_pContext->Unmap(g_pVertexBuffer, 0);

					g_pContext->IASetInputLayout(g_pInputLayout);
					UINT fillStride = sizeof(NdcVertex), fillOffset = 0;
					g_pContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &fillStride, &fillOffset);
					g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					g_pContext->VSSetShader(g_pVS, NULL, 0);
					g_pContext->PSSetShader(g_pPS, NULL, 0);
					g_pContext->Draw((UINT)fillCount, 0);
				}
			}

			if (g_fontReady && !g_textVerts.empty()) {
				D3D11_MAPPED_SUBRESOURCE mappedText;
				if (SUCCEEDED(g_pContext->Map(g_pTextVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedText))) {
					size_t textCount = g_textVerts.size() < (size_t)g_maxTextVerts ? g_textVerts.size() : (size_t)g_maxTextVerts;
					memcpy(mappedText.pData, g_textVerts.data(), textCount * sizeof(TexVertex));
					g_pContext->Unmap(g_pTextVertexBuffer, 0);

					g_pContext->IASetInputLayout(g_pTextInputLayout);
					UINT textStride = sizeof(TexVertex), textOffset = 0;
					g_pContext->IASetVertexBuffers(0, 1, &g_pTextVertexBuffer, &textStride, &textOffset);
					g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					g_pContext->VSSetShader(g_pTextVS, NULL, 0);
					g_pContext->PSSetShader(g_pTextPS, NULL, 0);
					g_pContext->PSSetShaderResources(0, 1, &g_pFontSRV);
					g_pContext->PSSetSamplers(0, 1, &g_pFontSampler);

					g_pContext->Draw((UINT)textCount, 0);
				}
			}
		}

	}

	if (diagFramesLeft > 0) diagFramesLeft--;

	// Unhook everything now (still safely on this thread, inside this call), then fall
	// through to the final oPresent() below so this last frame completes normally.
	if (g_unloadRequested) {
		g_unloadRequested = false;
		PerformUnload();
	}

	return oPresent(pSwapChain, SyncInterval, Flags);
}

// Creates a throwaway D3D11 device+swap chain purely to read the swap chain's
// vtable, then patches the Present slot in that (shared) vtable so the game's
// real swap chain calls our hook too.
void HookD3D11()
{
	Log("HookD3D11() starting...");

	WNDCLASSA wc = {};
	wc.lpfnWndProc = DefWindowProcA;
	wc.lpszClassName = "ROTK_DummyWnd11";
	RegisterClassA(&wc);
	HWND dummyWnd = CreateWindowA("ROTK_DummyWnd11", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferCount = 1;
	scd.BufferDesc.Width = 100;
	scd.BufferDesc.Height = 100;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.BufferDesc.RefreshRate.Numerator = 60;
	scd.BufferDesc.RefreshRate.Denominator = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = dummyWnd;
	scd.SampleDesc.Count = 1;
	scd.Windowed = TRUE;

	IDXGISwapChain* pSwapChain = nullptr;
	ID3D11Device* pDevice = nullptr;
	ID3D11DeviceContext* pContext = nullptr;
	D3D_FEATURE_LEVEL featureLevel;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
		D3D11_SDK_VERSION, &scd, &pSwapChain, &pDevice, &featureLevel, &pContext);

	if (FAILED(hr)) {
		char buf[128];
		sprintf_s(buf, "HookD3D11: D3D11CreateDeviceAndSwapChain failed! HRESULT=0x%lX", hr);
		Log(buf);
		DestroyWindow(dummyWnd);
		return;
	}

	void** vTable = *reinterpret_cast<void***>(pSwapChain);
	oPresent = (Present_t)vTable[8];  // IDXGISwapChain::Present

	DWORD oldProtect;
	VirtualProtect(&vTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
	vTable[8] = (void*)hkPresent;
	VirtualProtect(&vTable[8], sizeof(void*), oldProtect, &oldProtect);

	Log("HookD3D11: Present hooked successfully");

	pContext->Release();
	pDevice->Release();
	pSwapChain->Release();
	DestroyWindow(dummyWnd);
}

// The two full-module signature scans take seconds, so they run here instead of in DllMain:
// DllMain holds the loader lock, which freezes every other game thread until it returns.
DWORD WINAPI InitThread(LPVOID)
{
	// Get module base (H1Z1.exe)
	MODULEINFO modInfo = {};
	GetModuleInformation(handle, GetModuleHandleA("H1Z1.exe"), &modInfo, sizeof(modInfo));
	uint64_t moduleBase = (uint64_t)modInfo.lpBaseOfDll;
	uint64_t moduleEnd = moduleBase + modInfo.SizeOfImage;
	g_moduleBase = moduleBase;

	char buf[256];
	sprintf_s(buf, "Module base: 0x%I64X, Size: 0x%I64X", moduleBase, (uint64_t)modInfo.SizeOfImage);
	Log(buf);

	// cGraphics: computed directly from a verified RVA (moduleBase + RVA_GRAPHICS_SLOT)
	// rather than signature-scanned, since we don't have a graphics-specific pattern.
	adjustedGraphicsAddr = moduleBase + RVA_GRAPHICS_SLOT;
	sprintf_s(buf, "cGraphics via RVA: 0x%llX", adjustedGraphicsAddr);
	Log(buf);

	// The old game-global signature no longer exists in the current game build, and a failed
	// scan walks the whole 117 MB module (it was the ~90 s inject delay). cGame now comes from
	// the dump's fixed slot: [base+RVA_CGAME_DUMP] -> +dumpGameManager (see hkPresent).
	foundGameAddr = moduleBase + RVA_CGAME_DUMP;
	sprintf_s(buf, "cGame slot via RVA: 0x%llX", foundGameAddr);
	Log(buf);

	// Scan for the aim-punch CALL instruction (NoRecoil patch target)
	Log("Scanning for aim-punch call signature...");
	const char* aimPunchSig = "E8 27 A4 5B FE";
	g_aimPunchPatchAddr = FindSignature(handle, aimPunchSig, moduleBase, moduleEnd);
	if (g_aimPunchPatchAddr) {
		sprintf_s(buf, "Found aim-punch call at: 0x%llX", g_aimPunchPatchAddr);
		Log(buf);
	} else {
		Log("ERROR: Could not find aim-punch call signature - NoRecoil unavailable");
	}

	InitConfigPath();
	LoadConfig();

	Log("Hooking Direct3D11...");
	HookD3D11();
	Log("Cheat initialized");
	return 0;
}

// DLL Entry Point - called when DLL is injected
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		Log("DLL Injected!");
		g_hModule = hModule;
		handle = GetCurrentProcess();
		Log("Got process handle");

		HANDLE initThread = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
		if (initThread) CloseHandle(initThread);
		break;
	}
	case DLL_PROCESS_DETACH:
		// Best-effort: restore the original bytes if we patched them, so the game
		// isn't left permanently modified if the DLL unloads but the process keeps running.
		if (g_aimPunchPatched) {
			ApplyAimPunchPatch(false);
		}
		Log("DLL Unloaded");
		break;
	}
	return TRUE;
}
