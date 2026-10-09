#include "proxy_device.h"
#include "proxy_d3d9.h"
#include "overlay.h"

#include <cstdio>

void LogPresentParameters(const char* what, const D3DPRESENT_PARAMETERS* pp);

namespace
{
const char* PrimName(D3DPRIMITIVETYPE t)
{
    switch (t)
    {
    case D3DPT_POINTLIST: return "points";
    case D3DPT_LINELIST: return "lines";
    case D3DPT_LINESTRIP: return "linestrip";
    case D3DPT_TRIANGLELIST: return "tris";
    case D3DPT_TRIANGLESTRIP: return "tristrip";
    case D3DPT_TRIANGLEFAN: return "trifan";
    default: return "?";
    }
}

// Length in bytes of a D3D9 shader token stream, up to and including the end token. Instruction tokens are
// walked one dword at a time (operand tokens always have bit 31 set, so they can't look like the end token);
// comment blocks, which can hold anything, are skipped by their length.
size_t ShaderSize(const DWORD* function)
{
    for (size_t i = 1; i < (1u << 18);)
    {
        DWORD token = function[i];
        if (token == 0x0000FFFF)
            return (i + 1) * sizeof(DWORD);
        if ((token & 0xFFFF) == 0xFFFE)
            i += 1 + ((token >> 16) & 0x7FFF);
        else
            ++i;
    }
    return 0;
}

std::string ShaderVersion(DWORD token)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%s_%u_%u", (token >> 16) == 0xFFFE ? "vs" : (token >> 16) == 0xFFFF ? "ps" : "??",
             (token >> 8) & 0xFF, token & 0xFF);
    return buf;
}

std::string Rect(const RECT* r)
{
    if (!r)
        return "null";
    char buf[64];
    snprintf(buf, sizeof(buf), "[%ld,%ld,%ld,%ld]", r->left, r->top, r->right, r->bottom);
    return buf;
}
}

ProxyDevice::ProxyDevice(IDirect3DDevice9* real, ProxyD3D9* parent)
    : Device9Forward(real), m_parent(parent), m_trace(m_registry), m_refl(real), m_water(real), m_shadows(real), m_light(real), m_terrain(real)
{
    m_effects = EffectsAllowed();
    m_refl.enabled = m_effects && g_config.reflectionEnabled;
    m_refl.overlay = m_effects && g_config.overlayEnabled;
    m_water.enabled = m_effects && g_config.waterEnabled;
    m_shadows.enabled = m_effects && g_config.shadowsEnabled;
    m_shadows.overlay = m_effects && g_config.shadowOverlay;
    m_light.enabled = m_effects && g_config.lightingEnabled;
    if (m_light.enabled)
    {
        const float* s = g_config.lightSun[kLightUnit];
        const float* a = g_config.lightAmbient[kLightUnit];
        Log::Write("lighting on from start (unit sun %.0f,%.0f,%.0f, ambient %.0f,%.0f,%.0f)", s[0] * 255, s[1] * 255,
                   s[2] * 255, a[0] * 255, a[1] * 255, a[2] * 255);
    }
    NewLightFrame();  // the first frame too
    if (m_shadows.enabled)
        Log::Write("sun shadows on from start (size %u, max radius %g)", g_config.shadowSize, g_config.shadowMaxRadius);
    m_refl.needed = m_water.enabled || m_refl.overlay;
    if (!m_effects)
        Log::Write("effects off in this game (game id %s not in Games=%ls); passing everything through",
                   Log::GameId().c_str(), g_config.games.c_str());
    if (m_water.enabled)
        Log::Write("Reflective, dynamic water on from start (%ls)", g_config.colonizationDir.c_str());
    if (m_refl.enabled)
        Log::Write("reflection on from start (size %u, water height %g)", g_config.reflectionSize, g_config.waterHeight);
    D3DCAPS9 caps = {};
    if (SUCCEEDED(real->GetDeviceCaps(&caps)) && caps.MaxVertexShaderConst > 0)
        m_vsRegisters = caps.MaxVertexShaderConst < 256 ? caps.MaxVertexShaderConst : 256;
    Log::Write("device wrapped: VS %u.%u (%u float constants), PS %u.%u, max RTs %u, max texture %ux%u",
               (caps.VertexShaderVersion >> 8) & 0xFF, caps.VertexShaderVersion & 0xFF, caps.MaxVertexShaderConst,
               (caps.PixelShaderVersion >> 8) & 0xFF, caps.PixelShaderVersion & 0xFF, caps.NumSimultaneousRTs,
               caps.MaxTextureWidth, caps.MaxTextureHeight);
    if (g_config.dumpShaders)
        EnsureDir(Log::GameDir() + L"\\shaders");
    m_ghost = m_effects && g_config.ghostEnabled;
    if (m_ghost)
        Log::Write("ghost on from start (offset %g,%g,%g)", g_config.ghostOffset[0], g_config.ghostOffset[1],
                   g_config.ghostOffset[2]);
    std::string modifiers = std::string(g_config.keyCtrl ? "ctrl+" : "") + (g_config.keyShift ? "shift+" : "") +
                            (g_config.keyAlt ? "alt+" : "");
    Log::Write("hotkeys (%skey): water %s, shadows %s, lighting %s, light cycle %s, reflection %s, reflection "
               "overlay %s, shadow overlay %s, ghost %s, trace %s",
               modifiers.c_str(),
               KeyName(g_config.waterKey).c_str(), KeyName(g_config.shadowsKey).c_str(),
               KeyName(g_config.lightingKey).c_str(), KeyName(g_config.lightCycleKey).c_str(),
               KeyName(g_config.reflectionKey).c_str(),
               KeyName(g_config.overlayKey).c_str(), KeyName(g_config.shadowOverlayKey).c_str(),
               KeyName(g_config.ghostKey).c_str(), g_config.tracesEnabled ? KeyName(g_config.traceKey).c_str() : "off");
    m_requestFile = Log::GameDir() + L"\\trace.request";
    DeleteFileW(m_requestFile.c_str());  // a stale request from an earlier session shouldn't fire at startup
}

