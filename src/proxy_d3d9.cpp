#include "proxy_d3d9.h"
#include "proxy_device.h"

HRESULT ProxyD3D9::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9)
    {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    Log::Write("IDirect3D9::QueryInterface %s -> 0x%08x (passed through unwrapped)", Guid(riid).c_str(), static_cast<unsigned>(hr));
    return hr;
}

ULONG ProxyD3D9::Release()
{
    ULONG refs = m_real->Release();
    if (refs == 0)
    {
        Log::Write("IDirect3D9 %s released", Ptr(this).c_str());
        delete this;
    }
    return refs;
}

void LogPresentParameters(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp)
        return;
    Log::Write("%s: backbuffer %ux%u fmt %u x%u, ms %u/%u, swap %u, windowed %d, autodepth %d fmt %u, flags 0x%x, "
               "refresh %u, interval 0x%x",
               what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
               pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, pp->Windowed, pp->EnableAutoDepthStencil,
               pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

HRESULT ProxyD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                D3DPRESENT_PARAMETERS* pPresentationParameters,
                                IDirect3DDevice9** ppReturnedDeviceInterface)
{
    GU_LOG_CALL("IDirect3D9::CreateDevice");
    DWORD flags = BehaviorFlags;
    if (g_config.clearPureDevice && (flags & D3DCREATE_PUREDEVICE))
    {
        // A pure device can't report its state back (Get* calls fail), which traces rely on.
        flags &= ~D3DCREATE_PUREDEVICE;
        Log::Write("CreateDevice: cleared D3DCREATE_PUREDEVICE");
    }
    Log::Write("CreateDevice: adapter %u type %u behavior 0x%08x (requested 0x%08x)", Adapter, DeviceType, flags,
               BehaviorFlags);
    LogPresentParameters("CreateDevice", pPresentationParameters);

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, flags, pPresentationParameters, &device);
    Log::Write("CreateDevice -> 0x%08x", static_cast<unsigned>(hr));
    if (FAILED(hr) || !ppReturnedDeviceInterface)
    {
        if (ppReturnedDeviceInterface)
            *ppReturnedDeviceInterface = device;
        return hr;
    }
    *ppReturnedDeviceInterface = new ProxyDevice(device, this);
    return hr;
}
