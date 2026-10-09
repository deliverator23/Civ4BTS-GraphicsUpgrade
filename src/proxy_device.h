// IDirect3DDevice9 wrapper: the effects (water, reflection, shadows, lighting) and the debug tools (log, shader dumps,
// frame traces, ghost).
#pragma once
#include "common.h"
#include "d3d9_forward.h"
#include "water.h"
#include "reflection.h"
#include "lighting.h"
#include "shadows.h"
#include "terrain_normals.h"
#include "trace.h"

#include <unordered_set>
#include <vector>

class ProxyD3D9;

class ProxyDevice : public Device9Forward
{
public:
    ProxyDevice(IDirect3DDevice9* real, ProxyD3D9* parent);

    // The wrapper shares the real object's reference count and deletes itself when that reaches zero.
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
    STDMETHOD_(ULONG, AddRef)() override { return m_real->AddRef(); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9) override;

    // Frame boundaries and device lifetime
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override;
    STDMETHOD(Present)(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride,
                       CONST RGNDATA* pDirtyRegion) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;

    // Resource creation (logged always)
    STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                             IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                  IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                 IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                  DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface,
                                  HANDLE* pSharedHandle) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                         DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface,
                                         HANDLE* pSharedHandle) override;
    STDMETHOD(CreateVertexShader)(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override;
    STDMETHOD(CreatePixelShader)(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override;
    STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9* pVertexElements,
                                       IDirect3DVertexDeclaration9** ppDecl) override;

    // Render-target traffic (trace events)
    STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) override;
    STDMETHOD(Clear)(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z,
                     DWORD Stencil) override;
    STDMETHOD(StretchRect)(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                           IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect,
                           D3DTEXTUREFILTERTYPE Filter) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                             IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) override;
    STDMETHOD(SetClipPlane)(DWORD Index, CONST float* pPlane) override;
    STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override;
    STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) override;
    // Fixed-function lights, logged (distinct values once)
    STDMETHOD(SetLight)(DWORD Index, CONST D3DLIGHT9* pLight) override;
    // Which light slots the game uses (BtS's are 9 and up), for the lighting's fixed-function override
    STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) override;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override;

    // Draws
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
                                    UINT NumVertices, UINT startIndex, UINT primCount) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                               CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                      UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                      CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;

private:
    void NewLightFrame();
    void RegisterShader(const char* kind, const void* shader, const DWORD* function);
    void CountDraw(UINT primitives);
    // Debug ghost: if the draw just made was a unit/ship draw, issue it again shifted by the ghost offset.
    template <class Redraw> void Ghost(Redraw redraw);
    // Every draw goes through here: water detection, the game's draw, then the ghost and reflection redraws.
    template <class Draw> HRESULT Dispatch(Draw draw);
    const ShaderInfo* CurrentPixelShader();
    // Selection circles under ships: BtS draws them at water level before the water, so the reflective, dynamic water
    // would show them refracted beneath its surface. Such draws are held back and replayed straight after the water.
    bool DeferUntilAfterWater(bool indexed, D3DPRIMITIVETYPE type, INT base, UINT minVertex, UINT numVertices,
                              UINT start, UINT count);
    void ReplayDeferred();
    void ReleaseDeferredBlocks();  // the held-back draws' state blocks
    void DropDeferred();           // those and the replay state block (before Reset, at the end)

    ProxyD3D9* m_parent;
    Registry m_registry;
    Trace m_trace;
    Reflection m_refl;
    Water m_water;
    Shadows m_shadows;
    Lighting m_light;
    TerrainNormals m_terrain;  // heights and normals read from the terrain's vertex buffers (per-pixel lighting)
    float m_terrainLight[32] = {};  // Lighting::TerrainConstants for the shadows' terrain draws
    bool m_lightKeyWasDown = false;
    bool m_lightCycleKeyWasDown = false;
    std::unordered_set<uint64_t> m_loggedLights;  // fixed-function lights and ambients seen (logged once each)
    std::wstring m_lightmapSavedFor;  // trace folder the terrain lightmap was last saved into
    bool m_reflKeyWasDown = false, m_overlayKeyWasDown = false, m_waterKeyWasDown = false;
    bool m_shadowsKeyWasDown = false, m_shadowOverlayKeyWasDown = false;
    uint32_t m_frameBlobsSkipped = 0, m_framePaintedSkipped = 0;
    struct DeferredDraw
    {
        IDirect3DStateBlock9* state;  // everything the draw used (D3DSBT_ALL), applied again to replay it
        D3DMATRIX worlds[4];          // vertex-blend world matrices
        bool indexed;
        D3DPRIMITIVETYPE type;
        INT base;
        UINT minVertex, numVertices, start, count;
    };
    std::vector<DeferredDraw> m_deferred;
    IDirect3DStateBlock9* m_replayState = nullptr;  // the game's state around a replay
    ULONG m_deferRefs = 0;  // device references held by those state blocks (see Release)
    uint32_t m_frameDeferred = 0;
    uint32_t m_frameWaterDraws = 0;
    std::unordered_set<uint64_t> m_dumpedShaders;
    std::unordered_set<uint64_t> m_loggedDecls;
    UINT m_vsRegisters = 256;

    uint32_t m_frame = 0;
    bool m_keyWasDown = false;
    bool m_effects = false;  // effects allowed in this game (EffectsAllowed)
    bool m_ghost = false, m_ghostKeyWasDown = false;
    uint32_t m_frameGhosts = 0;
    std::wstring m_requestFile;  // <output>/<game>/trace.request starts a trace when it appears
    // Per-frame statistics, logged periodically
    uint32_t m_frameDraws = 0, m_frameRtSwitches = 0, m_framePrims = 0;
};