HRESULT ProxyDevice::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9)
    {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    Log::Write("IDirect3DDevice9::QueryInterface %s -> 0x%08x (passed through unwrapped)", Guid(riid).c_str(), static_cast<unsigned>(hr));
    return hr;
}

ULONG ProxyDevice::Release()
{
    ULONG refs = m_real->Release();
    if (refs != 0 && refs == m_refl.InternalRefs() + m_water.InternalRefs() + m_shadows.InternalRefs() + m_deferRefs +
                                 m_light.InternalRefs() + m_terrain.InternalRefs())
    {
        // Only our own objects still hold the device: the game has let go, so release them, which destroys the
        // device, and the wrapper with it.
        DropDeferred();  // first: it reads the device's reference count, so the device must still be alive
        m_refl.ReleaseResources();
        m_water.ReleaseAll();
        m_shadows.ReleaseAll();
        m_light.ReleaseAll();  // last: these don't read the device's reference count
        m_terrain.ReleaseAll();
        refs = 0;
    }
    if (refs == 0)
    {
        Log::Write("device released after %u frames", m_frame);
        delete this;
    }
    return refs;
}

HRESULT ProxyDevice::GetDirect3D(IDirect3D9** ppD3D9)
{
    if (!ppD3D9)
        return D3DERR_INVALIDCALL;
    m_parent->AddRef();
    *ppD3D9 = m_parent;
    return D3D_OK;
}

HRESULT ProxyDevice::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    LogPresentParameters("Reset", pPresentationParameters);
    m_water.ReleaseDefault();
    m_shadows.ReleaseDefault();
    DropDeferred();  // state blocks must go before Reset
    m_refl.ReleaseResources();  // D3DPOOL_DEFAULT objects and state blocks must be gone before Reset; made again on use
    HRESULT hr = m_real->Reset(pPresentationParameters);
    Log::Write("Reset -> 0x%08x", static_cast<unsigned>(hr));
    return hr;
}

