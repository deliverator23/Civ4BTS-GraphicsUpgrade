// Defines the d3d9 interface IIDs (IID_IDirect3DDevice9, ...) in this module, so we don't link d3d9.lib or dxguid.lib.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
