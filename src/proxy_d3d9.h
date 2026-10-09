// IDirect3D9 wrapper: hands the game a wrapped device from CreateDevice.
#pragma once
#include "common.h"
#include "d3d9_forward.h"

class ProxyD3D9 : public D3D9Forward
{
public:
    explicit ProxyD3D9(IDirect3D9* real) : D3D9Forward(real) {}

    // The wrapper shares the real object's reference count and deletes itself when that reaches zero.
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
    STDMETHOD_(ULONG, AddRef)() override { return m_real->AddRef(); }
    STDMETHOD_(ULONG, Release)() override;

    STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                            D3DPRESENT_PARAMETERS* pPresentationParameters,
                            IDirect3DDevice9** ppReturnedDeviceInterface) override;
};