HRESULT ProxyDevice::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride,
                             CONST RGNDATA* pDirtyRegion)
{
    GU_FIRST_CALL("IDirect3DDevice9::Present");
    if (m_trace.Active() && m_refl.SaveBmp(m_trace.Dir() + L"\\reflection.bmp"))
        m_trace.Event("{\"ev\":\"reflection_saved\",\"file\":\"reflection.bmp\"}");
    if (m_trace.Active() && m_water.SaveRefractionBmp(m_trace.Dir() + L"\\refraction.bmp"))
        m_trace.Event("{\"ev\":\"refraction_saved\",\"file\":\"refraction.bmp\"}");
    if (m_trace.Active() && m_shadows.SaveBmps(m_trace.Dir()))
        m_trace.Event("{\"ev\":\"shadows_saved\"}");
    m_shadows.EndFrame();
    m_refl.DrawOverlay();
    m_shadows.DrawOverlay();
    HRESULT hr = m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    ++m_frame;
    if (m_frame % 1000 == 0)
        Log::Write("frame %u: %u draws, %u primitives, %u render-target switches, %u ghost redraws, %u reflection "
                   "redraws, %u reflective, dynamic water draws, %u shadow casters, %u shadowed ground draws, %u blob "
                   "shadows skipped, %u painted shadows skipped, %u selection decals moved above the water",
                   m_frame, m_frameDraws, m_framePrims, m_frameRtSwitches, m_frameGhosts, m_refl.FrameRedraws(),
                   m_frameWaterDraws, m_shadows.FrameCasters(), m_shadows.FrameReceivers(), m_frameBlobsSkipped, m_framePaintedSkipped, m_frameDeferred);
    m_frameDraws = m_frameRtSwitches = m_framePrims = m_frameGhosts = m_frameWaterDraws = m_frameBlobsSkipped = m_framePaintedSkipped = m_frameDeferred = 0;
    m_refl.NewFrame();
    m_water.NewFrame();
    m_shadows.NewFrame();
    m_terrain.NewFrame();
    NewLightFrame();

    m_trace.FrameDone();

    // Hotkeys: exactly the modifiers plus a key, edge-triggered, only while the game's window is in front. A key of 0
    // is disabled. Alt is the left Alt only: AltGr (right Alt, which Windows reports as Ctrl+Alt) types characters on
    // many keyboards. Effect toggles only where effects are allowed; traces only when [debug] Traces=1.
    auto down = [](int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool inFront = foregroundPid == GetCurrentProcessId();
    const bool ctrl = down(VK_CONTROL), shift = down(VK_SHIFT), alt = down(VK_LMENU) && !down(VK_RMENU);
    auto combo = [&](UINT key, bool& wasDown) {
        bool isDown = inFront && down(static_cast<int>(key)) && ctrl == g_config.keyCtrl &&
                      shift == g_config.keyShift && alt == g_config.keyAlt;
        bool pressed = isDown && !wasDown;
        wasDown = isDown;
        return pressed;
    };
    bool pressed = g_config.tracesEnabled && combo(g_config.traceKey, m_keyWasDown);
    if (pressed)
        Log::Write("trace key pressed at frame %u", m_frame);
    if (!m_effects)
    {
        // effects not allowed in this game: no toggles (traces above still work for debugging)
    }
    else if (combo(g_config.ghostKey, m_ghostKeyWasDown))
    {
        m_ghost = !m_ghost;
        Log::Write("ghost %s at frame %u (offset %g,%g,%g)", m_ghost ? "on" : "off", m_frame, g_config.ghostOffset[0],
                   g_config.ghostOffset[1], g_config.ghostOffset[2]);
    }
    if (m_effects && combo(g_config.reflectionKey, m_reflKeyWasDown))
    {
        m_refl.enabled = !m_refl.enabled;
        Log::Write("reflection %s at frame %u", m_refl.enabled ? "on" : "off", m_frame);
    }
    if (m_effects && combo(g_config.waterKey, m_waterKeyWasDown))
    {
        m_water.enabled = !m_water.enabled;
        Log::Write("Reflective, dynamic water %s at frame %u", m_water.enabled ? "on" : "off", m_frame);
    }
    if (m_effects && combo(g_config.overlayKey, m_overlayKeyWasDown))
    {
        m_refl.overlay = !m_refl.overlay;
        Log::Write("reflection overlay %s at frame %u", m_refl.overlay ? "on" : "off", m_frame);
    }
    if (m_effects && combo(g_config.shadowsKey, m_shadowsKeyWasDown))
    {
        m_shadows.enabled = !m_shadows.enabled;
        Log::Write("sun shadows %s at frame %u", m_shadows.enabled ? "on" : "off", m_frame);
    }
    if (m_effects && combo(g_config.lightingKey, m_lightKeyWasDown))
    {
        m_light.enabled = !m_light.enabled;
        Log::Write("lighting %s at frame %u", m_light.enabled ? "on" : "off", m_frame);
    }
    if (m_effects && combo(g_config.lightCycleKey, m_lightCycleKeyWasDown))
    {
        m_light.cyclePaused = !m_light.cyclePaused;
        Log::Write("light cycle %s at frame %u (step %.2f)", m_light.cyclePaused ? "paused" : "playing", m_frame,
                   m_light.CyclePos());
    }
    if (m_effects && combo(g_config.shadowOverlayKey, m_shadowOverlayKeyWasDown))
    {
        m_shadows.overlay = !m_shadows.overlay;
        Log::Write("shadow overlay %s at frame %u", m_shadows.overlay ? "on" : "off", m_frame);
    }
    // The reflection pass only runs when something uses it: the water, or the debug overlay.
    m_refl.needed = m_water.enabled || m_refl.overlay;
    bool automatic = g_config.tracesEnabled && g_config.autoTraceFrame > 0 &&
                     m_frame == static_cast<uint32_t>(g_config.autoTraceFrame);
    // Trigger file: <output>/<game>/trace.request (checked every 10 frames), for keyboards that swallow the hotkey
    // and for starting traces from outside the game.
    bool requested = false;
    if (g_config.tracesEnabled && m_frame % 10 == 0 &&
        GetFileAttributesW(m_requestFile.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileW(m_requestFile.c_str());
        requested = true;
        Log::Write("trace requested by %ls at frame %u", m_requestFile.c_str(), m_frame);
    }
    if ((pressed || automatic || requested) && !m_trace.Active())
    {
        m_trace.Begin(m_frame, g_config.traceFrames, m_vsRegisters);  // everything after this Present
        if (m_light.CycleActive())
            Log::Write("trace: lighting on, light colour cycle at step %.2f, %s", m_light.CyclePos(),
                       m_light.Describe().c_str());
        else
            Log::Write("trace: lighting %s%s", m_light.enabled ? "on" : "off",
                       g_config.lightCycleOn ? " (the light colour cycle isn't applied)" : "");
    }
    return hr;
}

// The light cycle moves on, and what it drives follows: the shadows' sun and darkness, and the water's sun.
void ProxyDevice::NewLightFrame()
{
    m_light.NewFrame();
    m_light.hidePaintedTreeShadows = m_shadows.enabled;
    m_shadows.sunDirection = m_light.SunDirection();
    m_shadows.darknessScale = m_light.ShadowScale();
    m_water.sunColour = m_light.WaterSun();  // the lighting's (the moon's glint at night), or Colonization's
}

HRESULT ProxyDevice::BeginScene()
{
    GU_FIRST_CALL("IDirect3DDevice9::BeginScene");
    m_trace.Event("{\"ev\":\"begin_scene\"}");
    return m_real->BeginScene();
}

HRESULT ProxyDevice::EndScene()
{
    ReplayDeferred();  // normally already replayed after the water; never carried past the scene
    m_trace.Event("{\"ev\":\"end_scene\"}");
    return m_real->EndScene();
}

HRESULT ProxyDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                   IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
{
    HRESULT hr = m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
    if (SUCCEEDED(hr) && ppTexture && *ppTexture)
        m_shadows.ForgetTexture(*ppTexture);  // a new texture at a possibly reused address
    if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        Log::Write("CreateTexture %ux%u levels %u usage 0x%x fmt %u pool %u -> 0x%08x tex %s", Width, Height, Levels,
                   Usage, Format, Pool, static_cast<unsigned>(hr),
                   SUCCEEDED(hr) && ppTexture ? Ptr(*ppTexture).c_str() : "-");
    return hr;
}

HRESULT ProxyDevice::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                        IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
{
    HRESULT hr = m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    if (SUCCEEDED(hr) && ppVertexBuffer && *ppVertexBuffer)
        m_terrain.ForgetBuffer(*ppVertexBuffer);  // a new buffer at a possibly reused address
    return hr;
}

HRESULT ProxyDevice::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                       IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
{
    HRESULT hr = m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
    Log::Write("CreateCubeTexture %u levels %u usage 0x%x fmt %u pool %u -> 0x%08x tex %s", EdgeLength, Levels, Usage,
               Format, Pool, static_cast<unsigned>(hr),
               SUCCEEDED(hr) && ppCubeTexture ? Ptr(*ppCubeTexture).c_str() : "-");
    return hr;
}

HRESULT ProxyDevice::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                        DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface,
                                        HANDLE* pSharedHandle)
{
    HRESULT hr = m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface,
                                            pSharedHandle);
    Log::Write("CreateRenderTarget %ux%u fmt %u ms %u/%u lockable %d -> 0x%08x surf %s", Width, Height, Format,
               MultiSample, MultisampleQuality, Lockable, static_cast<unsigned>(hr),
               SUCCEEDED(hr) && ppSurface ? Ptr(*ppSurface).c_str() : "-");
    return hr;
}

