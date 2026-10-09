// Base classes that forward every IDirect3D9 and IDirect3DDevice9 method to the real object (generated from the
// Windows SDK's d3d9.h). The proxy classes override the methods they change; IUnknown is left to them (reference
// counting and wrapper identity). Every forwarded method logs its first call (GU_FIRST_CALL, common.h), so a stall
// or crash shows the last new call the game made.
#pragma once
#include "common.h"

class D3D9Forward : public IDirect3D9
{
protected:
    IDirect3D9* m_real;

public:
    explicit D3D9Forward(IDirect3D9* real) : m_real(real) {}
    virtual ~D3D9Forward() {}
    IDirect3D9* real() const { return m_real; }

    STDMETHOD(RegisterSoftwareDevice)(void* pInitializeFunction) override { GU_LOG_CALL("IDirect3D9::RegisterSoftwareDevice"); return m_real->RegisterSoftwareDevice(pInitializeFunction); }
    STDMETHOD_(UINT, GetAdapterCount)() override { GU_LOG_CALL("IDirect3D9::GetAdapterCount"); return m_real->GetAdapterCount(); }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter,DWORD Flags,D3DADAPTER_IDENTIFIER9* pIdentifier) override { GU_LOG_CALL("IDirect3D9::GetAdapterIdentifier"); return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter,D3DFORMAT Format) override { GU_LOG_CALL("IDirect3D9::GetAdapterModeCount"); return m_real->GetAdapterModeCount(Adapter, Format); }
    STDMETHOD(EnumAdapterModes)(UINT Adapter,D3DFORMAT Format,UINT Mode,D3DDISPLAYMODE* pMode) override { GU_LOG_CALL("IDirect3D9::EnumAdapterModes"); return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter,D3DDISPLAYMODE* pMode) override { GU_LOG_CALL("IDirect3D9::GetAdapterDisplayMode"); return m_real->GetAdapterDisplayMode(Adapter, pMode); }
    STDMETHOD(CheckDeviceType)(UINT Adapter,D3DDEVTYPE DevType,D3DFORMAT AdapterFormat,D3DFORMAT BackBufferFormat,BOOL bWindowed) override { GU_LOG_CALL("IDirect3D9::CheckDeviceType"); return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    STDMETHOD(CheckDeviceFormat)(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,DWORD Usage,D3DRESOURCETYPE RType,D3DFORMAT CheckFormat) override { GU_LOG_CALL("IDirect3D9::CheckDeviceFormat"); return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SurfaceFormat,BOOL Windowed,D3DMULTISAMPLE_TYPE MultiSampleType,DWORD* pQualityLevels) override { GU_LOG_CALL("IDirect3D9::CheckDeviceMultiSampleType"); return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    STDMETHOD(CheckDepthStencilMatch)(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,D3DFORMAT RenderTargetFormat,D3DFORMAT DepthStencilFormat) override { GU_LOG_CALL("IDirect3D9::CheckDepthStencilMatch"); return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SourceFormat,D3DFORMAT TargetFormat) override { GU_LOG_CALL("IDirect3D9::CheckDeviceFormatConversion"); return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    STDMETHOD(GetDeviceCaps)(UINT Adapter,D3DDEVTYPE DeviceType,D3DCAPS9* pCaps) override { GU_LOG_CALL("IDirect3D9::GetDeviceCaps"); return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) override { GU_LOG_CALL("IDirect3D9::GetAdapterMonitor"); return m_real->GetAdapterMonitor(Adapter); }
    STDMETHOD(CreateDevice)(UINT Adapter,D3DDEVTYPE DeviceType,HWND hFocusWindow,DWORD BehaviorFlags,D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DDevice9** ppReturnedDeviceInterface) override { GU_LOG_CALL("IDirect3D9::CreateDevice"); return m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface); }
};

class Device9Forward : public IDirect3DDevice9
{
protected:
    IDirect3DDevice9* m_real;

public:
    explicit Device9Forward(IDirect3DDevice9* real) : m_real(real) {}
    virtual ~Device9Forward() {}
    IDirect3DDevice9* real() const { return m_real; }

    STDMETHOD(TestCooperativeLevel)() override { GU_FIRST_CALL("IDirect3DDevice9::TestCooperativeLevel"); return m_real->TestCooperativeLevel(); }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { GU_FIRST_CALL("IDirect3DDevice9::GetAvailableTextureMem"); return m_real->GetAvailableTextureMem(); }
    STDMETHOD(EvictManagedResources)() override { GU_FIRST_CALL("IDirect3DDevice9::EvictManagedResources"); return m_real->EvictManagedResources(); }
    STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9) override { GU_FIRST_CALL("IDirect3DDevice9::GetDirect3D"); return m_real->GetDirect3D(ppD3D9); }
    STDMETHOD(GetDeviceCaps)(D3DCAPS9* pCaps) override { GU_FIRST_CALL("IDirect3DDevice9::GetDeviceCaps"); return m_real->GetDeviceCaps(pCaps); }
    STDMETHOD(GetDisplayMode)(UINT iSwapChain,D3DDISPLAYMODE* pMode) override { GU_FIRST_CALL("IDirect3DDevice9::GetDisplayMode"); return m_real->GetDisplayMode(iSwapChain, pMode); }
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { GU_FIRST_CALL("IDirect3DDevice9::GetCreationParameters"); return m_real->GetCreationParameters(pParameters); }
    STDMETHOD(SetCursorProperties)(UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap) override { GU_FIRST_CALL("IDirect3DDevice9::SetCursorProperties"); return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
    STDMETHOD_(void, SetCursorPosition)(int X,int Y,DWORD Flags) override { GU_FIRST_CALL("IDirect3DDevice9::SetCursorPosition"); m_real->SetCursorPosition(X, Y, Flags); }
    STDMETHOD_(BOOL, ShowCursor)(BOOL bShow) override { GU_FIRST_CALL("IDirect3DDevice9::ShowCursor"); return m_real->ShowCursor(bShow); }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain) override { GU_FIRST_CALL("IDirect3DDevice9::CreateAdditionalSwapChain"); return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
    STDMETHOD(GetSwapChain)(UINT iSwapChain,IDirect3DSwapChain9** pSwapChain) override { GU_FIRST_CALL("IDirect3DDevice9::GetSwapChain"); return m_real->GetSwapChain(iSwapChain, pSwapChain); }
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { GU_FIRST_CALL("IDirect3DDevice9::GetNumberOfSwapChains"); return m_real->GetNumberOfSwapChains(); }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override { GU_FIRST_CALL("IDirect3DDevice9::Reset"); return m_real->Reset(pPresentationParameters); }
    STDMETHOD(Present)(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion) override { GU_FIRST_CALL("IDirect3DDevice9::Present"); return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion); }
    STDMETHOD(GetBackBuffer)(UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer) override { GU_FIRST_CALL("IDirect3DDevice9::GetBackBuffer"); return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    STDMETHOD(GetRasterStatus)(UINT iSwapChain,D3DRASTER_STATUS* pRasterStatus) override { GU_FIRST_CALL("IDirect3DDevice9::GetRasterStatus"); return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
    STDMETHOD(SetDialogBoxMode)(BOOL bEnableDialogs) override { GU_FIRST_CALL("IDirect3DDevice9::SetDialogBoxMode"); return m_real->SetDialogBoxMode(bEnableDialogs); }
    STDMETHOD_(void, SetGammaRamp)(UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp) override { GU_FIRST_CALL("IDirect3DDevice9::SetGammaRamp"); m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }
    STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain,D3DGAMMARAMP* pRamp) override { GU_FIRST_CALL("IDirect3DDevice9::GetGammaRamp"); m_real->GetGammaRamp(iSwapChain, pRamp); }
    STDMETHOD(CreateTexture)(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateTexture"); return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
    STDMETHOD(CreateVolumeTexture)(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateVolumeTexture"); return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateCubeTexture"); return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
    STDMETHOD(CreateVertexBuffer)(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateVertexBuffer"); return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
    STDMETHOD(CreateIndexBuffer)(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateIndexBuffer"); return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
    STDMETHOD(CreateRenderTarget)(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateRenderTarget"); return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
    STDMETHOD(CreateDepthStencilSurface)(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateDepthStencilSurface"); return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
    STDMETHOD(UpdateSurface)(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestinationSurface,CONST POINT* pDestPoint) override { GU_FIRST_CALL("IDirect3DDevice9::UpdateSurface"); return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* pSourceTexture,IDirect3DBaseTexture9* pDestinationTexture) override { GU_FIRST_CALL("IDirect3DDevice9::UpdateTexture"); return m_real->UpdateTexture(pSourceTexture, pDestinationTexture); }
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* pRenderTarget,IDirect3DSurface9* pDestSurface) override { GU_FIRST_CALL("IDirect3DDevice9::GetRenderTargetData"); return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }
    STDMETHOD(GetFrontBufferData)(UINT iSwapChain,IDirect3DSurface9* pDestSurface) override { GU_FIRST_CALL("IDirect3DDevice9::GetFrontBufferData"); return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }
    STDMETHOD(StretchRect)(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter) override { GU_FIRST_CALL("IDirect3DDevice9::StretchRect"); return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
    STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color) override { GU_FIRST_CALL("IDirect3DDevice9::ColorFill"); return m_real->ColorFill(pSurface, pRect, color); }
    STDMETHOD(CreateOffscreenPlainSurface)(UINT Width,UINT Height,D3DFORMAT Format,D3DPOOL Pool,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { GU_FIRST_CALL("IDirect3DDevice9::CreateOffscreenPlainSurface"); return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
    STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget) override { GU_FIRST_CALL("IDirect3DDevice9::SetRenderTarget"); return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget); }
    STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex,IDirect3DSurface9** ppRenderTarget) override { GU_FIRST_CALL("IDirect3DDevice9::GetRenderTarget"); return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) override { GU_FIRST_CALL("IDirect3DDevice9::SetDepthStencilSurface"); return m_real->SetDepthStencilSurface(pNewZStencil); }
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface) override { GU_FIRST_CALL("IDirect3DDevice9::GetDepthStencilSurface"); return m_real->GetDepthStencilSurface(ppZStencilSurface); }
    STDMETHOD(BeginScene)() override { GU_FIRST_CALL("IDirect3DDevice9::BeginScene"); return m_real->BeginScene(); }
    STDMETHOD(EndScene)() override { GU_FIRST_CALL("IDirect3DDevice9::EndScene"); return m_real->EndScene(); }
    STDMETHOD(Clear)(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil) override { GU_FIRST_CALL("IDirect3DDevice9::Clear"); return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix) override { GU_FIRST_CALL("IDirect3DDevice9::SetTransform"); return m_real->SetTransform(State, pMatrix); }
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State,D3DMATRIX* pMatrix) override { GU_FIRST_CALL("IDirect3DDevice9::GetTransform"); return m_real->GetTransform(State, pMatrix); }
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE p0,CONST D3DMATRIX* p1) override { GU_FIRST_CALL("IDirect3DDevice9::MultiplyTransform"); return m_real->MultiplyTransform(p0, p1); }
    STDMETHOD(SetViewport)(CONST D3DVIEWPORT9* pViewport) override { GU_FIRST_CALL("IDirect3DDevice9::SetViewport"); return m_real->SetViewport(pViewport); }
    STDMETHOD(GetViewport)(D3DVIEWPORT9* pViewport) override { GU_FIRST_CALL("IDirect3DDevice9::GetViewport"); return m_real->GetViewport(pViewport); }
    STDMETHOD(SetMaterial)(CONST D3DMATERIAL9* pMaterial) override { GU_FIRST_CALL("IDirect3DDevice9::SetMaterial"); return m_real->SetMaterial(pMaterial); }
    STDMETHOD(GetMaterial)(D3DMATERIAL9* pMaterial) override { GU_FIRST_CALL("IDirect3DDevice9::GetMaterial"); return m_real->GetMaterial(pMaterial); }
    STDMETHOD(SetLight)(DWORD Index,CONST D3DLIGHT9* p1) override { GU_FIRST_CALL("IDirect3DDevice9::SetLight"); return m_real->SetLight(Index, p1); }
    STDMETHOD(GetLight)(DWORD Index,D3DLIGHT9* p1) override { GU_FIRST_CALL("IDirect3DDevice9::GetLight"); return m_real->GetLight(Index, p1); }
    STDMETHOD(LightEnable)(DWORD Index,BOOL Enable) override { GU_FIRST_CALL("IDirect3DDevice9::LightEnable"); return m_real->LightEnable(Index, Enable); }
    STDMETHOD(GetLightEnable)(DWORD Index,BOOL* pEnable) override { GU_FIRST_CALL("IDirect3DDevice9::GetLightEnable"); return m_real->GetLightEnable(Index, pEnable); }
    STDMETHOD(SetClipPlane)(DWORD Index,CONST float* pPlane) override { GU_FIRST_CALL("IDirect3DDevice9::SetClipPlane"); return m_real->SetClipPlane(Index, pPlane); }
    STDMETHOD(GetClipPlane)(DWORD Index,float* pPlane) override { GU_FIRST_CALL("IDirect3DDevice9::GetClipPlane"); return m_real->GetClipPlane(Index, pPlane); }
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State,DWORD Value) override { GU_FIRST_CALL("IDirect3DDevice9::SetRenderState"); return m_real->SetRenderState(State, Value); }
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State,DWORD* pValue) override { GU_FIRST_CALL("IDirect3DDevice9::GetRenderState"); return m_real->GetRenderState(State, pValue); }
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type,IDirect3DStateBlock9** ppSB) override { GU_FIRST_CALL("IDirect3DDevice9::CreateStateBlock"); return m_real->CreateStateBlock(Type, ppSB); }
    STDMETHOD(BeginStateBlock)() override { GU_FIRST_CALL("IDirect3DDevice9::BeginStateBlock"); return m_real->BeginStateBlock(); }
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** ppSB) override { GU_FIRST_CALL("IDirect3DDevice9::EndStateBlock"); return m_real->EndStateBlock(ppSB); }
    STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS9* pClipStatus) override { GU_FIRST_CALL("IDirect3DDevice9::SetClipStatus"); return m_real->SetClipStatus(pClipStatus); }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9* pClipStatus) override { GU_FIRST_CALL("IDirect3DDevice9::GetClipStatus"); return m_real->GetClipStatus(pClipStatus); }
    STDMETHOD(GetTexture)(DWORD Stage,IDirect3DBaseTexture9** ppTexture) override { GU_FIRST_CALL("IDirect3DDevice9::GetTexture"); return m_real->GetTexture(Stage, ppTexture); }
    STDMETHOD(SetTexture)(DWORD Stage,IDirect3DBaseTexture9* pTexture) override { GU_FIRST_CALL("IDirect3DDevice9::SetTexture"); return m_real->SetTexture(Stage, pTexture); }
    STDMETHOD(GetTextureStageState)(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD* pValue) override { GU_FIRST_CALL("IDirect3DDevice9::GetTextureStageState"); return m_real->GetTextureStageState(Stage, Type, pValue); }
    STDMETHOD(SetTextureStageState)(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value) override { GU_FIRST_CALL("IDirect3DDevice9::SetTextureStageState"); return m_real->SetTextureStageState(Stage, Type, Value); }
    STDMETHOD(GetSamplerState)(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD* pValue) override { GU_FIRST_CALL("IDirect3DDevice9::GetSamplerState"); return m_real->GetSamplerState(Sampler, Type, pValue); }
    STDMETHOD(SetSamplerState)(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value) override { GU_FIRST_CALL("IDirect3DDevice9::SetSamplerState"); return m_real->SetSamplerState(Sampler, Type, Value); }
    STDMETHOD(ValidateDevice)(DWORD* pNumPasses) override { GU_FIRST_CALL("IDirect3DDevice9::ValidateDevice"); return m_real->ValidateDevice(pNumPasses); }
    STDMETHOD(SetPaletteEntries)(UINT PaletteNumber,CONST PALETTEENTRY* pEntries) override { GU_FIRST_CALL("IDirect3DDevice9::SetPaletteEntries"); return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
    STDMETHOD(GetPaletteEntries)(UINT PaletteNumber,PALETTEENTRY* pEntries) override { GU_FIRST_CALL("IDirect3DDevice9::GetPaletteEntries"); return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
    STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber) override { GU_FIRST_CALL("IDirect3DDevice9::SetCurrentTexturePalette"); return m_real->SetCurrentTexturePalette(PaletteNumber); }
    STDMETHOD(GetCurrentTexturePalette)(UINT *PaletteNumber) override { GU_FIRST_CALL("IDirect3DDevice9::GetCurrentTexturePalette"); return m_real->GetCurrentTexturePalette(PaletteNumber); }
    STDMETHOD(SetScissorRect)(CONST RECT* pRect) override { GU_FIRST_CALL("IDirect3DDevice9::SetScissorRect"); return m_real->SetScissorRect(pRect); }
    STDMETHOD(GetScissorRect)(RECT* pRect) override { GU_FIRST_CALL("IDirect3DDevice9::GetScissorRect"); return m_real->GetScissorRect(pRect); }
    STDMETHOD(SetSoftwareVertexProcessing)(BOOL bSoftware) override { GU_FIRST_CALL("IDirect3DDevice9::SetSoftwareVertexProcessing"); return m_real->SetSoftwareVertexProcessing(bSoftware); }
    STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() override { GU_FIRST_CALL("IDirect3DDevice9::GetSoftwareVertexProcessing"); return m_real->GetSoftwareVertexProcessing(); }
    STDMETHOD(SetNPatchMode)(float nSegments) override { GU_FIRST_CALL("IDirect3DDevice9::SetNPatchMode"); return m_real->SetNPatchMode(nSegments); }
    STDMETHOD_(float, GetNPatchMode)() override { GU_FIRST_CALL("IDirect3DDevice9::GetNPatchMode"); return m_real->GetNPatchMode(); }
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount) override { GU_FIRST_CALL("IDirect3DDevice9::DrawPrimitive"); return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE p0,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount) override { GU_FIRST_CALL("IDirect3DDevice9::DrawIndexedPrimitive"); return m_real->DrawIndexedPrimitive(p0, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { GU_FIRST_CALL("IDirect3DDevice9::DrawPrimitiveUP"); return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { GU_FIRST_CALL("IDirect3DDevice9::DrawIndexedPrimitiveUP"); return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
    STDMETHOD(ProcessVertices)(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer9* pDestBuffer,IDirect3DVertexDeclaration9* pVertexDecl,DWORD Flags) override { GU_FIRST_CALL("IDirect3DDevice9::ProcessVertices"); return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
    STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9* pVertexElements,IDirect3DVertexDeclaration9** ppDecl) override { GU_FIRST_CALL("IDirect3DDevice9::CreateVertexDeclaration"); return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl) override { GU_FIRST_CALL("IDirect3DDevice9::SetVertexDeclaration"); return m_real->SetVertexDeclaration(pDecl); }
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl) override { GU_FIRST_CALL("IDirect3DDevice9::GetVertexDeclaration"); return m_real->GetVertexDeclaration(ppDecl); }
    STDMETHOD(SetFVF)(DWORD FVF) override { GU_FIRST_CALL("IDirect3DDevice9::SetFVF"); return m_real->SetFVF(FVF); }
    STDMETHOD(GetFVF)(DWORD* pFVF) override { GU_FIRST_CALL("IDirect3DDevice9::GetFVF"); return m_real->GetFVF(pFVF); }
    STDMETHOD(CreateVertexShader)(CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader) override { GU_FIRST_CALL("IDirect3DDevice9::CreateVertexShader"); return m_real->CreateVertexShader(pFunction, ppShader); }
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* pShader) override { GU_FIRST_CALL("IDirect3DDevice9::SetVertexShader"); return m_real->SetVertexShader(pShader); }
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** ppShader) override { GU_FIRST_CALL("IDirect3DDevice9::GetVertexShader"); return m_real->GetVertexShader(ppShader); }
    STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetVertexShaderConstantF"); return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetVertexShaderConstantF"); return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetVertexShaderConstantI"); return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetVertexShaderConstantI"); return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetVertexShaderConstantB"); return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetVertexShaderConstantB"); return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    STDMETHOD(SetStreamSource)(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride) override { GU_FIRST_CALL("IDirect3DDevice9::SetStreamSource"); return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
    STDMETHOD(GetStreamSource)(UINT StreamNumber,IDirect3DVertexBuffer9** ppStreamData,UINT* pOffsetInBytes,UINT* pStride) override { GU_FIRST_CALL("IDirect3DDevice9::GetStreamSource"); return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
    STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber,UINT Setting) override { GU_FIRST_CALL("IDirect3DDevice9::SetStreamSourceFreq"); return m_real->SetStreamSourceFreq(StreamNumber, Setting); }
    STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber,UINT* pSetting) override { GU_FIRST_CALL("IDirect3DDevice9::GetStreamSourceFreq"); return m_real->GetStreamSourceFreq(StreamNumber, pSetting); }
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* pIndexData) override { GU_FIRST_CALL("IDirect3DDevice9::SetIndices"); return m_real->SetIndices(pIndexData); }
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** ppIndexData) override { GU_FIRST_CALL("IDirect3DDevice9::GetIndices"); return m_real->GetIndices(ppIndexData); }
    STDMETHOD(CreatePixelShader)(CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader) override { GU_FIRST_CALL("IDirect3DDevice9::CreatePixelShader"); return m_real->CreatePixelShader(pFunction, ppShader); }
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* pShader) override { GU_FIRST_CALL("IDirect3DDevice9::SetPixelShader"); return m_real->SetPixelShader(pShader); }
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ppShader) override { GU_FIRST_CALL("IDirect3DDevice9::GetPixelShader"); return m_real->GetPixelShader(ppShader); }
    STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetPixelShaderConstantF"); return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetPixelShaderConstantF"); return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetPixelShaderConstantI"); return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetPixelShaderConstantI"); return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount) override { GU_FIRST_CALL("IDirect3DDevice9::SetPixelShaderConstantB"); return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { GU_FIRST_CALL("IDirect3DDevice9::GetPixelShaderConstantB"); return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    STDMETHOD(DrawRectPatch)(UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo) override { GU_FIRST_CALL("IDirect3DDevice9::DrawRectPatch"); return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
    STDMETHOD(DrawTriPatch)(UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo) override { GU_FIRST_CALL("IDirect3DDevice9::DrawTriPatch"); return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
    STDMETHOD(DeletePatch)(UINT Handle) override { GU_FIRST_CALL("IDirect3DDevice9::DeletePatch"); return m_real->DeletePatch(Handle); }
    STDMETHOD(CreateQuery)(D3DQUERYTYPE Type,IDirect3DQuery9** ppQuery) override { GU_FIRST_CALL("IDirect3DDevice9::CreateQuery"); return m_real->CreateQuery(Type, ppQuery); }
};
