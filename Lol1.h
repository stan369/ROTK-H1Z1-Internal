#pragma once

#include <Dwmapi.h> 
#include <TlHelp32.h>
#include "DX9.h"
#include "Couleurs.h"

#define CENTERX (GetSystemMetrics(SM_CXSCREEN)/2)-(screen_width/2)
#define CENTERY (GetSystemMetrics(SM_CYSCREEN)/2)-(screen_height/2)

enum circle_type { full, half, quarter };
enum text_alignment { lefted, centered, righted };

struct vertex
{
	FLOAT x, y, z, rhw;
	DWORD color;
};

extern int screen_width;
extern int screen_height;
extern LPDIRECT3D9 d3d;
extern LPDIRECT3DDEVICE9 d3ddev;
extern HWND hWnd;
extern HWND tWnd;
extern MARGINS margin;

// Drawing functions (simplified - rendering not critical for anticheat learning)
inline void SText(int x, int y, int orientation, void* g_pFont, bool bordered, DWORD color, DWORD bcolor, const wchar_t *fmt, ...) {}
inline void SText2(int x, int y, int orientation, void* g_pFont2, bool bordered, DWORD color, DWORD bcolor, const wchar_t *fmt, ...) {}
inline void DrawLine(float x, float y, float xx, float yy, float WitdhAmount, D3DCOLOR color) {}
inline void DrawBox(float x, float y, float width, float height, D3DCOLOR color) {}
inline void GetScreenSize(int* width, int* height) { *width = 1920; *height = 1080; }
inline void init(HWND hWnd) {}