HRESULT ProxyDevice::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                               D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                               BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{
    HRESULT hr = m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                                   ppSurface, pSharedHandle);
    Log::Write("CreateDepthStencilSurface %ux%u fmt %u ms %u/%u discard %d -> 0x%08x surf %s", Width, Height, Format,
               MultiSample, MultisampleQuality, Discard, static_cast<unsigned>(hr),
               SUCCEEDED(hr) && ppSurface ? Ptr(*ppSurface).c_str() : "-");
    return hr;
}

void ProxyDevice::RegisterShader(const char* kind, const void* shader, const DWORD* function)
{
    size_t size = function ? ShaderSize(function) : 0;
    ShaderInfo info;
    info.version = function ? ShaderVersion(function[0]) : kind;
    info.hash = size ? Fnv1a(function, size) : 0;
    if (size)
        ParseConstantTable(function, size, info);
    m_registry.shaders[shader] = info;
    if (size && g_config.dumpShaders && m_dumpedShaders.insert(info.hash).second)
    {
        std::wstring path = Log::GameDir() + L"\\shaders\\" + Widen(info.version + "_" + Hex64(info.hash)) + L".bin";
        bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!exists)
            WriteBytes(path, function, size);
        Log::Write("new %s %s %s, %zu bytes%s; constants: %s", kind, info.version.c_str(), Hex64(info.hash).c_str(),
                   size, exists ? " (already dumped)" : "", info.constants.empty() ? "-" : info.constants.c_str());
    }
}

HRESULT ProxyDevice::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
{
    HRESULT hr = m_real->CreateVertexShader(pFunction, ppShader);
    if (SUCCEEDED(hr) && ppShader && *ppShader)
        RegisterShader("vertex shader", *ppShader, pFunction);
    return hr;
}

HRESULT ProxyDevice::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{
    HRESULT hr = m_real->CreatePixelShader(pFunction, ppShader);
    if (SUCCEEDED(hr) && ppShader && *ppShader)
        RegisterShader("pixel shader", *ppShader, pFunction);
    return hr;
}

HRESULT ProxyDevice::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements,
                                             IDirect3DVertexDeclaration9** ppDecl)
{
    HRESULT hr = m_real->CreateVertexDeclaration(pVertexElements, ppDecl);
    if (SUCCEEDED(hr) && ppDecl && *ppDecl && pVertexElements)
    {
        size_t n = 0;
        while (pVertexElements[n].Stream != 0xFF && n < 64)
            ++n;
        uint64_t hash = Fnv1a(pVertexElements, n * sizeof(D3DVERTEXELEMENT9));
        m_registry.decls[*ppDecl] = hash;
        if (m_loggedDecls.insert(hash).second)
        {
            // stream.offset type usage index, e.g. "0.0 t2 u0 i0" = stream 0, offset 0, FLOAT3, POSITION0
            std::string elems;
            for (size_t i = 0; i < n; ++i)
            {
                const D3DVERTEXELEMENT9& e = pVertexElements[i];
                char buf[64];
                snprintf(buf, sizeof(buf), "%s%u.%u t%u u%u i%u", i ? ", " : "", e.Stream, e.Offset, e.Type, e.Usage,
                         e.UsageIndex);
                elems += buf;
            }
            Log::Write("new vertex declaration %s: %s", Hex64(hash).c_str(), elems.c_str());
        }
    }
    return hr;
}

HRESULT ProxyDevice::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
{
    ++m_frameRtSwitches;
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"set_rt\",\"i\":" + std::to_string(RenderTargetIndex) +
                      ",\"surf\":" + m_trace.SurfaceRef(pRenderTarget) + "}");
    return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}

HRESULT ProxyDevice::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"set_ds\",\"surf\":" + m_trace.SurfaceRef(pNewZStencil) + "}");
    return m_real->SetDepthStencilSurface(pNewZStencil);
}

HRESULT ProxyDevice::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    if (m_trace.Active())
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"ev\":\"clear\",\"rects\":%u,\"flags\":%u,\"color\":\"%08x\",\"z\":%g,\"stencil\":%u}",
                 Count, Flags, Color, Z, Stencil);
        m_trace.Event(buf);
    }
    return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

HRESULT ProxyDevice::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                                 IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"stretch_rect\",\"src\":" + m_trace.SurfaceRef(pSourceSurface) +
                      ",\"srect\":" + Rect(pSourceRect) + ",\"dst\":" + m_trace.SurfaceRef(pDestSurface) +
                      ",\"drect\":" + Rect(pDestRect) + ",\"filter\":" + std::to_string(Filter) + "}");
    return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
}

HRESULT ProxyDevice::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                                   IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"update_surface\",\"src\":" + m_trace.SurfaceRef(pSourceSurface) +
                      ",\"dst\":" + m_trace.SurfaceRef(pDestinationSurface) + "}");
    return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
}

