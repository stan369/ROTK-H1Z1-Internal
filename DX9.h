#ifndef DIRECT3D_H
#define DIRECT3D_H

#pragma once

#include <windows.h>
#include <directxmath.h>

#include <d3d9.h>

// DirectX Vector definitions (from d3dx9.h)
struct D3DXVECTOR2 {
	float x, y;
	D3DXVECTOR2() : x(0), y(0) {}
	D3DXVECTOR2(float x_, float y_) : x(x_), y(y_) {}
};
struct D3DXVECTOR3 {
	float x, y, z;
	D3DXVECTOR3() : x(0), y(0), z(0) {}
	D3DXVECTOR3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
	D3DXVECTOR3 operator+(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x+v.x, y+v.y, z+v.z); }
	D3DXVECTOR3 operator-(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x-v.x, y-v.y, z-v.z); }
	D3DXVECTOR3 operator*(float f) const { return D3DXVECTOR3(x*f, y*f, z*f); }
};
struct D3DXVECTOR4 {
	float x, y, z, w;
	D3DXVECTOR4() : x(0), y(0), z(0), w(0) {}
	D3DXVECTOR4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
struct D3DXMATRIX {
	float m[4][4];
	D3DXMATRIX() {}
};
typedef D3DXVECTOR2* LPD3DXVECTOR2;
typedef D3DXVECTOR3* LPD3DXVECTOR3;
typedef D3DXVECTOR4* LPD3DXVECTOR4;
typedef D3DXMATRIX* LPD3DXMATRIX;
typedef void* LPD3DXFONT;
typedef void* ID3DXLine;

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dxgi.lib")

#endif