HRESULT ProxyDevice::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"get_rt_data\",\"src\":" + m_trace.SurfaceRef(pRenderTarget) +
                      ",\"dst\":" + m_trace.SurfaceRef(pDestSurface) + "}");
    return m_real->GetRenderTargetData(pRenderTarget, pDestSurface);
}

HRESULT ProxyDevice::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
{
    if (m_trace.Active())
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%08x", color);
        m_trace.Event("{\"ev\":\"color_fill\",\"surf\":" + m_trace.SurfaceRef(pSurface) + ",\"rect\":" + Rect(pRect) +
                      ",\"color\":\"" + buf + "\"}");
    }
    return m_real->ColorFill(pSurface, pRect, color);
}

HRESULT ProxyDevice::SetClipPlane(DWORD Index, CONST float* pPlane)
{
    if (m_trace.Active() && pPlane)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"ev\":\"clip_plane\",\"i\":%u,\"plane\":[%g,%g,%g,%g]}", Index, pPlane[0],
                 pPlane[1], pPlane[2], pPlane[3]);
        m_trace.Event(buf);
    }
    return m_real->SetClipPlane(Index, pPlane);
}

HRESULT ProxyDevice::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"vs_const_i\",\"start\":" + std::to_string(StartRegister) +
                      ",\"count\":" + std::to_string(Vector4iCount) + "}");
    return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT ProxyDevice::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
{
    if (pLight && m_loggedLights.size() < 64 && m_loggedLights.insert(Fnv1a(pLight, sizeof(*pLight))).second)
    {
        const D3DLIGHT9& l = *pLight;
        Log::Write("fixed-function light %u: type %u diffuse %.3f,%.3f,%.3f ambient %.3f,%.3f,%.3f specular %.3f,%.3f,%.3f "
                   "direction %.3f,%.3f,%.3f",
                   Index, l.Type, l.Diffuse.r, l.Diffuse.g, l.Diffuse.b, l.Ambient.r, l.Ambient.g, l.Ambient.b,
                   l.Specular.r, l.Specular.g, l.Specular.b, l.Direction.x, l.Direction.y, l.Direction.z);
    }
    return m_real->SetLight(Index, pLight);
}

HRESULT ProxyDevice::LightEnable(DWORD Index, BOOL Enable)
{
    if (Enable)
        m_light.NoteLightSlot(Index);
    return m_real->LightEnable(Index, Enable);
}

HRESULT ProxyDevice::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if (State == D3DRS_AMBIENT && m_loggedLights.size() < 64 && m_loggedLights.insert(0xA3B1E7ull << 32 | Value).second)
        Log::Write("fixed-function ambient %08x (%u,%u,%u)", Value, (Value >> 16) & 0xFF, (Value >> 8) & 0xFF,
                   Value & 0xFF);
    return m_real->SetRenderState(State, Value);
}

HRESULT ProxyDevice::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"vs_const_b\",\"start\":" + std::to_string(StartRegister) +
                      ",\"count\":" + std::to_string(BoolCount) + "}");
    return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}

void ProxyDevice::CountDraw(UINT primitives)
{
    ++m_frameDraws;
    m_framePrims += primitives;
}

template <class Redraw>
void ProxyDevice::Ghost(Redraw redraw)
{
    if (!m_ghost)
        return;
    const float* off = g_config.ghostOffset;
    IDirect3DVertexShader9* vs = nullptr;
    m_real->GetVertexShader(&vs);
    if (vs)
    {
        // Shader path (TCiv4Skinning*, TCiv4Mech*): world-space bones, then mtxViewProj. Moving the object by `off` in
        // world space adds (row.xyz . off) to each register's w, since each output component is reg . (pos, 1).
        auto it = m_registry.shaders.find(vs);
        vs->Release();
        if (it == m_registry.shaders.end() || it->second.viewProj < 0 || it->second.worldBones < 0)
            return;
        UINT reg = static_cast<UINT>(it->second.viewProj);
        float vp[16], shifted[16];
        if (FAILED(m_real->GetVertexShaderConstantF(reg, vp, 4)))
            return;
        memcpy(shifted, vp, sizeof(vp));
        for (int r = 0; r < 4; ++r)
            shifted[r * 4 + 3] += vp[r * 4 + 0] * off[0] + vp[r * 4 + 1] * off[1] + vp[r * 4 + 2] * off[2];
        m_real->SetVertexShaderConstantF(reg, shifted, 4);
        redraw();
        m_real->SetVertexShaderConstantF(reg, vp, 4);
        if (m_trace.Active())
            m_trace.Event("{\"ev\":\"ghost\",\"path\":\"vs\",\"reg\":" + std::to_string(reg) + "}");
    }
    else
    {
        // Fixed-function path: only skinned draws (Gamebryo's fixed-function vertex blending: ship rigging, hulls).
        // Row vectors: (p + off) * View = p * View + off * View, so off * View adds to View's translation row.
        DWORD blend = D3DVBF_DISABLE;
        m_real->GetRenderState(D3DRS_VERTEXBLEND, &blend);
        if (blend == D3DVBF_DISABLE)
            return;
        DWORD fvf = 0;
        m_real->GetFVF(&fvf);
        if (!SkinnedLitFvf(fvf))
            return;
        D3DMATRIX view, shifted;
        if (FAILED(m_real->GetTransform(D3DTS_VIEW, &view)))
            return;
        shifted = view;
        for (int c = 0; c < 4; ++c)
            shifted.m[3][c] += off[0] * view.m[0][c] + off[1] * view.m[1][c] + off[2] * view.m[2][c];
        m_real->SetTransform(D3DTS_VIEW, &shifted);
        redraw();
        m_real->SetTransform(D3DTS_VIEW, &view);
        if (m_trace.Active())
            m_trace.Event("{\"ev\":\"ghost\",\"path\":\"ff\",\"vblend\":" + std::to_string(blend) + "}");
    }
    ++m_frameGhosts;
}

template <class Draw>
HRESULT ProxyDevice::Dispatch(Draw draw)
{
    // Water detection must happen before the water draw itself; the ghost and reflection redraws reuse the game's
    // state right after its draw.
    const ShaderInfo* vs = nullptr;
    bool fixedFunction = false;
    if (m_refl.enabled || m_water.enabled || m_shadows.enabled || m_light.enabled)
    {
        IDirect3DVertexShader9* shader = nullptr;
        m_real->GetVertexShader(&shader);
        if (shader)
        {
            auto it = m_registry.shaders.find(shader);
            if (it != m_registry.shaders.end())
                vs = &it->second;
            shader->Release();
        }
        else
            fixedFunction = true;
    }
    if (!m_deferred.empty() && m_frameWaterDraws > 0 && !(vs && vs->btsWater))
        ReplayDeferred();  // the water is done: the held-back selection decals go on top of it
    // INI light colours for this draw and its redraws; the game's values go back when Dispatch returns.
    struct LightScope
    {
        Lighting& light;
        ~LightScope() { light.Restore(); }
    } lightScope{m_light};
    if (m_light.enabled && fixedFunction)
    {
        // Ships' fixed-function parts are vertex-blended; units' rigid parts sit at their blob shadow; the rest of
        // the lit fixed-function meshes are buildings and improvements.
        DWORD blend = D3DVBF_DISABLE, fvf = 0;
        m_real->GetRenderState(D3DRS_VERTEXBLEND, &blend);
        m_real->GetFVF(&fvf);
        int group = blend != D3DVBF_DISABLE ? kLightMech : m_shadows.UnitPart(fvf) ? kLightUnit : kLightBuilding;
        m_light.Apply(nullptr, nullptr, group);
    }
    if (vs && vs->btsTerrain && m_refl.TargetIsBackBuffer())
        m_light.WorldDrawn();  // the light cycle's clock moves on
    if ((m_light.enabled || m_light.hidePaintedTreeShadows) && vs)
    {
        const ShaderInfo* ps = CurrentPixelShader();
        m_light.Apply(vs, ps, -1);
        if (vs->btsTerrain)
        {
            IDirect3DBaseTexture9* lightmap = nullptr;
            m_real->GetTexture(2, &lightmap);
            m_light.CheckLightmap(lightmap);
            if (lightmap)
                lightmap->Release();
        }
    }
    const bool hills = m_shadows.enabled && g_config.hillShadows;
    if ((m_light.enabled || hills) && vs && vs->btsTerrain && m_refl.TargetIsBackBuffer())
        m_terrain.Capture(*vs);  // heights from the terrain's vertex buffers: per-pixel lighting, hill shadows
    if (vs && vs->btsTerrain && m_trace.Active() && m_lightmapSavedFor != m_trace.Dir())
    {
        // Traces keep the terrain lightmap (BtS bakes it at map load), for working out how it was lit.
        IDirect3DBaseTexture9* lightmap = nullptr;
        m_real->GetTexture(2, &lightmap);
        if (SaveTextureBmp(lightmap, m_trace.Dir() + L"\\lightmap.bmp"))
            m_trace.Event("{\"ev\":\"lightmap_saved\"}");
        // ...and the terrain normals read so far (cell (x, y) = world (x, y) x spacing, wrapping; spacing in the log)
        if (m_terrain.SaveHillBmp(m_trace.Dir() + L"\\hill_shadows.bmp"))
            m_trace.Event("{\"ev\":\"hill_shadows_saved\"}");
        if (m_terrain.SaveBmp(m_trace.Dir() + L"\\terrain_normals.bmp"))
            m_trace.Event("{\"ev\":\"terrain_normals_saved\",\"spacing\":" + std::to_string(m_terrain.Spacing()) +
                          ",\"cells\":" + std::to_string(m_terrain.Cells()) + "}");
        if (lightmap)
            lightmap->Release();
        m_lightmapSavedFor = m_trace.Dir();
    }
    IDirect3DTexture9* normals = m_light.enabled && vs && vs->btsTerrain ? m_terrain.Texture() : nullptr;
    m_light.TerrainConstants(m_terrainLight, normals ? m_terrain.Spacing() : 0, m_terrain.Cells());
    m_shadows.terrainLight = m_light.enabled ? m_terrainLight : nullptr;
    m_shadows.terrainNormals = normals;
    // (also for fixed-function draws: roads and river foam take the hill shadows through an extra texture stage)
    m_shadows.hillShadows = hills && (fixedFunction || (vs && Shadows::MaybeReceiver(*vs))) ? m_terrain.HillTexture(m_light.SunDirection(), g_config.shadowDarkness * m_light.ShadowScale())
                                                                                             : nullptr;
    if (m_terrain.Spacing() > 0)
    {
        m_shadows.terrainGrid[0] = 1.0f / (m_terrain.Spacing() * m_terrain.Cells());
        m_shadows.terrainGrid[1] = 0.5f / m_terrain.Cells();
    }
    if (m_refl.enabled)
        m_refl.BeforeDraw(vs);
    if (m_water.enabled)
    {
        // Refraction: copy the screen at the first ship or water draw of the main scene, i.e. after the terrain (the
        // sea floor) and before anything that stands in the water.
        if (!m_water.SnapshotTaken())
        {
            bool shipOrWater = false;
            if (vs)
                shipOrWater = vs->btsWater || (vs->viewProj >= 0 && vs->worldBones >= 0);
            else if (fixedFunction)
            {
                DWORD blend = D3DVBF_DISABLE, fvf = 0;
                m_real->GetRenderState(D3DRS_VERTEXBLEND, &blend);
                m_real->GetFVF(&fvf);
                shipOrWater = blend != D3DVBF_DISABLE && SkinnedLitFvf(fvf);
            }
            if (shipOrWater && m_refl.TargetIsBackBuffer())
                m_water.SnapshotRefraction();
        }
        if (vs && vs->btsWater && m_refl.TargetIsBackBuffer() && m_water.Ready())
        {
            m_water.shadowTexture = m_shadows.LitTexture();  // sun shadows on its surface, in its shader
            m_water.shadowMatrix = m_shadows.LitMatrix();
            m_water.shadowDarkness = m_shadows.Darkness();
            m_water.skyLight = m_light.SkyLight();
            HRESULT hr = m_water.Draw(*vs, CurrentPixelShader(), m_refl.enabled ? m_refl.Texture() : nullptr,
                                      m_refl.WaterHeight(), [&] { return draw(); });
            ++m_frameWaterDraws;
            if (m_trace.Active())
                m_trace.Event("{\"ev\":\"water\"}");
            return hr;
        }
    }
    if (m_shadows.enabled || m_light.enabled)
    {
        if (m_shadows.enabled && fixedFunction && m_shadows.BlobShadow() && g_config.hideBlobShadows)
        {
            ++m_frameBlobsSkipped;
            if (m_trace.Active())
                m_trace.Event("{\"ev\":\"blob_shadow_skipped\"}");
            return D3D_OK;
        }
        // BtS's painted building shadows: hidden while the buildings cast real ones.
        if (m_shadows.enabled && fixedFunction && g_config.buildingShadows && m_shadows.PaintedShadow())
        {
            ++m_framePaintedSkipped;
            if (m_trace.Active())
                m_trace.Event("{\"ev\":\"painted_shadow_skipped\"}");
            return D3D_OK;
        }
        HRESULT hr = D3D_OK;
        if (vs && Shadows::MaybeReceiver(*vs) &&
            m_shadows.DrawReceiver(*vs, CurrentPixelShader(), [&] { return draw(); }, hr))
        {
            if (m_trace.Active())
                m_trace.Event("{\"ev\":\"shadowed_receiver\"}");
            return hr;
        }
        if (fixedFunction && m_shadows.DrawDecal([&] { return draw(); }, hr))
        {
            if (m_trace.Active())
                m_trace.Event("{\"ev\":\"shadowed_decal\"}");
            return hr;
        }
    }
    HRESULT hr = draw();
    if (vs && vs->btsWater)
        m_shadows.ShadeWater([&] { return draw(); });  // BtS's own water (Colonization's off): shadows on it too
    Ghost(draw);
    if (m_refl.enabled && (vs || fixedFunction) && m_refl.AfterDraw(vs, fixedFunction, [&] { draw(); }) &&
        m_trace.Active())
        m_trace.Event(std::string("{\"ev\":\"reflect\",\"path\":\"") + (vs ? "vs" : "ff") + "\"}");
    // Below the surface: ships' and submarines' underwater parts into the refraction, before the water is drawn.
    if (m_water.enabled && m_frameWaterDraws == 0 && m_water.SnapshotTaken() && (vs || fixedFunction))
    {
        bool ship = false;
        if (vs)
            ship = vs->viewProj >= 0 && vs->worldBones >= 0;
        else
        {
            DWORD blend = D3DVBF_DISABLE, fvf = 0;
            m_real->GetRenderState(D3DRS_VERTEXBLEND, &blend);
            m_real->GetFVF(&fvf);
            ship = blend != D3DVBF_DISABLE && SkinnedLitFvf(fvf);
        }
        if (ship && m_refl.TargetIsBackBuffer() &&
            m_water.DrawSubmerged(vs, m_refl.WaterHeight(), [&] { draw(); }) && m_trace.Active())
            m_trace.Event("{\"ev\":\"submerged\"}");
    }
    if (m_shadows.enabled && (vs || fixedFunction) && m_shadows.AfterDraw(vs, fixedFunction, [&] { draw(); }) &&
        m_trace.Active())
        m_trace.Event(std::string("{\"ev\":\"shadow_caster\",\"path\":\"") + (vs ? "vs" : "ff") + "\"}");
    return hr;
}

bool ProxyDevice::DeferUntilAfterWater(bool indexed, D3DPRIMITIVETYPE type, INT base, UINT minVertex,
                                       UINT numVertices, UINT start, UINT count)
{
    // Only while the reflective, dynamic water will still be drawn this frame, and only BtS's selection effects
    // (circle, dot): fixed function, vertex-blended but unlit (no normals), blended, into the back buffer, at water
    // level.
    if (!m_water.enabled || !m_water.Ready() || m_frameWaterDraws > 0)
        return false;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    m_real->GetVertexShader(&vs);
    m_real->GetPixelShader(&ps);
    bool shaders = vs || ps;
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    DWORD blend = D3DVBF_DISABLE, fvf = 0, alpha = FALSE;
    m_real->GetRenderState(D3DRS_VERTEXBLEND, &blend);
    m_real->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha);
    m_real->GetFVF(&fvf);
    D3DMATRIX world;
    if (shaders || blend == D3DVBF_DISABLE || !fvf || (fvf & D3DFVF_NORMAL) || !alpha ||
        FAILED(m_real->GetTransform(D3DTS_WORLDMATRIX(0), &world)) || world._43 > m_refl.WaterHeight() + 8.0f ||
        !m_refl.TargetIsBackBuffer())
        return false;
    DeferredDraw d = {};
    ULONG before = DeviceRefs(m_real);
    if (FAILED(m_real->CreateStateBlock(D3DSBT_ALL, &d.state)))
        return false;
    m_deferRefs += DeviceRefs(m_real) - before;
    for (int i = 0; i < 4; ++i)
        m_real->GetTransform(D3DTS_WORLDMATRIX(i), &d.worlds[i]);
    d.indexed = indexed;
    d.type = type;
    d.base = base;
    d.minVertex = minVertex;
    d.numVertices = numVertices;
    d.start = start;
    d.count = count;
    m_deferred.push_back(d);
    ++m_frameDeferred;
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"deferred_after_water\"}");
    return true;
}

void ProxyDevice::ReplayDeferred()
{
    if (m_deferred.empty())
        return;
    if (!m_replayState)
    {
        ULONG before = DeviceRefs(m_real);
        if (FAILED(m_real->CreateStateBlock(D3DSBT_ALL, &m_replayState)))
        {
            DropDeferred();
            return;
        }
        m_deferRefs += DeviceRefs(m_real) - before;
    }
    m_replayState->Capture();
    D3DMATRIX worlds[4];
    for (int i = 0; i < 4; ++i)
        m_real->GetTransform(D3DTS_WORLDMATRIX(i), &worlds[i]);
    for (DeferredDraw& d : m_deferred)
    {
        d.state->Apply();
        // BtS draws these without a depth test before the ship, which then covers them. Drawn after the ship and the
        // water, they test against the depth there (without writing it): hidden where the hull is in front, as in
        // BtS, on top of the water.
        m_real->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        m_real->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        for (int i = 0; i < 4; ++i)
            m_real->SetTransform(D3DTS_WORLDMATRIX(i), &d.worlds[i]);
        if (d.indexed)
            m_real->DrawIndexedPrimitive(d.type, d.base, d.minVertex, d.numVertices, d.start, d.count);
        else
            m_real->DrawPrimitive(d.type, d.start, d.count);
    }
    m_replayState->Apply();
    for (int i = 0; i < 4; ++i)
        m_real->SetTransform(D3DTS_WORLDMATRIX(i), &worlds[i]);
    if (m_trace.Active())
        m_trace.Event("{\"ev\":\"replayed_after_water\",\"draws\":" + std::to_string(m_deferred.size()) + "}");
    ReleaseDeferredBlocks();
}

void ProxyDevice::ReleaseDeferredBlocks()
{
    if (m_deferred.empty())
        return;
    ULONG before = DeviceRefs(m_real);
    for (DeferredDraw& d : m_deferred)
        d.state->Release();
    m_deferred.clear();
    ULONG after = DeviceRefs(m_real);
    m_deferRefs -= (std::min)(m_deferRefs, before - after);
}

void ProxyDevice::DropDeferred()
{
    ReleaseDeferredBlocks();
    if (m_replayState)
    {
        ULONG before = DeviceRefs(m_real);
        m_replayState->Release();  // also a state block: must go before Reset
        m_replayState = nullptr;
        ULONG after = DeviceRefs(m_real);
        m_deferRefs -= (std::min)(m_deferRefs, before - after);
    }
}

const ShaderInfo* ProxyDevice::CurrentPixelShader()
{
    IDirect3DPixelShader9* shader = nullptr;
    m_real->GetPixelShader(&shader);
    if (!shader)
        return nullptr;
    auto it = m_registry.shaders.find(shader);
    shader->Release();
    return it != m_registry.shaders.end() ? &it->second : nullptr;
}

HRESULT ProxyDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    CountDraw(PrimitiveCount);
    if (m_trace.Active())
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "\"prim\":\"%s\",\"start\":%u,\"count\":%u", PrimName(PrimitiveType), StartVertex,
                 PrimitiveCount);
        m_trace.Draw(m_real, "DP", buf);
    }
    if (m_effects && DeferUntilAfterWater(false, PrimitiveType, 0, 0, 0, StartVertex, PrimitiveCount))
        return D3D_OK;
    return Dispatch([&] { return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); });
}

HRESULT ProxyDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
                                          UINT NumVertices, UINT startIndex, UINT primCount)
{
    CountDraw(primCount);
    if (m_trace.Active())
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "\"prim\":\"%s\",\"base\":%d,\"minv\":%u,\"numv\":%u,\"start\":%u,\"count\":%u",
                 PrimName(PrimitiveType), BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
        m_trace.Draw(m_real, "DIP", buf);
    }
    if (m_effects &&
        DeferUntilAfterWater(true, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount))
        return D3D_OK;
    return Dispatch([&] {
        return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex,
                                            primCount);
    });
}

HRESULT ProxyDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                     CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    CountDraw(PrimitiveCount);
    if (m_trace.Active())
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "\"prim\":\"%s\",\"count\":%u,\"stride\":%u", PrimName(PrimitiveType),
                 PrimitiveCount, VertexStreamZeroStride);
        m_trace.Draw(m_real, "DPUP", buf);
    }
    return Dispatch(
        [&] { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); });
}

HRESULT ProxyDevice::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                            UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                            CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    CountDraw(PrimitiveCount);
    if (m_trace.Active())
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "\"prim\":\"%s\",\"minv\":%u,\"numv\":%u,\"count\":%u,\"ibfmt\":%u,\"stride\":%u",
                 PrimName(PrimitiveType), MinVertexIndex, NumVertices, PrimitiveCount, IndexDataFormat,
                 VertexStreamZeroStride);
        m_trace.Draw(m_real, "DIPUP", buf);
    }
    return Dispatch([&] {
        return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                                              IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
    });
}
