// Test harness for GraphicsUpgrade's d3d9 proxy, run without a game.
//
// Loads the proxy d3d9.dll given on the command line and drives it the way the games do: Direct3DCreate9,
// CreateDevice (asking for a pure device, which the proxy should clear), shaders, a vertex declaration, a managed
// texture, a cube texture, a render-target texture, then a few frames with render-target switches, Clear,
// StretchRect, a clip plane, shader draws and a fixed-function draw. The proxy's GraphicsUpgrade.ini (next to the DLL)
// should set AutoFrame so one of these frames is traced; run_tests.py checks the output.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>  // define IID_IDirect3DDevice9 here; the harness links no d3d9 libs
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "border_ps.h"
#include "border_vs.h"
#include "test_btswater_ps.h"
#include "test_btswater_vs.h"
#include "test_ghost_old_vs.h"
#include "test_ghost_vs.h"
#include "test_lit_vs.h"
#include "test_ps.h"
#include "test_terrain_old_ps.h"
#include "test_terrain_ps.h"
#include "test_terrain_vs.h"
#include "test_vs.h"
#include "test_water_vs.h"
#include "water_ps.h"
#include "wave_drape_ps.h"
#include "wave_drape_vs.h"
#include "wave_ps.h"
#include "wave_vs.h"
#include "water_shore_ps.h"
#include "water_shore_vs.h"
#include "water_vs.h"

#include "../src/jigsaw.h"
#include "../src/plot_stamps.h"
#include "../src/hill_field.h"
#include "../src/terrain_cells.h"
#include "../src/terrain_patch.h"
#include "../src/terrain_sets.h"

#define CHECK(expr)                                                                                                \
    do                                                                                                             \
    {                                                                                                              \
        HRESULT hr_ = (expr);                                                                                      \
        if (FAILED(hr_))                                                                                           \
        {                                                                                                          \
            printf("FAILED 0x%08lx: %s\n", static_cast<unsigned long>(hr_), #expr);                               \
            return 1;                                                                                              \
        }                                                                                                          \
    } while (0)

static std::vector<DWORD> Tokens(const BYTE* bytes, size_t size)
{
    std::vector<DWORD> v(size / 4);
    memcpy(v.data(), bytes, size);
    return v;
}

struct Vertex
{
    float x, y, z, u, v;
};

// Ghost check. With the proxy's ghost on (offset 0.9,0,0 in the test INI), each unit-style draw appears a second
// time 0.9 to the right in clip space (identity matrices). Three flat red triangles in a 64x64 target:
//   A: unit-style vertex shader (mtxWorldBones c0, mtxViewProj c60), centred (-0.5,-0.33): ghost expected
//   B: fixed-function with vertex blending (like Gamebryo's FF skinning), centred (-0.5,0.37): ghost expected
//   C: fixed-function without blending, centred (-0.8,0.8): no ghost
// Prints one line "GHOST a=. ag=. b=. bg=. c=. cg=. restored=." (1 = red pixel found / state restored).
static int GhostScene(IDirect3DDevice9* dev)
{
    IDirect3DSurface9 *rt = nullptr, *sys = nullptr, *old = nullptr;
    CHECK(dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr));
    CHECK(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    std::vector<DWORD> code = Tokens(g_ghostVS, sizeof(g_ghostVS));
    IDirect3DVertexShader9* vs = nullptr;
    CHECK(dev->CreateVertexShader(code.data(), &vs));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));

    IDirect3DSurface9* oldDepth = nullptr;
    CHECK(dev->GetRenderTarget(0, &old));
    CHECK(dev->GetDepthStencilSurface(&oldDepth));
    CHECK(dev->SetRenderTarget(0, rt));
    CHECK(dev->SetDepthStencilSurface(nullptr));
    CHECK(dev->BeginScene());
    CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0));
    // Flat red from the texture factor, so no textures or pixel shader are involved.
    CHECK(dev->SetPixelShader(nullptr));
    CHECK(dev->SetTexture(0, nullptr));
    CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
    CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));

    // A: unit-style shader
    const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK(dev->SetVertexShaderConstantF(0, bones, 3));
    CHECK(dev->SetVertexShaderConstantF(60, identity, 4));
    CHECK(dev->SetVertexDeclaration(decl));
    CHECK(dev->SetVertexShader(vs));
    const float triA[9] = {-0.6f, -0.43f, 0.5f, -0.5f, -0.23f, 0.5f, -0.4f, -0.43f, 0.5f};
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triA, 12));
    float vpAfter[16] = {};
    CHECK(dev->GetVertexShaderConstantF(60, vpAfter, 4));

    // B: fixed-function with one blend weight (WORLDMATRIX 0 and 1 identity)
    CHECK(dev->SetVertexShader(nullptr));
    D3DMATRIX m;
    memcpy(&m, identity, sizeof(m));
    CHECK(dev->SetTransform(D3DTS_WORLDMATRIX(0), &m));
    CHECK(dev->SetTransform(D3DTS_WORLDMATRIX(1), &m));
    CHECK(dev->SetTransform(D3DTS_VIEW, &m));
    CHECK(dev->SetTransform(D3DTS_PROJECTION, &m));
    CHECK(dev->SetFVF(D3DFVF_XYZB1 | D3DFVF_NORMAL));  // like BtS ship parts: blend weights and normals
    CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS));
    const float triB[21] = {-0.6f, 0.27f, 0.5f, 1, 0, 0, 1, -0.5f, 0.47f, 0.5f, 1, 0, 0, 1, -0.4f, 0.27f, 0.5f, 1, 0, 0, 1};
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triB, 28));
    D3DMATRIX viewAfter;
    CHECK(dev->GetTransform(D3DTS_VIEW, &viewAfter));

    // C: fixed-function, no blending
    CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
    CHECK(dev->SetFVF(D3DFVF_XYZ));
    const float triC[9] = {-0.9f, 0.73f, 0.5f, -0.8f, 0.93f, 0.5f, -0.7f, 0.73f, 0.5f};
    CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triC, 12));
    CHECK(dev->EndScene());

    CHECK(dev->GetRenderTargetData(rt, sys));
    D3DLOCKED_RECT lr;
    CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    auto red = [&](float x, float y) {
        int px = static_cast<int>((x + 1) * 32), py = static_cast<int>((1 - y) * 32);
        DWORD c = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + py * lr.Pitch)[px];
        return ((c >> 16) & 0xFF) > 0x80 ? 1 : 0;
    };
    int a = red(-0.5f, -0.36f), ag = red(0.4f, -0.36f), b = red(-0.5f, 0.34f), bg = red(0.4f, 0.34f),
        c = red(-0.8f, 0.8f), cg = red(0.1f, 0.8f);
    sys->UnlockRect();
    int restored = memcmp(vpAfter, identity, sizeof(identity)) == 0 && memcmp(&viewAfter, identity, sizeof(identity)) == 0;
    printf("GHOST a=%d ag=%d b=%d bg=%d c=%d cg=%d restored=%d\n", a, ag, b, bg, c, cg, restored);

    CHECK(dev->SetRenderTarget(0, old));
    CHECK(dev->SetDepthStencilSurface(oldDepth));
    CHECK(dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
    CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW));
    CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    oldDepth->Release();
    old->Release();
    decl->Release();
    vs->Release();
    sys->Release();
    rt->Release();
    return 0;
}

// Reflection check, run with the proxy's reflection on (water height 0.5 from the water draw) and a trace of
// frame 2 (which saves reflection.bmp). Each frame draws, into the back buffer with identity matrices, flat red:
//   A unit-style shader, z 0.6 (above water), at (-0.5,-0.5)  -> reflected at (+0.5,-0.5) (x flipped)
//   B unit-style shader, z 0.4 (below water), at (-0.5, 0.5)  -> clipped: nothing at (+0.5, 0.5)
//   C FF vertex-blended,  z 0.6,               at (-0.5, 0.0)  -> reflected at (+0.5, 0.0)
//   D FF vertex-blended,  z 0.4,               at (-0.8, 0.0)  -> clipped: nothing at (+0.8, 0.0)
//   F FF blended, no normals (selection circle), z 0.6, at (-0.2, 0.5) -> not reflected: nothing at (+0.2, 0.5)
//   W the water draw (water-style shader), which ends the ship phase
//   E unit-style shader, z 0.6, after the water, at (-0.5,-0.8) -> not reflected: nothing at (+0.5,-0.8)
// After frame 1 the device is Reset (the proxy must release its targets first). Prints "REFLECT restored=. reset=.".
static int ReflectScene(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS& pp, int frames)
{
    std::vector<DWORD> unitCode = Tokens(g_ghostVS, sizeof(g_ghostVS)), waterCode = Tokens(g_testWaterVS, sizeof(g_testWaterVS));
    IDirect3DVertexShader9 *unit = nullptr, *water = nullptr;
    CHECK(dev->CreateVertexShader(unitCode.data(), &unit));
    CHECK(dev->CreateVertexShader(waterCode.data(), &water));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float waterWorld[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0.5f};  // c4..c6: translation z = 0.5
    auto tri = [](float cx, float cy, float z, float* out, int stride) {
        const float pts[3][2] = {{cx - 0.08f, cy - 0.06f}, {cx, cy + 0.1f}, {cx + 0.08f, cy - 0.06f}};
        for (int i = 0; i < 3; ++i)
        {
            out[i * stride + 0] = pts[i][0];
            out[i * stride + 1] = pts[i][1];
            out[i * stride + 2] = z;
            if (stride >= 4)
                out[i * stride + 3] = 1;  // blend weight
            if (stride == 7)
            {
                out[i * stride + 4] = 0;  // normal
                out[i * stride + 5] = 0;
                out[i * stride + 6] = 1;
            }
        }
    };
    int restored = 1, reset = 1;
    for (int f = 0; f < frames; ++f)
    {
        if (f == 1)
        {
            // Our test resources are D3DPOOL_MANAGED or shaders, which survive Reset; the proxy's targets must not block it.
            HRESULT hr = dev->Reset(&pp);
            reset = SUCCEEDED(hr) ? 1 : 0;
        }
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetPixelShader(nullptr));
        CHECK(dev->SetTexture(0, nullptr));
        CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
        CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));

        float v3[9], v4[12], v7[21];
        // A and B: unit-style shader
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(unit));
        CHECK(dev->SetVertexShaderConstantF(0, bones, 3));
        CHECK(dev->SetVertexShaderConstantF(60, identity, 4));
        tri(-0.5f, -0.5f, 0.6f, v3, 3);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v3, 12));
        float vp[16];
        DWORD clip = 1;
        IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
        dev->GetVertexShaderConstantF(60, vp, 4);
        dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip);
        dev->GetRenderTarget(0, &rt);
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        if (memcmp(vp, identity, sizeof(vp)) != 0 || clip != 0 || rt != bb)
            restored = 0;
        rt->Release();
        bb->Release();
        tri(-0.5f, 0.5f, 0.4f, v3, 3);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v3, 12));

        // C and D: fixed function with vertex blending
        CHECK(dev->SetVertexShader(nullptr));
        D3DMATRIX m;
        memcpy(&m, identity, sizeof(m));
        CHECK(dev->SetTransform(D3DTS_WORLDMATRIX(0), &m));
        CHECK(dev->SetTransform(D3DTS_WORLDMATRIX(1), &m));
        CHECK(dev->SetTransform(D3DTS_VIEW, &m));
        CHECK(dev->SetTransform(D3DTS_PROJECTION, &m));
        CHECK(dev->SetFVF(D3DFVF_XYZB1 | D3DFVF_NORMAL));  // like BtS ship parts: blend weights and normals
        CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS));
        tri(-0.5f, 0.0f, 0.6f, v7, 7);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v7, 28));
        D3DMATRIX after;
        dev->GetTransform(D3DTS_VIEW, &after);
        if (memcmp(&after, identity, sizeof(after)) != 0)
            restored = 0;
        tri(-0.8f, 0.0f, 0.4f, v7, 7);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v7, 28));
        // F: blend weights but no normals, like the unit selection circle: not a unit, not reflected
        CHECK(dev->SetFVF(D3DFVF_XYZB1));
        tri(-0.2f, 0.5f, 0.6f, v4, 4);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v4, 16));
        CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));

        // W: water
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(water));
        CHECK(dev->SetVertexShaderConstantF(0, identity, 4));
        CHECK(dev->SetVertexShaderConstantF(4, waterWorld, 3));
        tri(0.0f, 0.9f, 0.5f, v3, 3);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v3, 12));

        // E: unit after the water
        CHECK(dev->SetVertexShader(unit));
        CHECK(dev->SetVertexShaderConstantF(60, identity, 4));
        tri(-0.5f, -0.8f, 0.6f, v3, 3);
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v3, 12));
        CHECK(dev->EndScene());
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("REFLECT restored=%d reset=%d\n", restored, reset);
    decl->Release();
    water->Release();
    unit->Release();
    return 0;
}

// Water check, run with [water] Enabled=1 pointing at the real Colonization install. A full-screen
// quad is drawn into the back buffer with BtS-style water shaders whose pixel shader outputs magenta. If the proxy
// swaps in Colonization's water shader, the centre pixel isn't magenta. Afterwards the BtS-style shaders, constants
// and textures must be back. Prints "WATER pixel=AARRGGBB restored=.".
static int WaterScene(IDirect3DDevice9* dev, int frames)
{
    std::vector<DWORD> vsCode = Tokens(g_btsWaterVS, sizeof(g_btsWaterVS)),
                       psCode = Tokens(g_btsWaterPS, sizeof(g_btsWaterPS));
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    CHECK(dev->CreateVertexShader(vsCode.data(), &vs));
    CHECK(dev->CreatePixelShader(psCode.data(), &ps));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                                       {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));
    std::vector<DWORD> unitCode = Tokens(g_ghostVS, sizeof(g_ghostVS));
    IDirect3DVertexShader9* unitVs = nullptr;
    CHECK(dev->CreateVertexShader(unitCode.data(), &unitVs));
    const D3DVERTEXELEMENT9 posElems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          D3DDECL_END()};
    IDirect3DVertexDeclaration9* posDecl = nullptr;
    CHECK(dev->CreateVertexDeclaration(posElems, &posDecl));
    IDirect3DTexture9 *coast = nullptr, *fog = nullptr;
    for (IDirect3DTexture9** t : {&coast, &fog})
    {
        CHECK(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t, nullptr));
        D3DLOCKED_RECT lr;
        CHECK((*t)->LockRect(0, &lr, nullptr, 0));
        *static_cast<DWORD*>(lr.pBits) = 0xFFFFFFFF;
        (*t)->UnlockRect(0);
    }
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    CHECK(dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0.5f};  // water plane at z = 0.5
    const float zero[8] = {};
    // x, y, z, coast uv (TEXCOORD1), fog uv (TEXCOORD2)
    const float quad[6][7] = {{-1, -1, 0.5f, 0, 1, 0, 1}, {-1, 1, 0.5f, 0, 0, 0, 0}, {1, 1, 0.5f, 1, 0, 1, 0},
                              {-1, -1, 0.5f, 0, 1, 0, 1}, {1, 1, 0.5f, 1, 0, 1, 0},  {1, -1, 0.5f, 1, 1, 1, 1}};
    DWORD pixel = 0, circlePixel = 0, hiddenPixel = 0;
    unsigned long long frameSum = 0;
    UINT brightX = 0, brightY = 0;
    int restored = 1;
    for (int f = 0; f < frames; ++f)
    {
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        // A selection circle like BtS's under a ship: fixed function, vertex-blended, no normals, alpha-blended, at
        // water level, drawn before the water. Red, at (-0.5,-0.5); with Colonization's water it must end up on top.
        {
            D3DMATRIX id;
            memcpy(&id, identity, sizeof(id));
            const float circle[3][4] = {{-0.6f, -0.6f, 0.5f, 1}, {-0.5f, -0.4f, 0.5f, 1}, {-0.4f, -0.6f, 0.5f, 1}};
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZB1));
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            for (int m = 0; m < 2; ++m)
                CHECK(dev->SetTransform(D3DTS_WORLDMATRIX(m), &id));
            CHECK(dev->SetTransform(D3DTS_VIEW, &id));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &id));
            CHECK(dev->SetTexture(0, nullptr));
            CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
            CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
            // from a vertex buffer, as BtS does (user-pointer draws can't be replayed later)
            IDirect3DVertexBuffer9* vb = nullptr;
            CHECK(dev->CreateVertexBuffer(sizeof(circle), D3DUSAGE_WRITEONLY, D3DFVF_XYZB1, D3DPOOL_MANAGED, &vb, nullptr));
            void* data = nullptr;
            CHECK(vb->Lock(0, sizeof(circle), &data, 0));
            memcpy(data, circle, sizeof(circle));
            vb->Unlock();
            CHECK(dev->SetStreamSource(0, vb, 0, 16));
            CHECK(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
            CHECK(dev->SetStreamSource(0, nullptr, 0, 0));
            vb->Release();  // the held-back draw's state block keeps it alive until it's replayed
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        }
        // A ship's hull in front of the circle's lower half (green, depth-writing at 0.3, nearer than the circle's
        // 0.5), drawn after it as BtS draws ships after their selection circles: the circle mustn't come out on top.
        {
            const float hull[6][3] = {{-0.56f, -0.65f, 0.3f}, {-0.56f, -0.5f, 0.3f}, {-0.44f, -0.5f, 0.3f},
                                      {-0.56f, -0.65f, 0.3f}, {-0.44f, -0.5f, 0.3f}, {-0.44f, -0.65f, 0.3f}};
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            D3DMATRIX id;
            memcpy(&id, identity, sizeof(id));
            CHECK(dev->SetTransform(D3DTS_WORLD, &id));
            CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF00FF00));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
            CHECK(dev->SetRenderState(D3DRS_ZENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, hull, 12));
            CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        }
        // Two ships drawn like BtS's (unit-style shader, mtxViewProj c60), before the water at z = 0.5: a "submarine"
        // 0.04 below the surface at (-0.5,0.5), and one above it at (0.5,0.5). With [water] Underwater* set, the proxy
        // draws the submarine's underwater part into the refraction (traces save it as refraction.bmp).
        {
            const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            const float sub[3][3] = {{-0.6f, 0.4f, 0.46f}, {-0.5f, 0.6f, 0.46f}, {-0.4f, 0.4f, 0.46f}};
            const float ship[3][3] = {{0.4f, 0.4f, 0.6f}, {0.5f, 0.6f, 0.6f}, {0.6f, 0.4f, 0.6f}};
            CHECK(dev->SetVertexDeclaration(posDecl));
            CHECK(dev->SetVertexShader(unitVs));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetVertexShaderConstantF(0, bones, 3));
            CHECK(dev->SetVertexShaderConstantF(60, identity, 4));
            CHECK(dev->SetTexture(0, nullptr));
            CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, sub, 12));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ship, 12));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        }
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(vs));
        CHECK(dev->SetPixelShader(ps));
        CHECK(dev->SetVertexShaderConstantF(0, identity, 4));
        CHECK(dev->SetVertexShaderConstantF(4, world, 3));
        CHECK(dev->SetVertexShaderConstantF(7, zero, 2));
        CHECK(dev->SetTexture(2, coast));
        CHECK(dev->SetTexture(3, fog));
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, 28));
        IDirect3DVertexShader9* nowVs = nullptr;
        IDirect3DPixelShader9* nowPs = nullptr;
        IDirect3DBaseTexture9* nowCoast = nullptr;
        float c0[16];
        dev->GetVertexShader(&nowVs);
        dev->GetPixelShader(&nowPs);
        dev->GetTexture(2, &nowCoast);
        dev->GetVertexShaderConstantF(0, c0, 4);
        if (nowVs != vs || nowPs != ps || nowCoast != coast || memcmp(c0, identity, sizeof(c0)) != 0)
            restored = 0;
        for (IUnknown* o : {static_cast<IUnknown*>(nowVs), static_cast<IUnknown*>(nowPs), static_cast<IUnknown*>(nowCoast)})
            if (o)
                o->Release();
        // Something drawn after the water (BtS: the waves): the proxy puts the held-back circle in before it.
        {
            const float after[3][3] = {{0.4f, 0.4f, 0.5f}, {0.5f, 0.6f, 0.5f}, {0.6f, 0.4f, 0.5f}};
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, after, 12));
            DWORD vb = 1;
            dev->GetRenderState(D3DRS_VERTEXBLEND, &vb);
            if (vb != D3DVBF_DISABLE)
                restored = 0;  // the replay must leave the game's state as it was
        }
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        pixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + (bd.Height / 2) * lr.Pitch)[bd.Width / 2];
        // the circle at (-0.5,-0.45), clear of the hull; and at (-0.5,-0.55), behind it
        // The whole frame's colour channels added up, and where the brightest pixel is (the glint, if any)
        frameSum = 0;
        DWORD brightest = 0;
        for (UINT y = 0; y < bd.Height; ++y)
        {
            const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < bd.Width; ++x)
            {
                const DWORD c = row[x], sum = (c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF);
                frameSum += sum;
                if (sum > brightest)
                    brightest = sum, brightX = x, brightY = y;
            }
        }
        circlePixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) +
                                                     static_cast<UINT>(bd.Height * 0.725f) * lr.Pitch)[bd.Width / 4];
        hiddenPixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) +
                                                     static_cast<UINT>(bd.Height * 0.775f) * lr.Pitch)[bd.Width / 4];
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("WATER pixel=%08lx restored=%d circle=%08lx hidden=%08lx sum=%llu brightest=%u,%u\n",
           static_cast<unsigned long>(pixel), restored, static_cast<unsigned long>(circlePixel),
           static_cast<unsigned long>(hiddenPixel), frameSum, brightX, brightY);
    sys->Release();
    bb->Release();
    fog->Release();
    coast->Release();
    decl->Release();
    posDecl->Release();
    unitVs->Release();
    ps->Release();
    vs->Release();
    return 0;
}

// Sun shadows check, run with [shadows] Enabled=1, SunDirection=-1,0,-1, WaterHeight=0. A top-down orthographic
// camera (500 world units = half the 256x256 screen) looks at a BtS-style terrain quad at z = 0 (test_terrain.hlsl,
// 1x1 textures: base 0.4, detail 0.5, lightmap 0.8, fog of war 0.9, so ~161 everywhere). Then a unit-style triangle
// (mtxViewProj c60) floats at z = 100 over the origin, alpha 1 from the texture factor. With the sun going down at
// 45 degrees towards -x, its shadow lands 100 units to the -x side, on the next frame's terrain. The device is Reset
// before frame 3. Prints "SHADOW lit=. shadowed=. lit2=. shadowed2=. caster=. restored=. reset=." (blue channel of
// world (200,0) and (-100,0) at the end of frames 2 and 5, red channel at the caster; restored = the game's
// shaders, constants, texture stage 4 and render target are back after each draw).
// With blobPath (BtS's unit_shadows.dds), each frame also draws, fixed function like BtS:
//   a blob shadow quad at world (300,300)                              -> hidden while shadows are on
//   a rigid lit mesh (a "weapon"), 100 units up over the blob, hanging 40 below its WORLD (the grip)
//                                                                      -> casts all of it: shadow at (200,400)
//   the same mesh with WORLD at (-300,-300), far from any blob         -> doesn't cast: (-400,-200) stays lit
// and adds " blob=. part=. far=." (blue channel at (300,300), (200,400), (-400,-200)).
// overlay draws one ground decal over the whole screen right after the terrain, so the same two points measure it:
//   river0 / river1  BtS's own compiled River_Shader pass P0 (floodplain) / P1 (river), loaded from riverDir (the
//                    game's compiled effects, extracted by run_tests.py), with 1x1 textures (P0 ~86, P1 ~92 unshadowed)
//   decal            a fixed-function alpha-blended decal, depth-tested, no depth writes, sampling the terrain's
//                    fog-of-war texture on stage 1, like BtS's roads (blue 192 x 0.9 = 173 unshadowed)
//   symbol           the same with the depth test off, like BtS's plot symbols (the yield icons, Symbols.fx): never
//                    shadowed or re-lit
//   overlay          the same without the fog of war, like an interface overlay on the ground: never shadowed
//   water            BtS-style water (magenta) over the ground at z = 1: shadows on its surface ([shadows] WaterShadow)
//   treeshadow       the same patch with uv in the painted-shadow part of BtS's tree atlas: dropped with shadows on
//   tree             BtS's own compiled tree shaders (ContourShader, from riverDir) drawing a ground-covering patch
//                    facing up: unlit in BtS (blue 128 x 0.9 = 115), lit with [lighting] Tree* colours
//   slope            the terrain drawn from a vertex and index buffer (as BtS does) as a 5x5 grid, 250 apart, sloping
//                    z = 0.2 x: the proxy reads its heights for per-pixel lighting (traces save terrain_normals.bmp)
//   ridge            the same grid, flat but for a 300-high ridge along x = 0: with the sun going towards -x, the
//                    ridge shades the ground at x = -250 (hill shadows); adds " hill=." (blue at (-250,0))
//   finemap          instead of the quad, three small terrain chunks 1024 grid cells apart at spacing 11.25 (High
//                    Detail Terrain): the proxy's height grid must keep them apart (traces save terrain_heights.bin)
//   widemap          the same at spacing 22.5, with mtxLightmap over a map 256 plots across
//   ultramap         the same with 9x9 chunks of vertices 2.8125 apart: the grid keeps 1 vertex in 4
//   vanish           no extra draws; frame 4 (after the Reset) draws nothing at all (a full-screen menu) and frame 5
//                    has no caster: the ground mustn't show frame 3's shadow
//   effect           a fixed-function additive effect (SRCALPHA/ONE, blue 64) over everything (depth test off), unlit,
//                    through a perspective camera, like a sandstorm's glow: with [lighting] LitEffectTextures it takes
//                    the change of light, as the roads do
//   effectlit        the same, lit with an emissive-only material (the lights add nothing)
//   effectmul        the same multiplying (ZERO/INVSRCCOLOR: blue x 191/255), like a sandstorm's shade: left alone
//   effectui         the additive one through the orthographic camera, like the interface: left alone
//   building         a lit fixed-function mesh (normals, depth writes) 100 units up at world (200,300), like a
//                    building: with [shadows] BuildingShadows=1 its shadow lands at (100,300); adds " bld=."
//   buildingwater    the same, after a patch of BtS-style water (mtxWorld z 0) at (-450,-450): the water's height is
//                    read from it, so with [reflection] WaterHeight above the building's base it still casts
static bool ReadAll(const std::wstring& path, std::vector<DWORD>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    std::vector<BYTE> bytes(1 << 16);
    bytes.resize(fread(bytes.data(), 1, bytes.size(), f));
    fclose(f);
    out.assign(bytes.size() / 4, 0);
    memcpy(out.data(), bytes.data(), out.size() * 4);
    return !out.empty();
}

// The top level of a DXT3 or DXT5 .dds as a managed texture (the proxy hashes that level).
static IDirect3DTexture9* LoadDdsTop(IDirect3DDevice9* dev, const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
    {
        printf("FAILED: can't open %ls\n", path);
        return nullptr;
    }
    std::vector<BYTE> dds(1 << 20);
    dds.resize(fread(dds.data(), 1, dds.size(), f));
    fclose(f);
    UINT h = *reinterpret_cast<const UINT*>(&dds[12]), w = *reinterpret_cast<const UINT*>(&dds[16]);
    D3DFORMAT fmt = static_cast<D3DFORMAT>(*reinterpret_cast<const UINT*>(&dds[84]));  // fourCC: DXT3 or DXT5
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr)))
    {
        printf("FAILED: can't create the texture for %ls\n", path);
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)))
    {
        const UINT rowBytes = (w / 4) * 16;  // one row of 4x4 blocks
        for (UINT y = 0; y < h / 4; ++y)
            memcpy(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, &dds[128 + y * rowBytes], rowBytes);
        tex->UnlockRect(0);
    }
    return tex;
}

// With the environment variable GU_TEST_OLD_SHADERS=1, the terrain and unit shaders are named as in older
// (non-Steam) BtS builds: TerrainBase0; mtxBones and SkinWorldViewProj, which the unit is drawn with as those builds
// do: its bones relative to its own placement (here 37,-23,11 away, so a proxy taking SkinWorldViewProj for the
// camera alone would put its shadow in the wrong place), and SkinWorldViewProj = the camera times that placement.
static bool OldShaders()
{
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(L"GU_TEST_OLD_SHADERS", v, 8) > 0 && v[0] == L'1';
}

static int ShadowScene(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS& pp, int frames, const wchar_t* blobPath,
                       const wchar_t* overlay, const wchar_t* riverDir, const wchar_t* flagPolePath, DWORD flagStage)
{
    const bool old = OldShaders();
    std::vector<DWORD> tvsCode = Tokens(g_terrainVS, sizeof(g_terrainVS)),
                       tpsCode = old ? Tokens(g_terrainOldPS, sizeof(g_terrainOldPS)) : Tokens(g_terrainPS, sizeof(g_terrainPS)),
                       unitCode = old ? Tokens(g_ghostOldVS, sizeof(g_ghostOldVS)) : Tokens(g_ghostVS, sizeof(g_ghostVS));
    IDirect3DVertexShader9 *tvs = nullptr, *unit = nullptr;
    IDirect3DPixelShader9* tps = nullptr;
    CHECK(dev->CreateVertexShader(tvsCode.data(), &tvs));
    CHECK(dev->CreatePixelShader(tpsCode.data(), &tps));
    CHECK(dev->CreateVertexShader(unitCode.data(), &unit));
    const D3DVERTEXELEMENT9 terrainElems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()};
    const D3DVERTEXELEMENT9 unitElems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                           D3DDECL_END()};
    IDirect3DVertexDeclaration9 *terrainDecl = nullptr, *unitDecl = nullptr;
    CHECK(dev->CreateVertexDeclaration(terrainElems, &terrainDecl));
    CHECK(dev->CreateVertexDeclaration(unitElems, &unitDecl));
    // base, fog of war, lightmap, detail (stages 0-3)
    const DWORD colours[4] = {0xFF666666, 0xFFE6E6E6, 0xFFCCCCCC, 0xFF808080};
    IDirect3DTexture9* tex[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        CHECK(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[i], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(tex[i]->LockRect(0, &lr, nullptr, 0));
        *static_cast<DWORD*>(lr.pBits) = colours[i];
        tex[i]->UnlockRect(0);
    }
    // GU_TEST_DETAIL_RAMP=1: the detail texture is 256x1, grey x at texel x (brighter along u), for detail normals
    // made from its brightness.
    wchar_t ramp[8] = {};
    if (GetEnvironmentVariableW(L"GU_TEST_DETAIL_RAMP", ramp, 8) > 0 && ramp[0] == L'1')
    {
        tex[3]->Release();
        CHECK(dev->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[3], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(tex[3]->LockRect(0, &lr, nullptr, 0));
        for (DWORD x = 0; x < 256; ++x)
            static_cast<DWORD*>(lr.pBits)[x] = 0xFF000000 | x << 16 | x << 8 | x;
        tex[3]->UnlockRect(0);
    }
    // GU_TEST_SHEET=1: the base texture is a 16x16 blend sheet (4x4 tiles of 4 texels), its top two rows of tiles grey
    // 0x20 and the bottom two 0x60; the slope's texture coordinates cross the boundary between them at world y = 0.
    // L or H: all of it 0x20 or 0x60.
    wchar_t sheetEnv[8] = {};
    if (GetEnvironmentVariableW(L"GU_TEST_SHEET", sheetEnv, 8) > 0 && sheetEnv[0] != L'0')
    {
        tex[0]->Release();
        CHECK(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[0], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(tex[0]->LockRect(0, &lr, nullptr, 0));
        for (DWORD y = 0; y < 16; ++y)
            for (DWORD x = 0; x < 16; ++x)
                reinterpret_cast<DWORD*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] =
                    sheetEnv[0] == L'L' || (sheetEnv[0] == L'1' && y < 8) ? 0xFF202020 : 0xFF606060;
        tex[0]->UnlockRect(0);
    }
    IDirect3DSurface9* sys = nullptr;
    CHECK(dev->CreateOffscreenPlainSurface(pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
                                          D3DPOOL_SYSTEMMEM, &sys, nullptr));
    // BtS's blob texture: a DXT3 .dds; only the top level is loaded (the proxy hashes that one).
    IDirect3DTexture9* blob = nullptr;
    if (blobPath)
    {
        blob = LoadDdsTop(dev, blobPath);
        if (!blob)
            return 1;
    }
    // With flagPolePath (BtS's Ancient_Flag_Pole.dds), the building mesh is drawn with it on stage flagStage: 0 like a
    // unit flag's pole, 3 like its cloth (the player's colours on the stages before). Either way it shouldn't cast.
    IDirect3DTexture9* flagPole = nullptr;
    if (flagPolePath)
    {
        flagPole = LoadDdsTop(dev, flagPolePath);
        if (!flagPole)
            return 1;
    }

    // Ground overlay (see above)
    const bool river0 = wcscmp(overlay, L"river0") == 0, river1 = wcscmp(overlay, L"river1") == 0,
               symbol = wcscmp(overlay, L"symbol") == 0, fowDecal = wcscmp(overlay, L"decal") == 0 || symbol,
               uiDecal = wcscmp(overlay, L"overlay") == 0, decal = fowDecal || uiDecal, treeShadow = wcscmp(overlay, L"treeshadow") == 0,
               tree = wcscmp(overlay, L"tree") == 0 || treeShadow,
               buildingWater = wcscmp(overlay, L"buildingwater") == 0,
               building = wcscmp(overlay, L"building") == 0 || buildingWater, ridge = wcscmp(overlay, L"ridge") == 0,
               vanish = wcscmp(overlay, L"vanish") == 0, waterOverlay = wcscmp(overlay, L"water") == 0,
               slope = wcscmp(overlay, L"slope") == 0 || ridge,
               wideMap = wcscmp(overlay, L"widemap") == 0, ultraMap = wcscmp(overlay, L"ultramap") == 0,
               sharedMap = wcscmp(overlay, L"sharedmap") == 0,
               fineMap = wcscmp(overlay, L"finemap") == 0 || wideMap || ultraMap || sharedMap,
               effectLit = wcscmp(overlay, L"effectlit") == 0, effectMul = wcscmp(overlay, L"effectmul") == 0,
               effectUi = wcscmp(overlay, L"effectui") == 0,
               effect = wcscmp(overlay, L"effect") == 0 || effectLit || effectMul || effectUi;
    IDirect3DVertexBuffer9* slopeVb = nullptr;
    IDirect3DIndexBuffer9* slopeIb = nullptr;
    if (slope)
    {
        float verts[25][7];
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x)
            {
                float wx = -500.0f + 250 * x, wy = -500.0f + 250 * y;
                const float z = ridge ? (x == 2 ? 300.0f : 0.0f) : 0.2f * wx;
                const float v[7] = {wx, wy, z, x / 4.0f, 1 - y / 4.0f, x / 4.0f, 1 - y / 4.0f};
                memcpy(verts[y * 5 + x], v, sizeof(v));
            }
        WORD idx[16 * 6];
        int k = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
                WORD a = static_cast<WORD>(y * 5 + x), b = a + 1, c = a + 5, d = a + 6;
                const WORD q[6] = {a, c, d, a, d, b};
                memcpy(&idx[k], q, sizeof(q));
                k += 6;
            }
        void* p = nullptr;
        CHECK(dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &slopeVb, nullptr));
        CHECK(slopeVb->Lock(0, 0, &p, 0));
        memcpy(p, verts, sizeof(verts));
        slopeVb->Unlock();
        CHECK(dev->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &slopeIb, nullptr));
        CHECK(slopeIb->Lock(0, 0, &p, 0));
        memcpy(p, idx, sizeof(idx));
        slopeIb->Unlock();
    }
    // finemap / widemap: three 5x5 terrain chunks (a vertex buffer each, placed by mtxWorld) exactly 1024 grid cells
    // apart, at (0,0), (1024 cells, 0) and (0, 1024 cells), flat at heights 10, 20 and 30. finemap: spacing 11.25,
    // as with "High Detail Terrain", no mtxLightmap; widemap: spacing 22.5, mtxLightmap over a map 256 plots (46,080
    // units) across. Either way the proxy's grid must keep the three apart (traces save terrain_heights.bin).
    // ultramap: 9x9 chunks of vertices 2.8125 apart (the 65-sample terrain patch with High Detail Terrain): the grid
    // keeps 1 vertex in 4 (11.25 apart) so a Huge map still fits, and the chunks are 1024 of those cells apart.
    // sharedmap: finemap's, all three drawn from the first chunk's buffer (height 10), as BtS draws some buffers at
    // more than one cell.
    const float fineSpacing = wideMap ? 22.5f : ultraMap ? 2.8125f : 11.25f, fineHeights[3] = {10, 20, 30};
    const float gridSpacing = ultraMap ? 11.25f : fineSpacing;
    const int fineN = ultraMap ? 9 : 5;
    const float fineAt[3][2] = {{0, 0}, {1024 * gridSpacing, 0}, {0, 1024 * gridSpacing}};
    IDirect3DVertexBuffer9* fineVb[3] = {};
    for (int c = 0; fineMap && c < 3; ++c)
    {
        float verts[81][7];
        for (int y = 0; y < fineN; ++y)
            for (int x = 0; x < fineN; ++x)
            {
                const float u = x / (fineN - 1.0f), v = y / (fineN - 1.0f);
                const float vert[7] = {fineSpacing * (x - fineN / 2), fineSpacing * (y - fineN / 2), fineHeights[c], u,
                                       1 - v, u, 1 - v};
                memcpy(verts[y * fineN + x], vert, sizeof(vert));
            }
        void* p = nullptr;
        const UINT bytes = static_cast<UINT>(fineN * fineN * sizeof(verts[0]));  // just the chunk's vertices
        CHECK(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &fineVb[c], nullptr));
        CHECK(fineVb[c]->Lock(0, 0, &p, 0));
        memcpy(p, verts, bytes);
        fineVb[c]->Unlock();
    }
    IDirect3DVertexShader9* riverVs = nullptr;
    IDirect3DPixelShader9* riverPs = nullptr;
    IDirect3DVertexDeclaration9* riverDecl = nullptr;
    IDirect3DTexture9* overlayTex[3] = {};  // river0: flood, mask; river1: normal, base; decal: colour
    auto solid = [&](DWORD colour, IDirect3DTexture9** t) {
        if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t, nullptr)))
            return false;
        D3DLOCKED_RECT lr;
        (*t)->LockRect(0, &lr, nullptr, 0);
        *static_cast<DWORD*>(lr.pBits) = colour;
        (*t)->UnlockRect(0);
        return true;
    };
    // water: BtS-style water (magenta, test_btswater.hlsl) over the whole ground at z = 1
    IDirect3DVertexShader9* btsWaterVs = nullptr;
    IDirect3DPixelShader9* btsWaterPs = nullptr;
    IDirect3DVertexDeclaration9* btsWaterDecl = nullptr;
    if (waterOverlay || buildingWater)
    {
        std::vector<DWORD> wvs = Tokens(g_btsWaterVS, sizeof(g_btsWaterVS)), wps = Tokens(g_btsWaterPS, sizeof(g_btsWaterPS));
        CHECK(dev->CreateVertexShader(wvs.data(), &btsWaterVs));
        CHECK(dev->CreatePixelShader(wps.data(), &btsWaterPs));
        const D3DVERTEXELEMENT9 we[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                                        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
                                        D3DDECL_END()};
        CHECK(dev->CreateVertexDeclaration(we, &btsWaterDecl));
    }
    if (tree)
    {
        std::wstring dir = riverDir ? riverDir : L".";
        std::vector<DWORD> vsCode, psCode;
        if (!ReadAll(dir + L"\\tree_vs.bin", vsCode) || !ReadAll(dir + L"\\tree_ps.bin", psCode))
        {
            printf("FAILED: BtS tree shaders not found in %ls\n", dir.c_str());
            return 1;
        }
        CHECK(dev->CreateVertexShader(vsCode.data(), &riverVs));  // (reusing the river slots for the tree)
        CHECK(dev->CreatePixelShader(psCode.data(), &riverPs));
        if (!solid(0xFF808080, &overlayTex[0]))
            return 1;
    }
    if (river0 || river1)
    {
        std::wstring dir = riverDir ? riverDir : L".";
        std::vector<DWORD> vsCode, psCode;
        if (!ReadAll(dir + (river0 ? L"\\river0_vs.bin" : L"\\river1_vs.bin"), vsCode) ||
            !ReadAll(dir + (river0 ? L"\\river0_ps.bin" : L"\\river1_ps.bin"), psCode))
        {
            printf("FAILED: BtS river shaders not found in %ls\n", dir.c_str());
            return 1;
        }
        CHECK(dev->CreateVertexShader(vsCode.data(), &riverVs));
        CHECK(dev->CreatePixelShader(psCode.data(), &riverPs));
        const D3DVERTEXELEMENT9 riverElems[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            {0, 20, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
            {0, 32, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
            {0, 44, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
            D3DDECL_END()};
        CHECK(dev->CreateVertexDeclaration(riverElems, &riverDecl));
        if (!(river0 ? solid(0xFF606060, &overlayTex[0]) && solid(0xFFFFFFFF, &overlayTex[1])
                     : solid(0xFF8080FF, &overlayTex[0]) && solid(0xFF808080, &overlayTex[1])))
            return 1;
    }
    if (decal && !solid(0xFF4080C0, &overlayTex[2]))
        return 1;
    if (effect && !solid(0xFF000040, &overlayTex[2]))  // the effect's colour: blue 64 (effectTex: a .dds of it)
        return 1;
    // building: frame 0 draws the painted-shadow spot with a white decoy (cached as "not a shadow"), which is then
    // released and the black painted shadow created, usually at the same address (reported as "reused").
    if (building && !solid(0xFFFFFFFF, &overlayTex[2]))
        return 1;
    int reused = -1;

    // register form: x/500, y/500, depth 0.5 - z/1000
    const float vp[16] = {1 / 500.0f, 0, 0, 0, 0, 1 / 500.0f, 0, 0, 0, 0, -1 / 1000.0f, 0.5f, 0, 0, 0, 1};
    const float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float zero[16] = {};
    const float scale[4] = {1, 0, 0, 0};
    const float marker[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};  // c20-c23, the proxy's mtxShadow
    const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // x, y, z, uv0, uv1
    const float quad[6][7] = {{-500, -500, 0, 0, 1, 0, 1}, {-500, 500, 0, 0, 0, 0, 0}, {500, 500, 0, 1, 0, 1, 0},
                              {-500, -500, 0, 0, 1, 0, 1}, {500, 500, 0, 1, 0, 1, 0},  {500, -500, 0, 1, 1, 1, 1}};
    const float caster[9] = {-60, -50, 100, 0, 80, 100, 60, -50, 100};
    // fixed-function meshes: x, y, z, normal, uv
    const float blobQuad[6][8] = {{-40, -40, 1, 0, 0, 1, 0, 1}, {-40, 40, 1, 0, 0, 1, 0, 0}, {40, 40, 1, 0, 0, 1, 1, 0},
                                  {-40, -40, 1, 0, 0, 1, 0, 1}, {40, 40, 1, 0, 0, 1, 1, 0},  {40, -40, 1, 0, 0, 1, 1, 1}};
    const float part[3][8] = {{-60, 50, 100, 0, 0, 1, 0, 0}, {0, 180, 100, 0, 0, 1, 0, 0}, {60, 50, 100, 0, 0, 1, 0, 0}};
    // The same, held like a club: WORLD at the grip, 40 above the mesh (a weapon hangs below its origin).
    const float heldPart[3][8] = {{-60, 50, -40, 0, 0, 1, 0, 0}, {0, 180, -40, 0, 0, 1, 0, 0}, {60, 50, -40, 0, 0, 1, 0, 0}};
    auto translation = [](float x, float y) {
        D3DMATRIX m = {};
        m._11 = m._22 = m._33 = m._44 = 1;
        m._41 = x;
        m._42 = y;
        return m;
    };
    D3DMATRIX ffView = translation(0, 0), ffProj;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            ffProj.m[i][j] = vp[j * 4 + i];  // row-vector form of the register-form camera
    auto pixel = [&](D3DLOCKED_RECT& lr, float x, float y) {
        int px = static_cast<int>((x / 500 + 1) * pp.BackBufferWidth / 2),
            py = static_cast<int>((1 - y / 500) * pp.BackBufferHeight / 2);
        return reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + py * lr.Pitch)[px];
    };
    unsigned lit = 0, shadowed = 0, lit2 = 0, shadowed2 = 0, casterRed = 0, blobPixel = 0, partPixel = 0,
             farPixel = 0, buildingPixel = 0, paintedPixel = 0, overlayPixel = 0, hillPixel = 0, footingPixel = 0;
    int restored = 1, reset = 1;
    for (int f = 0; f < frames; ++f)
    {
        if (f == 3)
            reset = SUCCEEDED(dev->Reset(&pp)) ? 1 : 0;
        if (vanish && f == 4)
        {
            // A frame with no ground and no casters (a full-screen menu or screen): nothing but a clear.
            CHECK(dev->BeginScene());
            CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
            CHECK(dev->EndScene());
            CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
            continue;
        }
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));

        // terrain
        CHECK(dev->SetVertexDeclaration(terrainDecl));
        CHECK(dev->SetVertexShader(tvs));
        CHECK(dev->SetPixelShader(tps));
        CHECK(dev->SetVertexShaderConstantF(0, vp, 4));
        CHECK(dev->SetVertexShaderConstantF(4, world, 3));
        CHECK(dev->SetVertexShaderConstantF(7, zero, 4));
        CHECK(dev->SetVertexShaderConstantF(11, scale, 1));
        CHECK(dev->SetVertexShaderConstantF(20, marker, 4));
        for (DWORD i = 0; i < 4; ++i)
            CHECK(dev->SetTexture(i, tex[i]));
        if (sheetEnv[0] != L'0' && sheetEnv[0] != 0)
        {
            // the blend sheet filtered as BtS filters it (bilinear, with mips' filter)
            CHECK(dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
            CHECK(dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
            CHECK(dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR));
        }
        if (slope)
        {
            CHECK(dev->SetStreamSource(0, slopeVb, 0, 28));
            CHECK(dev->SetIndices(slopeIb));
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 25, 0, 32));
            CHECK(dev->SetStreamSource(0, nullptr, 0, 0));
            CHECK(dev->SetIndices(nullptr));
        }
        else if (fineMap)
        {
            if (wideMap)
            {
                // mtxLightmap (a register per column): u = x / 46080 + 0.25, v = -y / 46080 + 0.75, like BtS's
                const float lightmap[8] = {1 / 46080.0f, 0, 0, 0.25f, 0, -1 / 46080.0f, 0, 0.75f};
                CHECK(dev->SetVertexShaderConstantF(9, lightmap, 2));
            }
            for (int c = 0; c < 3; ++c)
            {
                const float placed[12] = {1, 0, 0, fineAt[c][0], 0, 1, 0, fineAt[c][1], 0, 0, 1, 0};
                CHECK(dev->SetVertexShaderConstantF(4, placed, 3));
                CHECK(dev->SetStreamSource(0, fineVb[sharedMap ? 0 : c], 0, 28));
                CHECK(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 8));  // (some of the grid: only its heights matter)
            }
            CHECK(dev->SetStreamSource(0, nullptr, 0, 0));
            CHECK(dev->SetVertexShaderConstantF(4, world, 3));
        }
        else
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, 28));
        IDirect3DVertexShader9* nowVs = nullptr;
        IDirect3DPixelShader9* nowPs = nullptr;
        IDirect3DBaseTexture9* now4 = nullptr;
        float c20[16];
        dev->GetVertexShader(&nowVs);
        dev->GetPixelShader(&nowPs);
        dev->GetTexture(4, &now4);
        dev->GetVertexShaderConstantF(20, c20, 4);
        if (nowVs != tvs || nowPs != tps || now4 || memcmp(c20, marker, sizeof(c20)) != 0)
            restored = 0;
        for (IUnknown* o : {static_cast<IUnknown*>(nowVs), static_cast<IUnknown*>(nowPs), static_cast<IUnknown*>(now4)})
            if (o)
                o->Release();

        if (waterOverlay || buildingWater)
        {
            // x, y, z, coast uv, fog uv (buildingwater: a corner patch, x and y -500 to -400)
            const float hi = buildingWater ? -400.0f : 500.0f;
            const float wq[6][7] = {{-500, -500, 1, 0, 0, 0, 0}, {-500, hi, 1, 0, 0, 0, 0}, {hi, hi, 1, 0, 0, 0, 0},
                                    {-500, -500, 1, 0, 0, 0, 0}, {hi, hi, 1, 0, 0, 0, 0},  {hi, -500, 1, 0, 0, 0, 0}};
            CHECK(dev->SetVertexDeclaration(btsWaterDecl));
            CHECK(dev->SetVertexShader(btsWaterVs));
            CHECK(dev->SetPixelShader(btsWaterPs));
            CHECK(dev->SetVertexShaderConstantF(0, vp, 4));
            CHECK(dev->SetVertexShaderConstantF(4, world, 3));
            CHECK(dev->SetVertexShaderConstantF(7, zero, 2));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, wq, 28));
        }
        if (tree)
        {
            // x, y, z, normal, colour (instance index 0), uv: BtS's tree vertex (FVF 0x152)
            struct TreeVertex
            {
                float x, y, z, nx, ny, nz;
                DWORD instance;
                float u, v;
            };
            TreeVertex tq[6];
            const float corners[6][2] = {{-500, -500}, {-500, 500}, {500, 500}, {-500, -500}, {500, 500}, {500, -500}};
            // treeshadow: uv in the atlas's painted-shadow images (u 0.5-1, v 0.25-0.5), like BtS's tree shadow cards
            const float u = treeShadow ? 0.75f : 0.0f, v0 = treeShadow ? 0.375f : 0.0f;
            for (int v = 0; v < 6; ++v)
                tq[v] = {corners[v][0], corners[v][1], 0, 0, 0, 1, 0, u, v0};
            const float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            float heights[64 * 4] = {};
            CHECK(dev->SetVertexShader(riverVs));
            CHECK(dev->SetPixelShader(riverPs));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1));
            CHECK(dev->SetVertexShaderConstantF(0, heights, 64));   // fHeightOffset0
            CHECK(dev->SetVertexShaderConstantF(64, vp, 4));        // WorldViewProj
            CHECK(dev->SetVertexShaderConstantF(68, zero, 4));      // mtxFOW
            CHECK(dev->SetVertexShaderConstantF(72, rows, 3));      // World
            CHECK(dev->SetVertexShaderConstantF(75, zero, 2));      // fFrameTime, windir
            CHECK(dev->SetTexture(0, overlayTex[0]));
            CHECK(dev->SetTexture(1, tex[1]));  // fog of war
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tq, sizeof(TreeVertex)));
            IDirect3DVertexShader9* afterVs = nullptr;
            dev->GetVertexShader(&afterVs);
            if (afterVs != riverVs)
                restored = 0;
            if (afterVs)
                afterVs->Release();
            CHECK(dev->SetVertexShaderConstantF(0, vp, 4));  // the terrain's mtxWorldViewProj again
            CHECK(dev->SetVertexShaderConstantF(4, world, 3));
        }
        if (riverVs && !tree)
        {
            // x, y, z, uniform uv (TEXCOORD1), normal, binormal, tangent
            float rq[6][14];
            const float corners[6][2] = {{-500, -500}, {-500, 500}, {500, 500}, {-500, -500}, {500, 500}, {500, -500}};
            for (int v = 0; v < 6; ++v)
            {
                const float row[14] = {corners[v][0], corners[v][1], 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0};
                memcpy(rq[v], row, sizeof(row));
            }
            const float world4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            const float light[4] = {0, 0, 1, 0}, diffuse[4] = {0.6f, 0.6f, 0.6f, 0}, ambient[4] = {0.2f, 0.2f, 0.2f, 0};
            CHECK(dev->SetVertexDeclaration(riverDecl));
            CHECK(dev->SetVertexShader(riverVs));
            CHECK(dev->SetPixelShader(riverPs));
            CHECK(dev->SetVertexShaderConstantF(4, world4, 4));
            CHECK(dev->SetVertexShaderConstantF(8, zero, 4));
            CHECK(dev->SetVertexShaderConstantF(12, zero, 2));
            CHECK(dev->SetVertexShaderConstantF(river0 ? 14 : 12, light, 1));  // the effect preshader's light vector
            CHECK(dev->SetPixelShaderConstantF(0, diffuse, 1));
            CHECK(dev->SetPixelShaderConstantF(1, ambient, 1));
            CHECK(dev->SetTexture(0, river0 ? nullptr : overlayTex[0]));
            CHECK(dev->SetTexture(1, tex[1]));  // fog of war
            CHECK(dev->SetTexture(2, river0 ? overlayTex[0] : overlayTex[1]));
            CHECK(dev->SetTexture(3, river0 ? overlayTex[1] : nullptr));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
            CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, rq, 56));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            IDirect3DVertexShader9* afterVs = nullptr;
            IDirect3DPixelShader9* afterPs = nullptr;
            dev->GetVertexShader(&afterVs);
            dev->GetPixelShader(&afterPs);
            if (afterVs != riverVs || afterPs != riverPs)
                restored = 0;
            if (afterVs)
                afterVs->Release();
            if (afterPs)
                afterPs->Release();
            CHECK(dev->SetVertexShaderConstantF(4, world, 3));
            CHECK(dev->SetVertexShaderConstantF(8, zero, 4));
        }
        if (decal)
        {
            const float dq[6][5] = {{-500, -500, 0, 0, 1}, {-500, 500, 0, 0, 0}, {500, 500, 0, 1, 0},
                                    {-500, -500, 0, 0, 1}, {500, 500, 0, 1, 0},  {500, -500, 0, 1, 1}};
            D3DMATRIX identity4 = translation(0, 0);
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
            CHECK(dev->SetTransform(D3DTS_WORLD, &identity4));
            CHECK(dev->SetTransform(D3DTS_VIEW, &ffView));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &ffProj));
            for (DWORD i = 0; i < 4; ++i)
                CHECK(dev->SetTexture(i, nullptr));
            CHECK(dev->SetTexture(0, overlayTex[2]));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
            if (fowDecal)
            {
                CHECK(dev->SetTexture(1, tex[1]));  // the terrain's fog-of-war texture
                CHECK(dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0));
                CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE));
                CHECK(dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE));
                CHECK(dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT));
                CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
                CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT));
            }
            else
            {
                CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
                CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
            }
            CHECK(dev->SetRenderState(D3DRS_ZENABLE, symbol ? FALSE : TRUE));  // the depth buffer is clear: it passes
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
            CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, dq, 20));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
            // the first unused stage (where the proxy adds the shadow) must be as it was
            const DWORD free = fowDecal ? 2 : 1;
            DWORD op = 0, tci = 0;
            IDirect3DBaseTexture9* t = nullptr;
            dev->GetTextureStageState(free, D3DTSS_COLOROP, &op);
            dev->GetTextureStageState(free, D3DTSS_TEXCOORDINDEX, &tci);
            dev->GetTexture(free, &t);
            if (op != D3DTOP_DISABLE || tci != free || t)
                restored = 0;
            if (t)
                t->Release();
            CHECK(dev->SetTexture(1, nullptr));
            CHECK(dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1));
            CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
            CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
        }

        if (effect)
        {
            // x, y, z, uv: a quad at z = 1 seen through a perspective-form camera (w = view z = 1), so it covers the
            // screen as the orthographic camera's ground does; effectui keeps the orthographic camera (an interface).
            const float eq[6][5] = {{-500, -500, 1, 0, 1}, {-500, 500, 1, 0, 0}, {500, 500, 1, 1, 0},
                                    {-500, -500, 1, 0, 1}, {500, 500, 1, 1, 0},  {500, -500, 1, 1, 1}};
            D3DMATRIX identity4 = translation(0, 0), persp = {};
            persp._11 = persp._22 = 1 / 500.0f;
            persp._43 = 0.5f;
            persp._34 = 1;
            DWORD oldLighting = FALSE;
            dev->GetRenderState(D3DRS_LIGHTING, &oldLighting);
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
            CHECK(dev->SetTransform(D3DTS_WORLD, &identity4));
            CHECK(dev->SetTransform(D3DTS_VIEW, &ffView));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, effectUi ? &ffProj : &persp));
            for (DWORD i = 0; i < 4; ++i)
                CHECK(dev->SetTexture(i, nullptr));
            CHECK(dev->SetTexture(0, overlayTex[2]));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
            CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
            CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
            if (effectLit)
            {
                // Lit, but with an emissive-only material (black diffuse and ambient): the lights add nothing.
                D3DMATERIAL9 emissive = {};
                emissive.Emissive.b = emissive.Emissive.a = 1;
                CHECK(dev->SetMaterial(&emissive));
            }
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, effectLit ? TRUE : FALSE));
            CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));  // a sandstorm's glow: over everything
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_SRCBLEND, effectMul ? D3DBLEND_ZERO : D3DBLEND_SRCALPHA));
            CHECK(dev->SetRenderState(D3DRS_DESTBLEND, effectMul ? D3DBLEND_INVSRCCOLOR : D3DBLEND_ONE));
            CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF123456));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, eq, 20));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, oldLighting));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &ffProj));
            // the stage the proxy adds the light on, and the texture factor, must be as they were
            DWORD op = 0, factor = 0;
            IDirect3DBaseTexture9* t = nullptr;
            dev->GetTextureStageState(1, D3DTSS_COLOROP, &op);
            dev->GetTexture(1, &t);
            dev->GetRenderState(D3DRS_TEXTUREFACTOR, &factor);
            if (op != D3DTOP_DISABLE || t || factor != 0xFF123456)
                restored = 0;
            if (t)
                t->Release();
            CHECK(dev->SetTexture(0, nullptr));
        }

        // caster: flat red, alpha 1 (fixed-function pixel stage)
        for (DWORD i = 0; i < 4; ++i)
            CHECK(dev->SetTexture(i, nullptr));
        CHECK(dev->SetPixelShader(nullptr));
        if (building)
        {
            // x, y, z, normal: a lit, depth-writing fixed-function mesh 100 units up, WORLD at (200,300)
            const float mesh[3][6] = {{-60, -50, 90, 0, 0, 1}, {0, 80, 90, 0, 0, 1}, {60, -50, 90, 0, 0, 1}};
            D3DMATRIX at = translation(200, 300);
            at._43 = 10;  // standing on land, above the water (resource ripples at water level don't cast)
            D3DLIGHT9 sun = {};
            sun.Type = D3DLIGHT_DIRECTIONAL;
            sun.Diffuse.r = sun.Diffuse.g = sun.Diffuse.b = 1;
            sun.Direction.z = -1;
            D3DMATERIAL9 material = {};
            material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = material.Diffuse.a = 1;
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL));
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
            CHECK(dev->SetTransform(D3DTS_WORLD, &at));
            CHECK(dev->SetTransform(D3DTS_VIEW, &ffView));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &ffProj));
            CHECK(dev->SetLight(0, &sun));
            CHECK(dev->LightEnable(0, TRUE));
            CHECK(dev->SetMaterial(&material));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, TRUE));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
            CHECK(dev->SetTexture(flagStage, flagPole));  // bound but unused (colour and alpha from diffuse)
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, mesh, 24));
            CHECK(dev->SetTexture(flagStage, nullptr));
            // A building's foundations: part of the mesh below its base (here WORLD at (300,300,10), the triangle
            // 50 below that). Hidden by the terrain in the game (here: colour writes off). Unclipped, it would shade
            // the ground on the sunward side, around (340,300).
            const float footing[3][6] = {{-60, -50, -50, 0, 0, 1}, {0, 80, -50, 0, 0, 1}, {60, -50, -50, 0, 0, 1}};
            D3DMATRIX base = translation(300, 300);
            base._43 = 10;
            CHECK(dev->SetTransform(D3DTS_WORLD, &base));
            CHECK(dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, footing, 24));
            CHECK(dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            CHECK(dev->LightEnable(0, FALSE));
            // A painted building shadow like BtS's (black texture, the shape in alpha), flat on the ground at
            // (-300,300): hidden while buildings cast real shadows.
            const float painted[6][5] = {{-50, -50, 1, 0, 1}, {-50, 50, 1, 0, 0}, {50, 50, 1, 1, 0},
                                         {-50, -50, 1, 0, 1}, {50, 50, 1, 1, 0},  {50, -50, 1, 1, 1}};
            D3DMATRIX there = translation(-300, 300);
            CHECK(dev->SetTransform(D3DTS_WORLD, &there));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
            CHECK(dev->SetTexture(0, overlayTex[2]));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
            CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
            CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
            // world decal: the fog of war on stage 1 (passing the colour through)
            CHECK(dev->SetTexture(1, tex[1]));
            CHECK(dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0));
            CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG2));
            CHECK(dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT));
            CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2));
            CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, painted, 20));
            CHECK(dev->SetTexture(1, nullptr));
            CHECK(dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1));
            CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
            CHECK(dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
            // the same black texture as an interface overlay (no fog of war) at (-300,-300): left alone
            D3DMATRIX ui = translation(-300, -300);
            CHECK(dev->SetTransform(D3DTS_WORLD, &ui));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, painted, 20));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            CHECK(dev->SetTexture(0, nullptr));
        }
        if (blob)
        {
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1));
            CHECK(dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
            CHECK(dev->SetTransform(D3DTS_VIEW, &ffView));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &ffProj));
            D3DMATRIX at = translation(300, 300), away = translation(-300, -300);
            CHECK(dev->SetTransform(D3DTS_WORLD, &at));
            CHECK(dev->SetTexture(0, blob));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, blobQuad, 32));
            CHECK(dev->SetTexture(0, nullptr));
            CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
            CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
            D3DMATRIX grip = translation(300, 300);
            grip._43 = 140;
            CHECK(dev->SetTransform(D3DTS_WORLD, &grip));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, heldPart, 32));
            CHECK(dev->SetTransform(D3DTS_WORLD, &away));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, part, 32));
            D3DMATRIX viewAfter;
            dev->GetTransform(D3DTS_VIEW, &viewAfter);
            if (memcmp(&viewAfter, &ffView, sizeof(viewAfter)) != 0)
                restored = 0;
        }
        CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF0000));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
        CHECK(dev->SetVertexDeclaration(unitDecl));
        CHECK(dev->SetVertexShader(unit));
        float unitBones[12], unitCamera[16];
        memcpy(unitBones, bones, sizeof(unitBones));
        memcpy(unitCamera, vp, sizeof(unitCamera));
        if (old)
        {
            const float place[3] = {37, -23, 11};  // bones relative to the placement; the camera register times it
            for (int r = 0; r < 3; ++r)
                unitBones[r * 4 + 3] -= place[r];
            for (int r = 0; r < 4; ++r)
                unitCamera[r * 4 + 3] += vp[r * 4] * place[0] + vp[r * 4 + 1] * place[1] + vp[r * 4 + 2] * place[2];
        }
        CHECK(dev->SetVertexShaderConstantF(0, unitBones, 3));
        CHECK(dev->SetVertexShaderConstantF(60, unitCamera, 4));
        if (!(vanish && f >= 4))  // vanish: the caster goes after frame 3 (deleted, or out of view)
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, caster, 12));
        float c60[16];
        DWORD writes = 0, blendOp = 0;
        IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
        dev->GetVertexShaderConstantF(60, c60, 4);
        dev->GetRenderState(D3DRS_COLORWRITEENABLE, &writes);
        dev->GetRenderState(D3DRS_BLENDOP, &blendOp);
        dev->GetRenderTarget(0, &rt);
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        if (memcmp(c60, unitCamera, sizeof(c60)) != 0 || writes != 0xF || blendOp != D3DBLENDOP_ADD || rt != bb)
            restored = 0;
        CHECK(dev->EndScene());
        if (f == 2 || f == frames - 1)
        {
            CHECK(dev->GetRenderTargetData(bb, sys));
            D3DLOCKED_RECT lr;
            CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
            unsigned l = pixel(lr, 200, 0) & 0xFF, s = pixel(lr, -100, 0) & 0xFF;
            (f == 2 ? lit : lit2) = l;
            (f == 2 ? shadowed : shadowed2) = s;
            casterRed = (pixel(lr, 0, 0) >> 16) & 0xFF;
            blobPixel = pixel(lr, 300, 300) & 0xFF;
            partPixel = pixel(lr, 200, 400) & 0xFF;
            farPixel = pixel(lr, -400, -200) & 0xFF;
            buildingPixel = pixel(lr, 100, 300) & 0xFF;
            paintedPixel = pixel(lr, -300, 300) & 0xFF;
            overlayPixel = pixel(lr, -300, -300) & 0xFF;
            hillPixel = pixel(lr, -250, 0) & 0xFF;
            footingPixel = pixel(lr, 340, 300) & 0xFF;
            sys->UnlockRect();
        }
        rt->Release();
        bb->Release();
        if (building && f == 0)
        {
            const void* decoy = overlayTex[2];
            overlayTex[2]->Release();
            overlayTex[2] = nullptr;
            // Black, half transparent: a painted shadow. Try for the decoy's address, as happens in the game.
            std::vector<IDirect3DTexture9*> misses;
            for (int attempt = 0; attempt < 32 && !overlayTex[2]; ++attempt)
            {
                IDirect3DTexture9* t = nullptr;
                if (!solid(0x80000000, &t))
                    return 1;
                if (t == decoy || attempt == 31)
                    overlayTex[2] = t;
                else
                    misses.push_back(t);
            }
            for (IDirect3DTexture9* t : misses)
                t->Release();
            reused = overlayTex[2] == decoy ? 1 : 0;
        }
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("SHADOW lit=%u shadowed=%u lit2=%u shadowed2=%u caster=%u restored=%d reset=%d", lit, shadowed, lit2,
           shadowed2, casterRed, restored, reset);
    if (blob)
        printf(" blob=%u part=%u far=%u", blobPixel, partPixel, farPixel);
    if (building)
        printf(" bld=%u painted=%u ui=%u footing=%u reused=%d", buildingPixel, paintedPixel, overlayPixel,
               footingPixel, reused);
    if (ridge)
        printf(" hill=%u", hillPixel);
    printf("\n");
    if (blob)
        blob->Release();
    if (flagPole)
        flagPole->Release();
    if (btsWaterVs)
        btsWaterVs->Release();
    if (btsWaterPs)
        btsWaterPs->Release();
    if (btsWaterDecl)
        btsWaterDecl->Release();
    if (slopeVb)
        slopeVb->Release();
    if (slopeIb)
        slopeIb->Release();
    for (IDirect3DVertexBuffer9* vb : fineVb)
        if (vb)
            vb->Release();
    for (IUnknown* o : {static_cast<IUnknown*>(riverVs), static_cast<IUnknown*>(riverPs), static_cast<IUnknown*>(riverDecl),
                        static_cast<IUnknown*>(overlayTex[0]), static_cast<IUnknown*>(overlayTex[1]),
                        static_cast<IUnknown*>(overlayTex[2])})
        if (o)
            o->Release();
    sys->Release();
    for (IDirect3DTexture9* t : tex)
        t->Release();
    unitDecl->Release();
    terrainDecl->Release();
    unit->Release();
    tps->Release();
    tvs->Release();
    return 0;
}

// Lighting check: a unit-style lit shader (test_lit.hlsl: f3UnitLightDir/Diffuse/AmbientColor c64-c66) draws a
// triangle facing the light over the screen centre; the game's light is diffuse 0.5, ambient (0.1,0.2,0.3), light
// straight down onto it, so BtS's colour is (153,178,204). With [lighting] on, the proxy swaps in the INI colours
// (and direction). Prints "LIGHT pixel=AARRGGBB restored=." (restored = the game's c64-c66 back after the draw).
// With ff: the triangle is fixed function instead, like a building: lit by a directional light in slot 11 (BtS's
// slots are 9 and up; diffuse 0.5, straight down) and D3DRS_AMBIENT (16,32,48), so BtS's colour is (144,160,176);
// restored = light 11 and the ambient back.
// With tex (and ff): the triangle is textured with a 1x1 grey 64 (0.25) texture, modulated by its lit colour, and its
// material has an emissive 0.1, so BtS's colour is (64,64,64) under a light adding up past 1; restored also = the
// material, the texture factor and texture stage 1 back.
// With rt: drawn into a 64x64 render-target texture instead of the back buffer, as BtS draws the interface's 3D views
// (the unit in the lower-left corner, the Civilopedia's models); pixel is its centre. With screen: the same into a
// texture the size of the back buffer, as BtS draws the map behind the Civilopedia.
static int LightScene(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS& pp, bool ff, bool rt, bool screen, bool tex)
{
    rt = rt || screen;
    std::vector<DWORD> code = Tokens(g_litVS, sizeof(g_litVS));
    IDirect3DVertexShader9* vs = nullptr;
    CHECK(dev->CreateVertexShader(code.data(), &vs));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    IDirect3DTexture9* view = nullptr;
    IDirect3DSurface9* viewSurf = nullptr;
    UINT width = pp.BackBufferWidth, height = pp.BackBufferHeight;
    D3DFORMAT format = pp.BackBufferFormat;
    if (rt)
    {
        width = screen ? pp.BackBufferWidth : 64;
        height = screen ? pp.BackBufferHeight : 64;
        format = D3DFMT_A8R8G8B8;
        CHECK(dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &view, nullptr));
        CHECK(view->GetSurfaceLevel(0, &viewSurf));
    }
    CHECK(dev->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float light[12] = {0, 0, -1, 0, 0.5f, 0.5f, 0.5f, 0, 0.1f, 0.2f, 0.3f, 0};  // direction, diffuse, ambient
    // x, y, z, normal (facing +z, towards the light)
    const float tri[3][6] = {{-0.5f, -0.5f, 0.5f, 0, 0, 1}, {0, 0.5f, 0.5f, 0, 0, 1}, {0.5f, -0.5f, 0.5f, 0, 0, 1}};
    // ...with a texture coordinate (tex)
    float triUv[3][8] = {};
    for (int v = 0; v < 3; ++v)
        memcpy(triUv[v], tri[v], sizeof(tri[v]));
    IDirect3DTexture9* grey = nullptr;
    if (tex)
    {
        CHECK(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &grey, nullptr));
        D3DLOCKED_RECT glr;
        CHECK(grey->LockRect(0, &glr, nullptr, 0));
        *static_cast<DWORD*>(glr.pBits) = 0xFF404040;
        grey->UnlockRect(0);
    }
    DWORD pixel = 0;
    int restored = 1;
    for (int f = 0; f < 2; ++f)
    {
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        if (rt)
        {
            CHECK(dev->SetRenderTarget(0, viewSurf));
            CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0));
        }
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        CHECK(dev->SetPixelShader(nullptr));
        CHECK(dev->SetTexture(0, nullptr));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
        if (ff)
        {
            D3DMATRIX id;
            memcpy(&id, identity, sizeof(id));
            D3DLIGHT9 sun = {};
            sun.Type = D3DLIGHT_DIRECTIONAL;
            sun.Diffuse.r = sun.Diffuse.g = sun.Diffuse.b = 0.5f;
            sun.Direction.z = -1;
            D3DMATERIAL9 material = {};
            material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = material.Diffuse.a = 1;
            material.Ambient = material.Diffuse;
            if (tex)
            {
                material.Emissive.r = material.Emissive.g = material.Emissive.b = 0.1f;
                CHECK(dev->SetTexture(0, grey));
                CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
                CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
                CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE));
                CHECK(dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF123456));
            }
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | (tex ? D3DFVF_TEX1 : 0)));
            CHECK(dev->SetTransform(D3DTS_WORLD, &id));
            CHECK(dev->SetTransform(D3DTS_VIEW, &id));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &id));
            CHECK(dev->SetLight(11, &sun));
            CHECK(dev->LightEnable(11, TRUE));
            CHECK(dev->SetMaterial(&material));
            CHECK(dev->SetRenderState(D3DRS_AMBIENT, 0x00102030));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, TRUE));
            if (tex)
                CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triUv, 32));
            else
                CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 24));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            D3DLIGHT9 after = {};
            DWORD ambient = 0;
            dev->GetLight(11, &after);
            dev->GetRenderState(D3DRS_AMBIENT, &ambient);
            if (after.Diffuse.r != 0.5f || after.Direction.z != -1 || ambient != 0x00102030)
                restored = 0;
            if (tex)
            {
                D3DMATERIAL9 m = {};
                DWORD factor = 0, op1 = 0;
                dev->GetMaterial(&m);
                dev->GetRenderState(D3DRS_TEXTUREFACTOR, &factor);
                dev->GetTextureStageState(1, D3DTSS_COLOROP, &op1);
                if (m.Emissive.r != 0.1f || factor != 0xFF123456 || op1 != D3DTOP_DISABLE)
                    restored = 0;
                CHECK(dev->SetTexture(0, nullptr));
                CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
                CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
            }
        }
        else
        {
            CHECK(dev->SetVertexDeclaration(decl));
            CHECK(dev->SetVertexShader(vs));
            CHECK(dev->SetVertexShaderConstantF(0, bones, 3));
            CHECK(dev->SetVertexShaderConstantF(60, identity, 4));
            CHECK(dev->SetVertexShaderConstantF(64, light, 3));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 24));
            float after[12];
            dev->GetVertexShaderConstantF(64, after, 3);
            if (memcmp(after, light, sizeof(after)) != 0)
                restored = 0;
        }
        if (rt)
            CHECK(dev->SetRenderTarget(0, bb));
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(rt ? viewSurf : bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        pixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + (height / 2) * lr.Pitch)[width / 2];
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("LIGHT pixel=%08lx restored=%d\n", static_cast<unsigned long>(pixel), restored);
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    if (grey)
        grey->Release();
    if (viewSurf)
        viewSurf->Release();
    if (view)
        view->Release();
    sys->Release();
    bb->Release();
    decl->Release();
    vs->Release();
    return 0;
}

// Water on models, run with [water] SurfaceTextures listing surfacePath: a fixed-function quad like an oasis' pool
// (xyz, diffuse, one 2D texture coordinate; unlit, alpha-blended, depth writes) textured with surfacePath, at z = 0
// under a top-down orthographic camera (500 world units = half the screen), over a black screen, three frames. Prints
// "SURFACE pixel=. restored=." (the centre pixel; restored = no shaders bound and the draw's own render states and
// texture back after it).
static int SurfaceScene(IDirect3DDevice9* dev, const wchar_t* surfacePath)
{
    IDirect3DTexture9* tex = LoadDdsTop(dev, surfacePath);
    if (!tex)
        return 1;
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    CHECK(dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    // The camera 1000 above the origin looking down; depth 0..2000 -> 0..1.
    D3DMATRIX view = {}, proj = {}, world = {};
    view._11 = view._22 = 1, view._33 = -1, view._43 = 1000, view._44 = 1;
    proj._11 = proj._22 = 1.0f / 500, proj._33 = 1.0f / 2000, proj._44 = 1;
    world._11 = world._22 = world._33 = world._44 = 1;
    struct Vertex
    {
        float x, y, z;
        DWORD colour;
        float u, v;
    };
    const Vertex quad[6] = {{-400, -400, 0, 0xFFFFFFFF, 0, 1}, {-400, 400, 0, 0xFFFFFFFF, 0, 0},
                            {400, 400, 0, 0xFFFFFFFF, 1, 0},   {-400, -400, 0, 0xFFFFFFFF, 0, 1},
                            {400, 400, 0, 0xFFFFFFFF, 1, 0},   {400, -400, 0, 0xFFFFFFFF, 1, 1}};
    DWORD pixel = 0;
    int restored = 1;
    for (int frame = 0; frame < 3; ++frame)
    {
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetVertexShader(nullptr));
        CHECK(dev->SetPixelShader(nullptr));
        CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1));
        CHECK(dev->SetTransform(D3DTS_WORLD, &world));
        CHECK(dev->SetTransform(D3DTS_VIEW, &view));
        CHECK(dev->SetTransform(D3DTS_PROJECTION, &proj));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
        CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
        CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
        CHECK(dev->SetTexture(0, tex));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
        CHECK(dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE));
        CHECK(dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex)));
        // the game's state as it set it
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DBaseTexture9* t0 = nullptr;
        DWORD src = 0, dst = 0, zwrite = 0, bias = 1;
        dev->GetVertexShader(&vs);
        dev->GetPixelShader(&ps);
        dev->GetTexture(0, &t0);
        dev->GetRenderState(D3DRS_SRCBLEND, &src);
        dev->GetRenderState(D3DRS_DESTBLEND, &dst);
        dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
        dev->GetRenderState(D3DRS_DEPTHBIAS, &bias);
        if (vs || ps || t0 != tex || src != D3DBLEND_SRCALPHA || dst != D3DBLEND_INVSRCALPHA || !zwrite || bias != 0)
            restored = 0;
        for (IUnknown* o : {static_cast<IUnknown*>(vs), static_cast<IUnknown*>(ps), static_cast<IUnknown*>(t0)})
            if (o)
                o->Release();
        CHECK(dev->SetTexture(0, nullptr));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        pixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + (bd.Height / 2) * lr.Pitch)[bd.Width / 2];
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("SURFACE pixel=%08lx restored=%d\n", static_cast<unsigned long>(pixel), restored);
    sys->Release();
    bb->Release();
    tex->Release();
    return 0;
}

// Our water shader (shaders\water.hlsl) against the original, Colonization's (the game's compiled
// shaders, extracted from its compiled effects into colDir): both drawn over a 16x16 grid of water with the same random textures
// (normal map, coast fade, fog, grid, environment cube, reflection, refraction) and realistic constants, a different
// view direction at every vertex. Prints "WATERCMP max=. differing=. pixels=. colours=." (the largest difference in
// any channel, how many pixels differ at all, and how many distinct colours the original drew, to show the picture
// isn't flat).
// Our wave shader (wave.hlsl) against BtS's own compiled one (Civ4Wave.fx Wave_Shader P0, from gameDir), with random
// textures, vertex colours and texture matrices, drawn alpha-blended over black (so the opacity shows in the colour):
//   0 BtS's, the base layer offset 0.5        1 ours, the same                   = 0
//   2 ours, the base offset 10.5 (a model's opacity 0.5)                        = 0 with the opacity halved
//   3 ours, the base offset 10 (opacity 0)                                      = black
// Prints "WAVECMP same=<worst difference 1 vs 0> half=<worst 2 vs 0 x 0.5> zero=<brightest of 3> lit=<channels of 0
// above 8>".
static int WaveCompareScene(IDirect3DDevice9* dev, const std::wstring& gameDir)
{
    std::vector<DWORD> vsGame, psGame;
    if (!ReadAll(gameDir + L"\\wave_vs.bin", vsGame) || !ReadAll(gameDir + L"\\wave_ps.bin", psGame))
    {
        printf("FAILED: BtS's wave shaders not found in %ls\n", gameDir.c_str());
        return 1;
    }
    std::vector<DWORD> vsOurs = Tokens(g_waveVS, sizeof(g_waveVS)), psOurs = Tokens(g_wavePS, sizeof(g_wavePS));
    IDirect3DVertexShader9* vs[2] = {};
    IDirect3DPixelShader9* ps[2] = {};
    CHECK(dev->CreateVertexShader(vsGame.data(), &vs[0]));
    CHECK(dev->CreatePixelShader(psGame.data(), &ps[0]));
    CHECK(dev->CreateVertexShader(vsOurs.data(), &vs[1]));
    CHECK(dev->CreatePixelShader(psOurs.data(), &ps[1]));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                       {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));
    unsigned seed = 4242;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0xFF; };
    IDirect3DTexture9* tex[3] = {};
    for (int st = 0; st < 3; ++st)
    {
        CHECK(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[st], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(tex[st]->LockRect(0, &lr, nullptr, 0));
        for (UINT y = 0; y < 32; ++y)
            for (UINT x = 0; x < 32; ++x)
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] =
                    rnd() << 24 | rnd() << 16 | rnd() << 8 | rnd();
        tex[st]->UnlockRect(0);
    }
    struct V
    {
        float x, y, z;
        DWORD colour;
        float u, v;
    };
    std::vector<V> verts;
    for (int gy = 0; gy < 12; ++gy)
        for (int gx = 0; gx < 12; ++gx)
        {
            auto at = [&](int ix, int iy) {
                return V{-500 + 1000 * ix / 12.0f, -500 + 1000 * iy / 12.0f, 0,
                         static_cast<DWORD>(rnd()) << 24 | 0xFFFFFF, ix / 12.0f, iy / 12.0f};
            };
            V a = at(gx, gy), b = at(gx + 1, gy), c = at(gx, gy + 1), d = at(gx + 1, gy + 1);
            verts.insert(verts.end(), {a, b, d, a, d, c});
        }
    const float wvp[16] = {0.002f, 0, 0, 0, 0, 0.002f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 1};
    const float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float fow[8] = {0.0008f, 0, 0, 0.5f, 0, 0.0009f, 0, 0.45f};
    const float decal[8] = {0.3f, 1.2f, 0.1f, 0.05f, 1.4f, -0.2f, 0.2f, 0.1f};
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    CHECK(dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    std::vector<DWORD> image[5];
    for (int which = 0; which < 5; ++which)
    {
        // The base row's constant part: 0.5 split between z and w (any split adds up the same), or 10.5 or 10.
        const float offset = which == 2 ? 10.5f : which == 3 ? 10.0f : 0.5f;
        // Ours: the light (PS c0) unchanged, or (which 4) red 1, green 0.5, blue 0.25.
        const float light[4] = {1, which == 4 ? 0.5f : 1.0f, which == 4 ? 0.25f : 1.0f, 1};
        const float base[8] = {1, 0, offset * 0.25f, offset * 0.75f, 0, 3, -1, 0};
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
        CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(vs[which ? 1 : 0]));
        CHECK(dev->SetPixelShader(ps[which ? 1 : 0]));
        CHECK(dev->SetVertexShaderConstantF(0, wvp, 4));
        CHECK(dev->SetVertexShaderConstantF(4, world, 3));
        CHECK(dev->SetVertexShaderConstantF(7, fow, 2));
        CHECK(dev->SetVertexShaderConstantF(9, base, 2));
        CHECK(dev->SetVertexShaderConstantF(11, decal, 2));
        CHECK(dev->SetPixelShaderConstantF(0, light, 1));
        for (int st = 0; st < 3; ++st)
        {
            CHECK(dev->SetTexture(st, tex[st]));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        }
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        for (UINT y = 0; y < bd.Height; ++y)
            for (UINT x = 0; x < bd.Width; ++x)
                image[which].push_back(reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x]);
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    int same = 0, half = 0, zero = 0, tinted = 0;
    size_t lit = 0;
    const float tint[3] = {0.25f, 0.5f, 1.0f};  // the light of which 4: blue, green, red (the order of the bytes)
    for (size_t i = 0; i < image[0].size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            const int c0 = (image[0][i] >> (8 * k)) & 0xFF, c1 = (image[1][i] >> (8 * k)) & 0xFF,
                      c2 = (image[2][i] >> (8 * k)) & 0xFF, c3 = (image[3][i] >> (8 * k)) & 0xFF,
                      c4 = (image[4][i] >> (8 * k)) & 0xFF;
            tinted = (std::max)(tinted, static_cast<int>(std::fabs(c4 - c0 * tint[k]) + 0.5f));
            same = (std::max)(same, abs(c1 - c0));
            // over black: colour x opacity, so half the opacity is half the colour (the colour's own alpha applied
            // first, so the exact value can be a step off either way)
            half = (std::max)(half, abs(2 * c2 - c0));
            zero = (std::max)(zero, c3);
            lit += c0 > 8;
        }
    printf("WAVECMP same=%d half=%d zero=%d lit=%zu tinted=%d\n", same, half, zero, lit, tinted);

    // Draped (Shader Model 3, vertex textures), seen by a camera tilted so height shows (clip y takes 0.002 z):
    //   4 ours flat                              5 draped, the ground unknown (-10000) = 4
    //   6 draped, the ground 100 up: the waves lifted onto it, so the picture moves
    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);
    IDirect3D9* d3d9 = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    D3DDISPLAYMODE mode = {};
    const bool vtf = caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) && SUCCEEDED(dev->GetDirect3D(&d3d9)) &&
                     SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode)) &&
                     SUCCEEDED(d3d9->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                       D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_R32F));
    if (d3d9)
        d3d9->Release();
    if (!vtf)
        printf("WAVEDRAPE skipped: no vertex textures\n");
    else
    {
        std::vector<DWORD> vsDrapeCode = Tokens(g_waveDrapeVS, sizeof(g_waveDrapeVS)),
                           psDrapeCode = Tokens(g_waveDrapePS, sizeof(g_waveDrapePS));
        IDirect3DVertexShader9* vsDrape = nullptr;
        IDirect3DPixelShader9* psDrape = nullptr;
        CHECK(dev->CreateVertexShader(vsDrapeCode.data(), &vsDrape));
        CHECK(dev->CreatePixelShader(psDrapeCode.data(), &psDrape));
        IDirect3DTexture9* ground[2] = {};
        for (int g = 0; g < 2; ++g)
        {
            CHECK(dev->CreateTexture(64, 64, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &ground[g], nullptr));
            D3DLOCKED_RECT lr;
            CHECK(ground[g]->LockRect(0, &lr, nullptr, 0));
            for (UINT y = 0; y < 64; ++y)
                for (UINT x = 0; x < 64; ++x)
                    reinterpret_cast<float*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = g ? 100.0f : -10000.0f;
            ground[g]->UnlockRect(0);
        }
        const float tilted[16] = {0.002f, 0, 0, 0, 0, 0.002f, 0.002f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 1};
        const float drapeGrid[4] = {1.0f / 22.5f, 1.0f / 64, 1.5f, 0};
        const float base0[8] = {1, 0, 0, 0.5f, 0, 3, -1, 0};
        std::vector<DWORD> drapeImage[3];
        for (int which = 0; which < 3; ++which)
        {
            CHECK(dev->BeginScene());
            CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
            CHECK(dev->SetVertexDeclaration(decl));
            CHECK(dev->SetVertexShader(which ? vsDrape : vs[1]));
            CHECK(dev->SetPixelShader(which ? psDrape : ps[1]));
            CHECK(dev->SetVertexShaderConstantF(0, tilted, 4));
            CHECK(dev->SetVertexShaderConstantF(4, world, 3));
            CHECK(dev->SetVertexShaderConstantF(7, fow, 2));
            CHECK(dev->SetVertexShaderConstantF(9, base0, 2));
            CHECK(dev->SetVertexShaderConstantF(11, decal, 2));
            const float noChange[4] = {1, 1, 1, 1};  // the light unchanged (PS c0)
            CHECK(dev->SetPixelShaderConstantF(0, noChange, 1));
            CHECK(dev->SetVertexShaderConstantF(13, tilted, 4));  // the placement is the identity: VP = WVP
            CHECK(dev->SetVertexShaderConstantF(17, drapeGrid, 1));
            CHECK(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, which ? ground[which - 1] : nullptr));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            for (int st = 0; st < 3; ++st)
                CHECK(dev->SetTexture(st, tex[st]));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
            CHECK(dev->EndScene());
            CHECK(dev->GetRenderTargetData(bb, sys));
            D3DLOCKED_RECT lr;
            CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
            for (UINT y = 0; y < bd.Height; ++y)
                for (UINT x = 0; x < bd.Width; ++x)
                    drapeImage[which].push_back(
                        reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x]);
            sys->UnlockRect();
            CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
        }
        int flat = 0;
        size_t moved = 0;
        for (size_t i = 0; i < drapeImage[0].size(); ++i)
            for (int k = 0; k < 3; ++k)
            {
                const int a = (drapeImage[0][i] >> (8 * k)) & 0xFF, b = (drapeImage[1][i] >> (8 * k)) & 0xFF,
                          c = (drapeImage[2][i] >> (8 * k)) & 0xFF;
                flat = (std::max)(flat, abs(b - a));
                moved += c != a;
            }
        printf("WAVEDRAPE flat=%d moved=%zu\n", flat, moved);
        CHECK(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr));
        ground[0]->Release();
        ground[1]->Release();
        vsDrape->Release();
        psDrape->Release();
    }
    for (int st = 0; st < 3; ++st)
    {
        CHECK(dev->SetTexture(st, nullptr));
        tex[st]->Release();
    }
    for (int k = 0; k < 2; ++k)
        vs[k]->Release(), ps[k]->Release();
    decl->Release();
    sys->Release();
    bb->Release();
    return 0;
}

// BtS's culture border shader (CultureBOrderShader.fx TCultureBorder_Shader14 P0 and P1, from gameDir) on a 17 x 17
// grid over a 400-unit square at z 0, as BtS draws a plot's border (position + uv, no depth writes, alpha-blended),
// with random border and fog textures, seen from straight above (the camera 500 up, perspective).
//   Through the proxy (whatever the INI says; with [debug] TerrainVertsPerPlot 33/65 it draws them with ours):
//     plain: the border over black, no depth in the way. Prints "BORDERPLAIN p=<pass> sum=<all channels> lit=<above 8>"
//     ridge: a black ridge 10 units above the border, drawn first with depth writes, then the border. Prints
//            "BORDERRIDGE p=<pass> visible=<channels above 8>"
//   Ours directly (border.hlsl, Shader Model 3 vertex textures), the proxy not replacing anything (its INI's Games):
//     moved 0 units, no heights: against BtS's drawn the same. Prints "BORDERCMP same0=<worst P0> same1=<worst P1>"
//     heights unknown (-10000), then 100 up: "BORDERLIFT flat=<worst vs no heights> moved=<channels changed>"
static int BorderScene(IDirect3DDevice9* dev, const std::wstring& gameDir)
{
    std::vector<DWORD> code[4];
    const wchar_t* names[4] = {L"border0_vs.bin", L"border0_ps.bin", L"border1_vs.bin", L"border1_ps.bin"};
    for (int k = 0; k < 4; ++k)
        if (!ReadAll(gameDir + L"\\" + names[k], code[k]))
        {
            printf("FAILED: BtS's border shaders not found in %ls\n", gameDir.c_str());
            return 1;
        }
    IDirect3DVertexShader9* vs[2] = {};
    IDirect3DPixelShader9* ps[2] = {};
    for (int p = 0; p < 2; ++p)
    {
        CHECK(dev->CreateVertexShader(code[2 * p].data(), &vs[p]));
        CHECK(dev->CreatePixelShader(code[2 * p + 1].data(), &ps[p]));
    }
    unsigned seed = 1717;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0xFF; };
    IDirect3DTexture9* tex[3] = {};
    for (int st = 0; st < 3; ++st)
    {
        CHECK(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[st], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(tex[st]->LockRect(0, &lr, nullptr, 0));
        for (UINT y = 0; y < 32; ++y)
            for (UINT x = 0; x < 32; ++x)
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] =
                    rnd() << 24 | rnd() << 16 | rnd() << 8 | rnd();
        tex[st]->UnlockRect(0);
    }
    struct V
    {
        float x, y, z, u, v;
    };
    std::vector<V> verts;
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 16; ++gx)
        {
            auto at = [&](int ix, int iy) { return V{-200 + 25.0f * ix, -200 + 25.0f * iy, 0, ix / 16.0f, iy / 16.0f}; };
            V a = at(gx, gy), b = at(gx + 1, gy), c = at(gx, gy + 1), d = at(gx + 1, gy + 1);
            verts.insert(verts.end(), {a, b, d, a, d, c});
        }
    // The camera 500 up looking down (+y up the screen), 60 degree view, near 10, far 5000: clip rows.
    const float s = 1.7320508f, q = 5000.0f / 4990.0f;
    const float wvp[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, -q, 500 * q - 10 * q, 0, 0, -1, 500};
    const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float fow[8] = {0.0011f, 0, 0, 0.5f, 0, -0.0012f, 0, 0.45f};
    const float mats[2][16] = {{0.9f, 0.2f, 0.05f, 0, -0.1f, 1.1f, 0.3f, 0, 1.3f, 0, 0.25f, 0, 0, 1.2f, 0.5f, 0},
                               {0, -1.0f, 0.75f, 0, 1.0f, 0, 0.25f, 0, 0.6f, 0.4f, 0.1f, 0, -0.4f, 0.6f, 0.2f, 0}};
    const float colours[2][8] = {{0.31f, 0.808f, 0.078f, 1, 0.9f, 0.2f, 0.4f, 0.7f},
                                 {0.2f, 0.4f, 0.9f, 0.8f, 1, 1, 0.3f, 0.5f}};
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    CHECK(dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    auto capture = [&](std::vector<DWORD>& out) -> int {
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        out.clear();
        for (UINT y = 0; y < bd.Height; ++y)
            for (UINT x = 0; x < bd.Width; ++x)
                out.push_back(reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x] &
                              0xFFFFFF);
        sys->UnlockRect();
        return 0;
    };
    // One frame: (the ridge,) then the border with pass p's shaders as given (BtS's: the proxy may put ours in).
    auto frame = [&](int p, bool ridge, IDirect3DVertexShader9* v, IDirect3DPixelShader9* f, const float* extra,
                     std::vector<DWORD>& out) -> int {
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, TRUE));
        CHECK(dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
        if (ridge)
        {
            // Black, depth written, 10 above the border and wider than it.
            struct R
            {
                float x, y, z;
                DWORD c;
            } r[6] = {{-300, -300, 10, 0xFF000000}, {300, -300, 10, 0xFF000000}, {300, 300, 10, 0xFF000000},
                      {-300, -300, 10, 0xFF000000}, {300, 300, 10, 0xFF000000}, {-300, 300, 10, 0xFF000000}};
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetPixelShader(nullptr));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
            CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
            CHECK(dev->SetTexture(0, nullptr));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
            CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
            D3DMATRIX id = {}, view = {}, proj = {};
            id._11 = id._22 = id._33 = id._44 = 1;
            view = id;  // the rows above as one matrix: world -> clip, given as the projection (D3D's transposed)
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    proj.m[j][i] = wvp[i * 4 + j];
            CHECK(dev->SetTransform(D3DTS_WORLD, &id));
            CHECK(dev->SetTransform(D3DTS_VIEW, &view));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &proj));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, r, sizeof(R)));
        }
        CHECK(dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        CHECK(dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
        CHECK(dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
        CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
        CHECK(dev->SetVertexShader(v));
        CHECK(dev->SetPixelShader(f));
        CHECK(dev->SetVertexShaderConstantF(0, wvp, 4));
        CHECK(dev->SetVertexShaderConstantF(4, world, 4));
        CHECK(dev->SetVertexShaderConstantF(8, fow, 2));
        CHECK(dev->SetVertexShaderConstantF(10, mats[p], 4));
        if (extra)
            CHECK(dev->SetVertexShaderConstantF(14, extra, 6));
        CHECK(dev->SetPixelShaderConstantF(0, colours[p], 2));
        for (int st = 0; st < 3; ++st)
        {
            CHECK(dev->SetTexture(st, tex[st]));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        }
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
        CHECK(dev->EndScene());
        if (capture(out))
            return 1;
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
        return 0;
    };
    auto lit = [](const std::vector<DWORD>& img) {
        size_t n = 0;
        for (DWORD c : img)
            for (int k = 0; k < 3; ++k)
                n += ((c >> (8 * k)) & 0xFF) > 8;
        return n;
    };
    auto worst = [](const std::vector<DWORD>& a, const std::vector<DWORD>& b) {
        int w = 0;
        for (size_t i = 0; i < a.size(); ++i)
            for (int k = 0; k < 3; ++k)
                w = (std::max)(w, abs(static_cast<int>((a[i] >> (8 * k)) & 0xFF) - static_cast<int>((b[i] >> (8 * k)) & 0xFF)));
        return w;
    };
    std::vector<DWORD> game[2], img;
    for (int p = 0; p < 2; ++p)
    {
        if (frame(p, false, vs[p], ps[p], nullptr, game[p]))
            return 1;
        unsigned long long sum = 0;
        for (DWORD c : game[p])
            sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
        printf("BORDERPLAIN p=%d sum=%llu lit=%zu\n", p, sum, lit(game[p]));
        if (frame(p, true, vs[p], ps[p], nullptr, img))
            return 1;
        printf("BORDERRIDGE p=%d visible=%zu\n", p, lit(img));
    }

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);
    IDirect3D9* d3d9 = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    D3DDISPLAYMODE mode = {};
    const bool vtf = caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) && SUCCEEDED(dev->GetDirect3D(&d3d9)) &&
                     SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode)) &&
                     SUCCEEDED(d3d9->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                       D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_R32F));
    if (d3d9)
        d3d9->Release();
    if (!vtf)
        printf("BORDERCMP skipped: no vertex textures\n");
    else
    {
        std::vector<DWORD> vsCode = Tokens(g_borderVS, sizeof(g_borderVS)), psCode = Tokens(g_borderPS, sizeof(g_borderPS));
        IDirect3DVertexShader9* ourVs = nullptr;
        IDirect3DPixelShader9* ourPs = nullptr;
        CHECK(dev->CreateVertexShader(vsCode.data(), &ourVs));
        CHECK(dev->CreatePixelShader(psCode.data(), &ourPs));
        IDirect3DTexture9* ground[2] = {};
        for (int g = 0; g < 2; ++g)
        {
            CHECK(dev->CreateTexture(64, 64, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &ground[g], nullptr));
            D3DLOCKED_RECT lr;
            CHECK(ground[g]->LockRect(0, &lr, nullptr, 0));
            for (UINT y = 0; y < 64; ++y)
                for (UINT x = 0; x < 64; ++x)
                    reinterpret_cast<float*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = g ? 100.0f : -10000.0f;
            ground[g]->UnlockRect(0);
        }
        // c14-c17 the view-projection (the placement is the identity), c18 the grid, lift and pull, c19 the camera.
        float extra[24] = {};
        memcpy(extra, wvp, sizeof(wvp));
        const float tail[8] = {1.0f / 22.5f, 1.0f / 64, -1e6f, 0, 0, 0, 500, 1};
        memcpy(extra + 16, tail, sizeof(tail));
        int same[2] = {};
        for (int p = 0; p < 2; ++p)
        {
            CHECK(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr));
            if (frame(p, false, ourVs, ourPs, extra, img))
                return 1;
            same[p] = worst(img, game[p]);
        }
        printf("BORDERCMP same0=%d same1=%d\n", same[0], same[1]);
        extra[18] = 1.5f;  // lifted where the ground is above
        std::vector<DWORD> lifted[2];
        for (int g = 0; g < 2; ++g)
        {
            CHECK(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, ground[g]));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            CHECK(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            if (frame(0, false, ourVs, ourPs, extra, lifted[g]))
                return 1;
        }
        size_t moved = 0;
        for (size_t i = 0; i < lifted[1].size(); ++i)
            moved += lifted[1][i] != game[0][i];
        printf("BORDERLIFT flat=%d moved=%zu\n", worst(lifted[0], game[0]), moved);
        CHECK(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr));
        ground[0]->Release();
        ground[1]->Release();
        ourVs->Release();
        ourPs->Release();
    }
    for (int st = 0; st < 3; ++st)
    {
        CHECK(dev->SetTexture(st, nullptr));
        tex[st]->Release();
    }
    for (int p = 0; p < 2; ++p)
        vs[p]->Release(), ps[p]->Release();
    sys->Release();
    bb->Release();
    return 0;
}

static int WaterCompareScene(IDirect3DDevice9* dev, const std::wstring& colDir)
{
    std::vector<DWORD> vsOrig, psOrig;
    if (!ReadAll(colDir + L"\\vs_1_1_2d8d0013f830bfbb.bin", vsOrig) ||
        !ReadAll(colDir + L"\\ps_2_0_d03b0dfb6f9efa7c.bin", psOrig))
    {
        printf("FAILED: Colonization's water shaders not found in %ls\n", colDir.c_str());
        return 1;
    }
    std::vector<DWORD> vsOurs = Tokens(g_waterVS, sizeof(g_waterVS)), psOurs = Tokens(g_waterPS, sizeof(g_waterPS));
    IDirect3DVertexShader9* vs[2] = {};
    IDirect3DPixelShader9* ps[2] = {};
    CHECK(dev->CreateVertexShader(vsOrig.data(), &vs[0]));
    CHECK(dev->CreatePixelShader(psOrig.data(), &ps[0]));
    CHECK(dev->CreateVertexShader(vsOurs.data(), &vs[1]));
    CHECK(dev->CreatePixelShader(psOurs.data(), &ps[1]));
    const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                                       {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
                                       D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));

    // Random textures (a fixed seed), alpha too.
    unsigned seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0xFF; };
    auto fill = [&](IDirect3DTexture9* t, UINT size, unsigned maxValue) {
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(0, &lr, nullptr, 0)))
            return;
        for (UINT y = 0; y < size; ++y)
            for (UINT x = 0; x < size; ++x)
            {
                DWORD c = 0;
                for (int k = 0; k < 4; ++k)
                    c |= static_cast<DWORD>(rnd() * maxValue / 255) << (8 * k);
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = c;
            }
        t->UnlockRect(0);
    };
    const UINT sizes[7] = {64, 64, 32, 32, 0, 64, 64};
    const unsigned maxes[7] = {255, 255, 255, 40, 255, 255, 255};  // the grid dim, so the sum stays in range
    IDirect3DBaseTexture9* tex[7] = {};
    for (int st = 0; st < 7; ++st)
    {
        if (st == 4)
        {
            IDirect3DCubeTexture9* cube = nullptr;
            CHECK(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr));
            for (int face = 0; face < 6; ++face)
            {
                D3DLOCKED_RECT lr;
                CHECK(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &lr, nullptr, 0));
                for (UINT y = 0; y < 16; ++y)
                    for (UINT x = 0; x < 16; ++x)
                    {
                        DWORD c = 0;
                        for (int k = 0; k < 4; ++k)
                            c |= static_cast<DWORD>(rnd()) << (8 * k);
                        reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = c;
                    }
                cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
            }
            tex[st] = cube;
            continue;
        }
        IDirect3DTexture9* t = nullptr;
        CHECK(dev->CreateTexture(sizes[st], sizes[st], 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr));
        fill(t, sizes[st], maxes[st]);
        tex[st] = t;
    }

    // A 16x16 grid over world (-500..500)^2 at z = 0, coast and fog uv across it.
    struct V
    {
        float x, y, z, u1, v1, u2, v2;
    };
    std::vector<V> verts;
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 16; ++gx)
        {
            auto at = [&](int ix, int iy) {
                float u = ix / 16.0f, v = iy / 16.0f;
                return V{-500 + 1000 * u, -500 + 1000 * v, 0, u, v, 1 - v, u};
            };
            V a = at(gx, gy), b = at(gx + 1, gy), c = at(gx, gy + 1), d = at(gx + 1, gy + 1);
            verts.insert(verts.end(), {a, b, d, a, d, c});
        }
    // Constants as the water code sets them (raw registers): an orthographic view of the grid, the eye above one
    // corner, projective reflection and refraction lookups, scrolling normal-map layers.
    const float wvp[16] = {0.002f, 0, 0, 0, 0, 0.002f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 1};
    const float invView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float eyeCol[4] = {300, -400, 350, 1};  // c7 = the eye (row 3 of the inverse view)
    const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float reflection[16] = {0.0008f, 0, 0, 0.5f, 0, 0.0009f, 0, 0.45f, 0, 0, 0, 0, 0.0001f, 0.0002f, 0, 1.1f};
    const float refraction[16] = {-0.0007f, 0, 0, 0.55f, 0, 0.0006f, 0, 0.5f, 0, 0, 0, 0, -0.0002f, 0.0001f, 0, 1};
    const float tex1[12] = {0.002f, 0, 0, 0, 0, 0.002f, 0, 0, 0.1f, 0.2f, 1, 0};
    const float tex2[12] = {-0.003f, 0, 0, 0, 0, 0.003f, 0, 0, 0.3f, -0.1f, 1, 0};
    const float grid[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0.5f, 0.5f, 1, 0};
    // c2/c3: GraphicsUpgrade's additions at their neutral values (no shadows, the sky untinted)
    const float water[16] = {0.22995f, 0.238738f, 0.0954951f, 0.7f, 0.998001f, 0.847591f, 0.542458f, 0,
                             0, 0, 0.45f, 0, 1, 1, 1, 0};

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    CHECK(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    CHECK(dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    // Our additions, each against something it must equal exactly (any picture works, random normals included):
    //   2 shadow  half light everywhere (a 0x80 shadow texture), WaterShadow 1, darkness 100 (glint kept)
    //             = the plain picture x 0.502
    //   3 glint   the same with darkness 0.498 (the texel is 128/255 = 0.502 light): full shadow, so no glint
    //   4         = the same with darkness 100 and no sun at all
    //   5 sky     the sky light 0
    //   6         = the plain settings with a sky cube whose colours are black (its alpha, the glint's, kept)
    IDirect3DTexture9* half = nullptr;
    CHECK(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &half, nullptr));
    {
        D3DLOCKED_RECT lr;
        CHECK(half->LockRect(0, &lr, nullptr, 0));
        *static_cast<DWORD*>(lr.pBits) = 0xFF808080;
        half->UnlockRect(0);
    }
    IDirect3DCubeTexture9* darkSky = nullptr;
    CHECK(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &darkSky, nullptr));
    for (int face = 0; face < 6; ++face)
    {
        D3DLOCKED_RECT from, to;
        auto* sky = static_cast<IDirect3DCubeTexture9*>(tex[4]);
        CHECK(sky->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &from, nullptr, D3DLOCK_READONLY));
        CHECK(darkSky->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &to, nullptr, 0));
        for (UINT y = 0; y < 16; ++y)
            for (UINT x = 0; x < 16; ++x)
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(to.pBits) + y * to.Pitch)[x] =
                    reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(from.pBits) + y * from.Pitch)[x] & 0xFF000000;
        darkSky->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
        sky->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
    }
    const float shadowMatrix[16] = {0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0, 0, 1};  // every point -> (0.5, 0.5)
    const int kPictures = 7;
    std::vector<DWORD> image[kPictures];
    for (int which = 0; which < kPictures; ++which)
    {
        float pc[16];
        memcpy(pc, water, sizeof(pc));
        if (which >= 2 && which <= 4)
            pc[8] = 1, pc[9] = 1, pc[10] = which == 3 ? 0.498f : 100.0f;  // WaterShadow 1, on, darkness (0.498: full)
        if (which == 4)
            pc[4] = pc[5] = pc[6] = 0;  // no sun
        if (which == 5)
            pc[12] = pc[13] = pc[14] = 0;  // no sky light
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        CHECK(dev->SetRenderState(D3DRS_ZENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        CHECK(dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(vs[which ? 1 : 0]));
        CHECK(dev->SetPixelShader(ps[which ? 1 : 0]));
        CHECK(dev->SetVertexShaderConstantF(30, shadowMatrix, 4));
        CHECK(dev->SetTexture(7, which >= 2 && which <= 4 ? half : nullptr));
        CHECK(dev->SetVertexShaderConstantF(0, wvp, 4));
        CHECK(dev->SetVertexShaderConstantF(4, invView, 4));
        CHECK(dev->SetVertexShaderConstantF(7, eyeCol, 1));
        CHECK(dev->SetVertexShaderConstantF(8, world, 4));
        CHECK(dev->SetVertexShaderConstantF(12, reflection, 4));
        CHECK(dev->SetVertexShaderConstantF(16, refraction, 4));
        CHECK(dev->SetVertexShaderConstantF(20, tex1, 3));
        CHECK(dev->SetVertexShaderConstantF(23, tex2, 3));
        CHECK(dev->SetVertexShaderConstantF(26, grid, 3));
        CHECK(dev->SetPixelShaderConstantF(0, pc, 4));
        for (int st = 0; st < 7; ++st)
        {
            CHECK(dev->SetTexture(st, st == 4 && which == 6 ? darkSky : tex[st]));
            const DWORD address = st == 4 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSU, address));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSV, address));
            CHECK(dev->SetSamplerState(st, D3DSAMP_ADDRESSW, address));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
            CHECK(dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        }
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        for (UINT y = 0; y < bd.Height; ++y)
            for (UINT x = 0; x < bd.Width; ++x)
                image[which].push_back(reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x]);
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    int maxDiff = 0;
    size_t differing = 0;
    std::vector<DWORD> colours;
    for (size_t i = 0; i < image[0].size(); ++i)
    {
        int d = 0;
        for (int k = 0; k < 3; ++k)
            d = (std::max)(d, abs(static_cast<int>((image[0][i] >> (8 * k)) & 0xFF) -
                                  static_cast<int>((image[1][i] >> (8 * k)) & 0xFF)));
        maxDiff = (std::max)(maxDiff, d);
        differing += d > 0;
        if (colours.size() < 100000)
            colours.push_back(image[0][i] & 0xFFFFFF);
    }
    std::sort(colours.begin(), colours.end());
    size_t distinct = std::unique(colours.begin(), colours.end()) - colours.begin();
    printf("WATERCMP max=%d differing=%zu pixels=%zu colours=%zu\n", maxDiff, differing, image[0].size(), distinct);
    // The additions: the largest difference from what each must equal (the shadow where the plain picture isn't
    // clipped at 255), and how many channels each changes from the plain picture (so they did something).
    auto channel = [](DWORD c, int k) { return static_cast<int>((c >> (8 * k)) & 0xFF); };
    int shadowWorst = 0, glintWorst = 0, skyWorst = 0;
    size_t glintChanged = 0, skyChanged = 0;
    for (size_t i = 0; i < image[1].size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            int plain = channel(image[1][i], k);
            if (plain < 250)
                shadowWorst = (std::max)(shadowWorst, abs(channel(image[2][i], k) - (plain * 128 + 127) / 255));
            glintWorst = (std::max)(glintWorst, abs(channel(image[3][i], k) - channel(image[4][i], k)));
            skyWorst = (std::max)(skyWorst, abs(channel(image[5][i], k) - channel(image[6][i], k)));
            glintChanged += channel(image[3][i], k) != channel(image[2][i], k);
            skyChanged += channel(image[5][i], k) != plain;
        }
    printf("WATERADD shadow=%d glint=%d glintchanged=%zu sky=%d skychanged=%zu\n", shadowWorst, glintWorst, glintChanged,
           skyWorst, skyChanged);

    // DXT5nm normal maps (f4NormalFormat, c8): random gentle normals (slopes up to about 6 degrees) drawn from rgb,
    // the same normals stored DXT5nm-style (x in alpha, y in green, red and blue 0) with c8.x = 1, and a flat map.
    // The first two must match (but for z, which the Shader Model 2 build takes as 1), and differ from the flat one.
    IDirect3DTexture9* normalTex[3] = {};
    for (int k = 0; k < 3; ++k)
    {
        CHECK(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &normalTex[k], nullptr));
        D3DLOCKED_RECT lr;
        CHECK(normalTex[k]->LockRect(0, &lr, nullptr, 0));
        unsigned nseed = 777;
        for (UINT y = 0; y < 64; ++y)
            for (UINT x = 0; x < 64; ++x)
            {
                nseed = nseed * 1103515245u + 12345u;
                const float nx = (static_cast<int>((nseed >> 16) & 0xFF) - 128) / 1280.0f;  // -0.1 .. 0.1
                nseed = nseed * 1103515245u + 12345u;
                const float ny = (static_cast<int>((nseed >> 16) & 0xFF) - 128) / 1280.0f;
                const float nz = std::sqrt(1.0f - nx * nx - ny * ny);
                auto byte = [](float v) { return static_cast<DWORD>(std::lround((v * 0.5f + 0.5f) * 255)); };
                DWORD c = k == 0 ? 0xFF000000 | byte(nx) << 16 | byte(ny) << 8 | byte(nz)  // a r g b
                          : k == 1 ? byte(nx) << 24 | byte(ny) << 8                     // x in alpha, y in green
                                   : 0xFF8080FF;                                         // flat
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = c;
            }
        normalTex[k]->UnlockRect(0);
    }
    std::vector<DWORD> nmImage[3];
    for (int k = 0; k < 3; ++k)
    {
        const float format[4] = {k == 1 ? 1.0f : 0.0f, 0, 0, 0};
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetVertexShader(vs[1]));
        CHECK(dev->SetPixelShader(ps[1]));
        CHECK(dev->SetTexture(7, nullptr));
        CHECK(dev->SetPixelShaderConstantF(0, water, 4));
        CHECK(dev->SetPixelShaderConstantF(8, format, 1));
        for (int st = 0; st < 7; ++st)
            CHECK(dev->SetTexture(st, st == 0 ? normalTex[k] : tex[st]));
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        for (UINT y = 0; y < bd.Height; ++y)
            for (UINT x = 0; x < bd.Width; ++x)
                nmImage[k].push_back(reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x]);
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    const float noFormat[4] = {};
    CHECK(dev->SetPixelShaderConstantF(8, noFormat, 1));
    // The sky cube is random noise, so a hair's change in a normal can land on another texel: compare the average
    // difference (x 1000), DXT5nm against rgb, with that between rgb and the flat map.
    double nmDiff = 0, flatDiff = 0;
    for (size_t i = 0; i < nmImage[0].size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            nmDiff += abs(channel(nmImage[0][i], k) - channel(nmImage[1][i], k));
            flatDiff += abs(channel(nmImage[0][i], k) - channel(nmImage[2][i], k));
        }
    const double samples = 3.0 * nmImage[0].size();
    printf("WATERNM mean=%d flat=%d\n", static_cast<int>(1000 * nmDiff / samples), static_cast<int>(1000 * flatDiff / samples));
    for (IDirect3DTexture9* t : normalTex)
        t->Release();

    // The shoreline water (water.hlsl with SHORE, Shader Model 3), against our plain one (image[1]):
    //   0 every addition off (strengths 0, the glint not turned)        = the plain picture
    //   1 the glint turned a quarter round the vertical                  changes the glint
    //   2 caustics, the shore 2 cells from land and 8 units deep         change the picture
    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) && caps.VertexShaderVersion >= D3DVS_VERSION(3, 0))
    {
        std::vector<DWORD> vsShoreCode = Tokens(g_waterShoreVS, sizeof(g_waterShoreVS)),
                           psShoreCode = Tokens(g_waterShorePS, sizeof(g_waterShorePS));
        IDirect3DVertexShader9* vsShore = nullptr;
        IDirect3DPixelShader9* psShore = nullptr;
        CHECK(dev->CreateVertexShader(vsShoreCode.data(), &vsShore));
        CHECK(dev->CreatePixelShader(psShoreCode.data(), &psShore));
        auto texel = [&](DWORD argb) {
            IDirect3DTexture9* t = nullptr;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) &&
                SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)))
                *static_cast<DWORD*>(lr.pBits) = argb, t->UnlockRect(0);
            return t;
        };
        IDirect3DTexture9* shallow = texel(0xFF104000);  // r 16 = 2 cells, g 64 = 8 units deep, known
        IDirect3DTexture9* bright = texel(0xFFFFFFFF);   // caustics at full
        if (!shallow || !bright)
        {
            printf("FAILED: shore test textures\n");
            return 1;
        }
        std::vector<DWORD> shoreImage[3];
        for (int which = 0; which < 3; ++which)
        {
            const float strengths[4] = {which == 2 ? 1.0f : 0.0f, 0.0f, 7.5f, which == 1 ? 1.0f : 0.0f};
            const float quarter[12] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0};
            const float identity3[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            const float shoreGrid[4] = {0.001f, 0.5f, 0.01f, 0.002f}, scroll[4] = {};
            CHECK(dev->BeginScene());
            CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
            CHECK(dev->SetVertexDeclaration(decl));
            CHECK(dev->SetVertexShader(vsShore));
            CHECK(dev->SetPixelShader(psShore));
            CHECK(dev->SetTexture(7, nullptr));
            CHECK(dev->SetVertexShaderConstantF(0, wvp, 4));
            CHECK(dev->SetVertexShaderConstantF(4, invView, 4));
            CHECK(dev->SetVertexShaderConstantF(7, eyeCol, 1));
            CHECK(dev->SetVertexShaderConstantF(8, world, 4));
            CHECK(dev->SetVertexShaderConstantF(12, reflection, 4));
            CHECK(dev->SetVertexShaderConstantF(16, refraction, 4));
            CHECK(dev->SetVertexShaderConstantF(20, tex1, 3));
            CHECK(dev->SetVertexShaderConstantF(23, tex2, 3));
            CHECK(dev->SetVertexShaderConstantF(26, grid, 3));
            CHECK(dev->SetVertexShaderConstantF(34, shoreGrid, 1));
            CHECK(dev->SetVertexShaderConstantF(35, scroll, 1));
            CHECK(dev->SetPixelShaderConstantF(0, water, 4));
            CHECK(dev->SetPixelShaderConstantF(4, strengths, 1));
            CHECK(dev->SetPixelShaderConstantF(5, which == 1 ? quarter : identity3, 3));
            for (int st = 0; st < 7; ++st)
                CHECK(dev->SetTexture(st, tex[st]));
            CHECK(dev->SetTexture(8, shallow));
            for (int st = 9; st < 11; ++st)
                CHECK(dev->SetTexture(st, bright));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V)));
            CHECK(dev->EndScene());
            CHECK(dev->GetRenderTargetData(bb, sys));
            D3DLOCKED_RECT lr;
            CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
            for (UINT y = 0; y < bd.Height; ++y)
                for (UINT x = 0; x < bd.Width; ++x)
                    shoreImage[which].push_back(
                        reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x]);
            sys->UnlockRect();
            CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
        }
        int neutralWorst = 0;
        size_t turned = 0, caustics = 0;
        for (size_t i = 0; i < image[1].size(); ++i)
            for (int k = 0; k < 3; ++k)
            {
                const int plain = channel(image[1][i], k), off = channel(shoreImage[0][i], k);
                neutralWorst = (std::max)(neutralWorst, abs(off - plain));
                turned += channel(shoreImage[1][i], k) != off;
                caustics += channel(shoreImage[2][i], k) != off;
            }
        printf("WATERSHORE neutral=%d turned=%zu caustics=%zu\n", neutralWorst, turned, caustics);
        for (int st = 7; st < 11; ++st)
            CHECK(dev->SetTexture(st, nullptr));
        shallow->Release();
        bright->Release();
        vsShore->Release();
        psShore->Release();
    }
    else
        printf("WATERSHORE skipped: no Shader Model 3\n");
    CHECK(dev->SetTexture(7, nullptr));
    half->Release();
    darkSky->Release();
    for (int st = 0; st < 7; ++st)
        tex[st]->Release();
    for (int k = 0; k < 2; ++k)
        vs[k]->Release(), ps[k]->Release();
    decl->Release();
    sys->Release();
    bb->Release();
    return 0;
}

// test_proxy terrainpatch: TerrainPatch::Apply on a fake exe image holding BtS's bytes at the patch sites. Prints
// "TERRAINPATCH name=0/1 ..." for run_tests.py.
static int TerrainPatchTest()
{
    const size_t size = 0xbb000;
    std::vector<uint8_t> stock(size, 0xcc);
    for (const auto& site : TerrainPatch::Sites(33))
        memcpy(stock.data() + site.rva, site.original.data(), site.original.size());
    auto run = [&](std::vector<uint8_t>& image, int verts, int& writes) {
        writes = 0;
        return TerrainPatch::Apply(image.data(), image.size(), verts, [&](uint32_t rva, const uint8_t* b, size_t n) {
            memcpy(image.data() + rva, b, n);
            ++writes;
        });
    };
    int writes = 0;
    // 33: all seven sites rewritten, nothing else touched
    std::vector<uint8_t> img = stock;
    std::string why = run(img, 33, writes);
    const uint8_t want[][6] = {{0xba, 0x21, 0, 0, 0}, {0xc1, 0xe9, 5}, {0xc1, 0xe8, 5}, {0xc1, 0xe0, 5},
                               {0xb8, 0x20, 0, 0, 0}, {0xc1, 0xe0, 5}, {0xb8, 0x20, 0, 0, 0}};
    const uint32_t rvas[] = {0xba862, 0xba884, 0xba887, 0x78db3, 0x78dc4, 0x78ef3, 0x78f04};
    const size_t lens[] = {5, 3, 3, 3, 5, 3, 5};
    bool bytes33 = why.empty() && writes == 7;
    size_t changed = 0;
    for (size_t i = 0; i < size; ++i)
        changed += img[i] != stock[i];
    for (int k = 0; k < 7; ++k)
        bytes33 = bytes33 && memcmp(img.data() + rvas[k], want[k], lens[k]) == 0;
    // the same again: already patched, nothing written, success
    std::string again = run(img, 33, writes);
    const bool idempotent = again.empty() && writes == 0;
    // 65: shift 6, sizes 65 and 64
    std::vector<uint8_t> img65 = stock;
    run(img65, 65, writes);
    const bool v65 = writes == 7 && img65[0xba863] == 65 && img65[0xba886] == 6 && img65[0x78dc5] == 64;
    // one byte different (another build): refused, nothing written
    std::vector<uint8_t> other = stock;
    other[0x78f05] = 0x11;
    std::string refusedWhy = run(other, 33, writes);
    const bool refused = !refusedWhy.empty() && writes == 0 && other == [&] {
        std::vector<uint8_t> o = stock;
        o[0x78f05] = 0x11;
        return o;
    }();
    // an image too small for the sites: refused
    std::vector<uint8_t> tiny(0x1000, 0);  // ("small" is a Windows header macro)
    const bool tooSmall = !run(tiny, 33, writes).empty() && writes == 0;
    // unsupported values refused; 17 does nothing
    std::vector<uint8_t> img20 = stock, img17 = stock;
    const bool bad = !run(img20, 20, writes).empty() && writes == 0 && img20 == stock;
    const bool stock17 = run(img17, 17, writes).empty() && writes == 0 && img17 == stock;
    // The landscape's cell values as the patch gives them (the XML's iGameCellSize unused; plots per cell at most so
    // many that a cell stays within 129 samples across)
    auto cellsAre = [](int verts, int xmlPlots, int samples, int plots, int vertCnt) {
        const TerrainPatch::CellValues c = TerrainPatch::Cells(verts, xmlPlots);
        return c.samples == samples && c.plots == plots && c.vertCnt == vertCnt;
    };
    const bool cells = cellsAre(33, 4, 33, 4, 129) && cellsAre(65, 4, 65, 2, 129) && cellsAre(65, 2, 65, 2, 129) &&
                       cellsAre(17, 4, 17, 4, 65) && cellsAre(65, 1, 65, 1, 65) && cellsAre(33, 8, 33, 4, 129) &&
                       TerrainPatch::CellImports().size() == 6;
    // The heightmap folder: the nine PUSHes of the tile loader's path strings pointed at ours (heightmap64 at 65); the
    // scale masks' format prints the new prefix and skips the old one's argument.
    std::vector<uint8_t> hmImg = stock;
    const auto prefixes = TerrainPatch::TilePrefixes();
    for (const auto& p : prefixes)
    {
        const uint8_t push[5] = {0x68, uint8_t(p.stringVa), uint8_t(p.stringVa >> 8), uint8_t(p.stringVa >> 16),
                                 uint8_t(p.stringVa >> 24)};
        memcpy(hmImg.data() + p.pushRva, push, 5);
    }
    std::vector<std::string> hmStrings;
    std::vector<uint32_t> hmAddrs;
    for (const auto& p : prefixes)
        hmStrings.push_back(TerrainPatch::TileString(p, 65));
    for (size_t k = 0; k < prefixes.size(); ++k)
        hmAddrs.push_back(0x1000000 + static_cast<uint32_t>(k) * 0x100);
    int hmWrites = 0;
    const std::string hmWhy = TerrainPatch::ApplySites(hmImg.data(), hmImg.size(),
                                                       TerrainPatch::TileFolderSites(0x400000, prefixes, hmAddrs),
                                                       [&](uint32_t rva, const uint8_t* b, size_t n) {
        memcpy(hmImg.data() + rva, b, n);
        ++hmWrites;
    });
    bool heightmapFolder = hmWhy.empty() && hmWrites == 9 &&
                           hmStrings[1] == "art/Terrain/heightmap64/flats/flat1_" &&
                           hmStrings[4] == "art/Terrain/heightmap64/Coasts/CoastTile1_0" &&
                           hmStrings[8] == "%.0sart/Terrain/heightmap64/CoastBlendMasks/CoastScaleMask%02d.tga" &&
                           TerrainPatch::InTileFolder("art/Terrain/heightmap/peaks/Peak_", 33) ==
                               "art/Terrain/heightmap32/peaks/Peak_";
    char formatted[128];
    snprintf(formatted, sizeof(formatted), hmStrings[8].c_str(), "art/Terrain/heightmap/CoastBlendMasks/CoastScaleMask", 7);
    heightmapFolder = heightmapFolder &&
                      std::string(formatted) == "art/Terrain/heightmap64/CoastBlendMasks/CoastScaleMask07.tga";
    for (size_t k = 0; k < prefixes.size(); ++k)
    {
        uint32_t imm;
        memcpy(&imm, hmImg.data() + prefixes[k].pushRva + 1, 4);
        heightmapFolder = heightmapFolder && hmImg[prefixes[k].pushRva] == 0x68 && imm == hmAddrs[k];
    }
    // Hills' and peaks' coast scale masks: the two calls to the picker sent to a thunk; a mask resampled to the tiles.
    const uint32_t base = 0x400000, thunk = 0x12345678;
    std::vector<uint8_t> maskImg = stock;
    for (uint32_t rva : TerrainPatch::kMaskCalls)
    {
        const auto call = TerrainPatch::CallTo(base + rva, base + TerrainPatch::kMaskPicker);
        memcpy(maskImg.data() + rva, call.data(), call.size());
    }
    const bool maskCalls = maskImg[0x78d24] == 0xe8 && maskImg[0x78d24 + 1] == 0x27 && maskImg[0x78e39 + 1] == 0x12;
    int maskWrites = 0;
    const std::string maskWhy = TerrainPatch::ApplySites(maskImg.data(), maskImg.size(),
                                                         TerrainPatch::MaskSites(base, thunk),
                                                         [&](uint32_t rva, const uint8_t* b, size_t n) {
        memcpy(maskImg.data() + rva, b, n);
        ++maskWrites;
    });
    bool maskSites = maskCalls && maskWhy.empty() && maskWrites == 2;
    for (uint32_t rva : TerrainPatch::kMaskCalls)
    {
        int32_t rel;
        memcpy(&rel, maskImg.data() + rva + 1, 4);
        maskSites = maskSites && maskImg[rva] == 0xe8 && base + rva + 5 + rel == thunk;
    }
    // A flat mask stays flat; a ramp 0, 17 ... 255 (as the stock masks' 16 steps) stays a ramp, evenly spaced and the
    // same either way round (out[i] + out[63 - i] = 255), within the ends.
    uint8_t flat[16 * 16], ramp[16 * 16], out[64 * 64];
    for (int i = 0; i < 256; ++i)
        flat[i] = 200, ramp[i] = static_cast<uint8_t>(17 * (i % 16));
    TerrainPatch::ResampleMask(flat, 16, 16, 16, 64, out);
    bool maskFlat = true;
    for (uint8_t v : out)
        maskFlat = maskFlat && v == 200;
    TerrainPatch::ResampleMask(ramp, 16, 16, 16, 64, out);
    bool maskRamp = out[0] > 0 && out[0] < 17 && out[63] > 238 && out[63] < 255;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            maskRamp = maskRamp && out[y * 64 + x] == out[x] && (x == 0 || out[x] > out[x - 1]) &&
                       std::abs(out[x] + out[63 - x] - 255) <= 1;
    printf("TERRAINPATCH bytes33=%d changed=%zu idempotent=%d v65=%d refused=%d toosmall=%d bad=%d stock17=%d cells=%d "
           "masksites=%d maskflat=%d maskramp=%d hmfolder=%d\n",
           bytes33, changed, idempotent, v65, refused, tooSmall, bad, stock17, cells, maskSites, maskFlat, maskRamp,
           heightmapFolder);
    printf("TERRAINPATCH refusal: %s\n", refusedWhy.c_str());
    return 0;
}

// test_proxy terraincells: the terrain grid's partial updates (terrain_cells.h) against doing everything. Prints
// "TERRAINCELLS pieces=0/1 shore=0/1 shoreall=0/1 steps=N seam=N".
static int TerrainCellsTest()
{
    using namespace TerrainCells;
    unsigned seed = 12345;
    auto rnd = [&](int n) {
        seed = seed * 1103515245u + 12345u;
        return static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
    };
    // Every texel a rectangle covers (wrapped, at most one grid across) written once, at its own place.
    const int g = 64, gm = g - 1;
    bool pieces = true;
    for (int t = 0; t < 400 && pieces; ++t)
    {
        Rect r;
        const int x0 = rnd(300) - 150, y0 = rnd(300) - 150;
        r.Add(x0, y0, x0 + rnd(100), y0 + rnd(100));
        std::vector<int> hits(g * g, 0);
        ForEachPiece(r, g, [&](int gx0, int gy0, int gx1, int gy1, int tx, int ty) {
            pieces = pieces && tx + gx1 - gx0 < g && ty + gy1 - gy0 < g;
            for (int y = gy0; y <= gy1; ++y)
                for (int x = gx0; x <= gx1; ++x)
                {
                    pieces = pieces && (x & gm) == tx + x - gx0 && (y & gm) == ty + y - gy0;
                    ++hits[(y & gm) * g + (x & gm)];
                }
        });
        const Rect c = ClampToGrid(r, g);
        for (int ty = 0; ty < g; ++ty)
            for (int tx = 0; tx < g; ++tx)
            {
                const bool covered = ((tx - c.x0) & gm) <= c.x1 - c.x0 && ((ty - c.y0) & gm) <= c.y1 - c.y0;
                pieces = pieces && hits[ty * g + tx] == (covered ? 1 : 0);
            }
    }
    // The shore, updated around each new chunk of terrain (land and sea, some across the grid's seam), against
    // working out everything read and 32 cells round it, as it used to be.
    const int n = 256, nm = n - 1;
    std::vector<float> heights(n * n, std::numeric_limits<float>::quiet_NaN());
    auto height = [&](int x, int y) { return heights[(y & nm) * n + (x & nm)]; };
    std::vector<uint32_t> tex(n * n, 0);
    Rect read;
    float water = 49.5f;
    auto incremental = [&](const Rect& changed) {
        Rect write, window;
        ShoreRects(changed, n, write, window);
        const std::vector<float> dist = ShoreDistances(window, water, height);
        const int w = window.x1 - window.x0 + 1;
        ForEachPiece(write, n, [&](int gx0, int gy0, int gx1, int gy1, int, int) {
            for (int y = gy0; y <= gy1; ++y)
                for (int x = gx0; x <= gx1; ++x)
                    tex[(y & nm) * n + (x & nm)] =
                        ShoreTexel(dist[static_cast<size_t>(y - window.y0) * w + (x - window.x0)], height(x, y), water);
        });
    };
    auto reference = [&]() {
        std::vector<uint32_t> ref(n * n, 0);
        const Rect win = {read.x0 - kShoreKeep, read.y0 - kShoreKeep, read.x1 + kShoreKeep, read.y1 + kShoreKeep};
        const std::vector<float> dist = ShoreDistances(win, water, height);
        const int w = win.x1 - win.x0 + 1;
        for (int y = win.y0; y <= win.y1; ++y)
            for (int x = win.x0; x <= win.x1; ++x)
                if (!std::isnan(height(x, y)))
                    ref[(y & nm) * n + (x & nm)] =
                        ShoreTexel(dist[static_cast<size_t>(y - win.y0) * w + (x - win.x0)], height(x, y), water);
        return ref;
    };
    bool shore = true;
    int steps = 0, seam = 0;
    for (; steps < 16; ++steps)
    {
        Rect c;
        const int x0 = rnd(140) - 40, y0 = rnd(140) - 40;
        c.Add(x0, y0, x0 + 2 + rnd(18), y0 + 2 + rnd(18));
        seam += c.x0 < 0 || c.y0 < 0;
        for (int y = c.y0; y <= c.y1; ++y)
            for (int x = c.x0; x <= c.x1; ++x)
                heights[(y & nm) * n + (x & nm)] = water + static_cast<float>(rnd(41) - 20);
        read.Add(c);
        incremental(c);
        shore = shore && tex == reference();
    }
    water = 55.0f;  // the water rising: everything read worked out again
    incremental(read);
    const bool shoreAll = tex == reference();
    printf("TERRAINCELLS pieces=%d shore=%d shoreall=%d steps=%d seam=%d\n", pieces, shore, shoreAll, steps, seam);
    return 0;
}

// test_proxy tilesets: terrain tile sets' logic and patch sites (terrain_sets.h). Prints "TILESETS name=0/1 ...".
static int TileSetsTest()
{
    using namespace TileSets;
    auto range = [](int count, int sets, int set, int lo, int size) {
        const Range r = SetRange(count, sets, set);
        return r.lo == lo && r.size == size;
    };
    const bool ranges = range(8, 2, 0, 0, 4) && range(8, 2, 1, 4, 4) && range(7, 2, 1, 3, 3) && range(1, 2, 1, 0, 1) &&
                        range(8, 1, 0, 0, 8) && range(6, 3, 2, 4, 2) && range(2, 3, 1, 0, 2);
    const int v1[4] = {1, 1, 0, -1}, v2[4] = {0, 1, -1, -1}, v3[4] = {-1, -1, -1, -1}, v4[4] = {1, 1, 0, 0},
              v5[4] = {2, 2, 1, -1}, v6[4] = {-1, 1, -1, -1};
    const bool majority = Majority(v1, 2) == 1 && Majority(v2, 2) == 0 && Majority(v3, 2) == 0 &&
                          Majority(v4, 2) == 0 && Majority(v5, 3) == 2 && Majority(v6, 2) == 1;
    int c[4][2];
    Corners(5, 3, 10, c);
    const bool corners = c[0][0] == 4 && c[0][1] == 7 && c[1][0] == 5 && c[1][1] == 7 && c[2][0] == 5 &&
                         c[2][1] == 6 && c[3][0] == 4 && c[3][1] == 6;
    const bool seed = Seed(1, 1) == 0x1C81u + 0xB5Du && Seed(3, 0) == 3u * 0x1C81u;
    // The sites on a fake image: rewritten with JMP / CALL to the thunks only when every one holds BtS's bytes.
    const uint32_t base = 0x400000, picker = 0x10001000, hill = 0x10002000, peak = 0x10003000, atlas = 0x10004000,
                   wave = 0x10005000;
    const auto sites = Sites(base, picker, hill, peak, atlas, wave);
    std::vector<uint8_t> stock(0x220000, 0xcc);
    for (const auto& s : sites)
        memcpy(stock.data() + s.rva, s.original.data(), s.original.size());
    auto rel = [&](const std::vector<uint8_t>& img, uint32_t at) {
        int32_t r;
        memcpy(&r, img.data() + at, 4);
        return r;
    };
    std::vector<uint8_t> img = stock;
    int writes = 0;
    auto write = [&](std::vector<uint8_t>& im) {
        return [&](uint32_t rva, const uint8_t* b, size_t n) { memcpy(im.data() + rva, b, n), ++writes; };
    };
    const std::string why = TerrainPatch::ApplySites(img.data(), img.size(), sites, write(img));
    // the coast index's two calls: still to FUN_004788e0 before (the original bytes), to the thunks after
    const bool callsBefore = stock[kAtlasCall] == 0xE8 &&
                             rel(stock, kAtlasCall + 1) == static_cast<int32_t>(base + kCoastClassFn - (base + kAtlasCall + 5)) &&
                             rel(stock, kWaveCall + 1) == static_cast<int32_t>(base + kCoastClassFn - (base + kWaveCall + 5));
    bool patched = why.empty() && writes == 5 && callsBefore && img[kPicker] == 0xE9 &&
                   rel(img, kAtlasCall + 1) == static_cast<int32_t>(atlas - (base + kAtlasCall + 5)) &&
                   rel(img, kWaveCall + 1) == static_cast<int32_t>(wave - (base + kWaveCall + 5)) &&
                   rel(img, kPicker + 1) == static_cast<int32_t>(picker - (base + kPicker + 5)) && img[kHills] == 0xE8 &&
                   rel(img, kHills + 1) == static_cast<int32_t>(hill - (base + kHills + 5)) && img[kPeaks] == 0x56 &&
                   img[kPeaks + 1] == 0xE8 && rel(img, kPeaks + 2) == static_cast<int32_t>(peak - (base + kPeaks + 6));
    for (int k = 0; k < 5; ++k)
        patched = patched && img[kHills + 5 + k] == 0x90 && img[kPeaks + 6 + k] == 0x90;
    std::vector<uint8_t> other = stock;
    other[kPeaks + 3] = 0x75;  // another list address: another build
    writes = 0;
    const std::string refusal = TerrainPatch::ApplySites(other.data(), other.size(), sites, write(other));
    std::vector<uint8_t> otherStock = stock;
    otherStock[kPeaks + 3] = 0x75;
    const bool refused = !refusal.empty() && writes == 0 && other == otherStock;
    // coast variants folded into the art: stock 8, 8, 6, 7; a class's art never past the atlas's 8, 8, 8, 7 cells
    const int stockArt[4] = {8, 8, 6, 7}, bigArt[4] = {20, 20, 20, 20};
    const bool cycle = CycleIndex(5, 1, stockArt) == 5 && CycleIndex(9, 2, stockArt) == 1 &&   // one corner: 9 % 8
                       CycleIndex(6, 5, stockArt) == 0 && CycleIndex(7, 10, stockArt) == 1 &&  // diagonal: % 6
                       CycleIndex(7, 7, stockArt) == 0 && CycleIndex(13, 14, stockArt) == 6 && // three: % 7
                       CycleIndex(9, 9, stockArt) == 1 && CycleIndex(3, 15, stockArt) == 3 &&  // 15: not a coast
                       CycleIndex(10, 5, bigArt) == 2 && CycleIndex(10, 11, bigArt) == 3;      // atlas caps 8 and 7
    // LandBlend: parsing (14 masks of tile,rotation entries), and the three calls to the blend pick sent to a thunk
    LandBlend lb;
    std::string lwhy;
    const bool parsed = ParseLandBlend("terrain_grass: 8,0; 1,0; 2,0 17,90; 6,0; 13,0; 10,0; 3,0; 5,0; 9,0; 7,0; 4,0; 12,0; "
                                       "11,0; 14,0", lb, lwhy) &&
                        lb.terrain == "TERRAIN_GRASS" && lb.masks[1].size() == 1 && lb.masks[1][0] == std::make_pair(8, 0) &&
                        lb.masks[3].size() == 2 && lb.masks[3][1] == std::make_pair(17, 90) && lb.masks[14][0].first == 14;
    // the short form: names, any separators, any case; each gets the standard layout, the same as the old full lines
    const auto names = LandBlendNames(" terrain_grass, TERRAIN_SNOW;TERRAIN_PEAK  ");
    LandBlend full;
    std::string fwhy;
    ParseLandBlend("TERRAIN_GRASS: 8,0; 1,0; 2,0; 6,0; 13,0; 10,0; 3,0; 5,0; 9,0; 7,0; 4,0; 12,0; 11,0; 14,0", full, fwhy);
    const LandBlend standard = StandardLandBlend("terrain_grass");
    bool shortForm = names.size() == 3 && names[0] == "TERRAIN_GRASS" && names[2] == "TERRAIN_PEAK" &&
                     standard.terrain == "TERRAIN_GRASS" && LandBlendNames("").empty();
    for (int m = 1; m <= 14; ++m)
        shortForm = shortForm && standard.masks[m] == full.masks[m];
    std::string w1, w2, w3, w4;
    const bool badOnes = !ParseLandBlend("TERRAIN_GRASS: 8,0; 1,0", lb, w1) &&
                         !ParseLandBlend("TERRAIN_GRASS: 8,45; 1,0; 2,0; 6,0; 13,0; 10,0; 3,0; 5,0; 9,0; 7,0; 4,0; 12,0; "
                                         "11,0; 14,0", lb, w2) &&
                         !ParseLandBlend("8,0; 1,0", lb, w3) &&
                         !ParseLandBlend("TERRAIN_GRASS: 8,0;; 2,0; 6,0; 13,0; 10,0; 3,0; 5,0; 9,0; 7,0; 4,0; 12,0; 11,0; "
                                         "14,0; 15,0", lb, w4);
    std::vector<uint8_t> limg(0x7a000, 0xcc);
    const uint32_t lbase = 0x400000, lthunk = 0x12345678;
    for (uint32_t rva : kBlendPickCalls)
    {
        const auto call = Rel32(0xE8, lbase + rva, lbase + kBlendPick);
        memcpy(limg.data() + rva, call.data(), call.size());
    }
    int lwrites = 0;
    const std::string lres = TerrainPatch::ApplySites(limg.data(), limg.size(), LandBlendSites(lbase, lthunk),
                                                      [&](uint32_t rva, const uint8_t* b, size_t n) {
        memcpy(limg.data() + rva, b, n);
        ++lwrites;
    });
    bool landSites = lres.empty() && lwrites == 3;
    for (uint32_t rva : kBlendPickCalls)
    {
        int32_t rel;
        memcpy(&rel, limg.data() + rva + 1, 4);
        landSites = landSites && limg[rva] == 0xE8 && lbase + rva + 5 + rel == lthunk;
    }
    // A square's layers, as the engine builds them (lowest LayerOrder first). Terrains: 0 sand (water), 13 Rugged,
    // 14 Rock, 9 another land terrain without a land list.
    using A = BlendAction;
    auto run = [](const BlendCorners& c, std::vector<std::tuple<int, int, bool>> layers, std::vector<A> want,
                  int wantFull = -1) {
        BlendSquare sq;
        int full = -1;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            int f = -1;
            if (sq.Next(5, 7, c, std::get<0>(layers[i]), std::get<1>(layers[i]), std::get<2>(layers[i]), f) != want[i])
                return false;
            if (f >= 0)
                full = f;
        }
        return wantFull < 0 || full == wantFull;
    };
    const BlendCorners allLand = {{14, 14, 13, 13}, {false, false, false, false}};
    const BlendCorners oneLand = {{14, 14, 0, 0}, {false, false, true, true}};
    const BlendCorners mixed = {{14, 13, 0, 0}, {false, false, true, true}};       // TL Rock, TR Rugged: land mask 3
    const BlendCorners mixedNo = {{9, 14, 0, 0}, {false, false, true, true}};      // the lowest land has no list
    const BlendCorners landBase = {{14, 13, 0, -1}, {false, false, true, true}};   // BL off the map
    const bool squares =
        run(allLand, {{13, 15, true}, {14, 3, true}}, {A::Engine, A::Land}) &&           // all land: land entries
        run(allLand, {{13, 15, true}, {14, 3, false}}, {A::Engine, A::Engine}) &&        // no list: the engine's
        run(oneLand, {{0, 15, false}, {14, 3, true}}, {A::Engine, A::Engine}) &&         // one land terrain: paired
        run(mixed, {{0, 15, false}, {13, 2, true}, {14, 1, true}},
            {A::Engine, A::FullMask, A::Land}, 3) &&                                     // mixed: full mask 3, then land
        run(mixedNo, {{0, 15, false}, {9, 1, false}, {14, 2, true}},
            {A::Engine, A::Engine, A::Engine}) &&                                        // lowest has no list: untouched
        run(landBase, {{13, 15, true}, {0, 12, false}, {14, 1, true}},
            {A::Engine, A::Engine, A::Engine});                                          // a land terrain drawn whole
    // overlays (40 = peaks: at no corner): on all land the land entry; on a coast, covering part of the land, the
    // full land mask (here 3); covering all of it, the engine's (paired) pick; without a list, the engine's
    const BlendCorners rockCoast = {{14, 14, 0, 0}, {false, false, true, true}};
    const bool overlays =
        run(allLand, {{13, 15, true}, {14, 3, true}, {40, 1, true}}, {A::Engine, A::Land, A::Land}) &&
        run(rockCoast, {{0, 15, false}, {14, 3, true}, {40, 1, true}}, {A::Engine, A::Engine, A::FullMask}, 3) &&
        run(rockCoast, {{0, 15, false}, {14, 3, true}, {40, 3, true}}, {A::Engine, A::Engine, A::Engine}) &&
        run(rockCoast, {{0, 15, false}, {14, 3, true}, {40, 1, false}}, {A::Engine, A::Engine, A::Engine}) &&
        run(mixed, {{0, 15, false}, {13, 2, true}, {14, 1, true}, {40, 2, true}},
            {A::Engine, A::FullMask, A::Land, A::FullMask}, 3);
    // a new square starts afresh: the same sequence twice at different squares
    BlendSquare sq2;
    int f = -1;
    bool fresh = sq2.Next(1, 1, mixed, 13, 2, true, f) == A::FullMask && sq2.Next(1, 1, mixed, 14, 1, true, f) == A::Land &&
                 sq2.Next(2, 1, mixed, 13, 2, true, f) == A::FullMask;

    // [terrain tiles] MixedCoastBlend (blendAll): every higher paired land layer of a mixed coast square takes the full
    // land mask too, and the square is marked blended; nothing else changes.
    auto runAll = [](const BlendCorners& c, std::vector<std::tuple<int, int, bool>> layers, std::vector<A> want,
                     bool wantBlended, int wantFull = -1) {
        BlendSquare sq;
        int full = -1;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            int f = -1;
            if (sq.Next(5, 7, c, std::get<0>(layers[i]), std::get<1>(layers[i]), std::get<2>(layers[i]), f, true) !=
                want[i])
                return false;
            if (f >= 0)
                full = f;
        }
        return sq.blended == wantBlended && (wantFull < 0 || full == wantFull);
    };
    const bool blendPicks =
        runAll(mixed, {{0, 15, false}, {13, 2, true}, {14, 1, true}}, {A::Engine, A::FullMask, A::FullMask}, true, 3) &&
        runAll(mixed, {{0, 15, false}, {13, 2, true}, {9, 1, false}}, {A::Engine, A::FullMask, A::Engine}, true, 3) &&
        runAll(mixedNo, {{0, 15, false}, {9, 1, false}, {14, 2, true}}, {A::Engine, A::Engine, A::Engine}, false) &&
        runAll(landBase, {{13, 15, true}, {0, 12, false}, {14, 1, true}}, {A::Engine, A::Engine, A::Engine}, false) &&
        runAll(oneLand, {{0, 15, false}, {14, 3, true}}, {A::Engine, A::Engine}, false) &&
        runAll(allLand, {{13, 15, true}, {14, 3, true}}, {A::Engine, A::Land}, false) &&
        runAll(mixed, {{0, 15, false}, {13, 2, true}, {14, 1, true}, {40, 2, true}},
               {A::Engine, A::FullMask, A::FullMask, A::FullMask}, true, 3);
    // The shader's weight (terrain_shadow.hlsl, MixWeight), as written there: corners SW, SE, NW, NE; terrain + 1, water,
    // rank (0: not paired). Drawn one over another in rank order, each terrain should end up at its share of the paired
    // land corners, and two squares should agree along their shared edge.
    struct Corner
    {
        int terrain;
        bool water;
        int rank;
    };
    auto weight = [](const Corner c[4], int layer, int layerRank, float fx, float fy) {
        const float bw[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        float own = 0, lower = 0;
        bool anyOwn = false, anyLower = false;
        for (int k = 0; k < 4; ++k)
        {
            const bool land = !c[k].water;
            const bool isOwn = land && c[k].terrain == layer;
            const bool isLower = land && c[k].rank > 0 && c[k].rank < layerRank;
            anyOwn = anyOwn || isOwn;
            anyLower = anyLower || isLower;
            own += isOwn ? bw[k] : 0;
            lower += isLower ? bw[k] : 0;
        }
        return !anyOwn || !anyLower ? 1.0f : own / (std::max)(own + lower, 1e-5f);
    };
    // Layers Rock (15, rank 1), Rugged (14, rank 2), Mesa-as-terrain (41, rank 3), drawn as "over" with alpha = weight:
    // a terrain's part of the result.
    auto shares = [&](const Corner c[4], float fx, float fy, float out[3]) {
        const int layers[3] = {15, 14, 41};
        float rest = 1;  // what the layers drawn later leave of the ones before
        for (int k = 2; k >= 0; --k)
        {
            bool present = false;
            for (int i = 0; i < 4; ++i)
                present = present || (!c[i].water && c[i].terrain == layers[k]);
            const float a = present ? weight(c, layers[k], k + 1, fx, fy) : 0.0f;
            out[k] = rest * a;
            rest *= 1 - a;
        }
    };
    const Corner sqA[4] = {{15, false, 1}, {14, false, 2}, {1, true, 0}, {41, false, 3}};  // SW Rock, SE Rugged, NW water
    const Corner sqB[4] = {{14, false, 2}, {15, false, 1}, {41, false, 3}, {1, true, 0}};   // east of A: shares A's SE, NE
    bool partition = true, edges = true;
    for (int i = 0; i <= 8; ++i)
        for (int j = 0; j <= 8; ++j)
        {
            const float fx = i / 8.0f, fy = j / 8.0f;
            const float bw[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            const float land = bw[0] + bw[1] + bw[3];
            float s[3];
            shares(sqA, fx, fy, s);
            if (land > 0.05f)
                partition = partition && std::fabs(s[0] - bw[0] / land) < 1e-4f && std::fabs(s[1] - bw[1] / land) < 1e-4f &&
                            std::fabs(s[2] - bw[3] / land) < 1e-4f;
        }
    for (int j = 0; j <= 8; ++j)  // A's east edge (fx = 1) is B's west edge (fx = 0)
    {
        float a[3], b2[3];
        shares(sqA, 1, j / 8.0f, a);
        shares(sqB, 0, j / 8.0f, b2);
        for (int k = 0; k < 3; ++k)
            edges = edges && std::fabs(a[k] - b2[k]) < 1e-4f;
    }
    const Corner single[4] = {{15, false, 1}, {15, false, 1}, {1, true, 0}, {1, true, 0}};  // one land terrain: whole
    const bool whole = weight(single, 15, 1, 0.3f, 0.6f) == 1.0f && weight(sqA, 41, 3, 0, 0) == 0.0f &&
                       weight(sqA, 15, 1, 0.9f, 0.9f) == 1.0f && weight(sqA, 99, 4, 0.5f, 0.5f) == 1.0f;
    const bool mixBlend = blendPicks && partition && edges && whole;
    printf("TILESETS mixblend=%d (picks %d, partition %d, edges %d, whole %d)\n", mixBlend, blendPicks, partition, edges,
           whole);
    printf("TILESETS ranges=%d majority=%d corners=%d seed=%d patched=%d refused=%d cycle=%d landparse=%d landbad=%d "
           "landsites=%d mixed=%d overlays=%d landshort=%d\n", ranges, majority, corners, seed, patched, refused, cycle,
           parsed, badOnes, landSites, squares && fresh, overlays, shortForm);
    printf("TILESETS land refusals: %s | %s | %s | %s\n", w1.c_str(), w2.c_str(), w3.c_str(), w4.c_str());
    printf("TILESETS refusal: %s\n", refusal.c_str());
    return 0;
}

// test_proxy hillfield: the hill field (hill_field.h) with its made-up stamps on made-up maps. Prints
// "HILLFIELD name=value ..." for run_tests.py.
static int HillFieldTest()
{
    using namespace HillField;
    auto makeMap = [](int w, int h, bool wrapX, const std::vector<std::array<int, 2>>& hills) {
        Map m;
        m.width = w;
        m.height = h;
        m.wrapX = wrapX;
        m.hills.assign(static_cast<size_t>(w) * h, 0);
        for (const auto& p : hills)
            m.hills[p[1] * w + p[0]] = 1;
        return m;
    };
    // 24 x 18 plots: a straight three-plot run, a diagonal run, an L, a blob, pairs and lone hills
    const std::vector<std::array<int, 2>> hills = {
        {3, 3},   {4, 3},   {5, 3},   {9, 2},   {10, 3},  {11, 4},  {15, 3},  {15, 4},  {15, 5},  {16, 5},
        {4, 9},   {5, 9},   {4, 10},  {5, 10},  {6, 10},  {5, 11},  {12, 9},  {13, 9},  {18, 10}, {19, 11},
        {8, 14},  {20, 15}, {2, 15},  {21, 2},  {12, 13}, {13, 14}, {14, 14}, {10, 7},  {11, 7},  {11, 8}};
    const Map map = makeMap(24, 18, false, hills);
    Field field;
    field.SetStamps(SyntheticStamps());
    field.Build(map);
    const int W = map.width, H = map.height;
    // every square's tile, laid out as the engine lays them (N apart), against the field sampled there directly
    auto layout = [&](const Field& f, int n, std::vector<uint8_t>& grid, int& cut, int& mismatched) {
        const int gw = n * (W + 1), gh = n * (H + 1);
        grid.assign(static_cast<size_t>(gw) * gh, 0);
        std::vector<uint8_t> t(static_cast<size_t>(n) * n);
        cut = mismatched = 0;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
                if (f.Tile(sx, sy, n, t.data()))
                {
                    ++cut;
                    for (int j = 0; j < n; ++j)
                        for (int i = 0; i < n; ++i)
                        {
                            const int gx = n * sx + i, gy = n * sy + j;
                            grid[static_cast<size_t>(gy) * gw + gx] = t[j * n + i];
                            const float X = static_cast<float>(gx) / n - 1, Y = H - static_cast<float>(gy) / n;
                            mismatched += t[j * n + i] != f.At(X, Y, n) ? 1 : 0;
                        }
                }
    };
    // curvature across tile seams over curvature across the lines either side of them (1 or 2 samples in), on hill
    // ground: 1 = no crease. (Against the whole inside it reads higher: seams run through the plot centres, where the
    // summits are.)
    auto seamRatio = [&](const std::vector<uint8_t>& g, int n) {
        const int gw = n * (W + 1), gh = n * (H + 1);
        std::vector<float> seam, inner;
        auto at = [&](int x, int y) { return static_cast<float>(g[static_cast<size_t>(y) * gw + x]); };
        auto beside = [n](int k) { return k % n == 1 || k % n == 2 || k % n == n - 1 || k % n == n - 2; };
        for (int y = 2; y + 2 < gh; ++y)
            for (int x = 2; x + 2 < gw; ++x)
            {
                const float c = at(x, y);
                if (c < 40)
                    continue;
                const float dx = std::fabs(at(x + 1, y) - 2 * c + at(x - 1, y)),
                            dy = std::fabs(at(x, y + 1) - 2 * c + at(x, y - 1));
                if (x % n == 0)
                    seam.push_back(dx);
                else if (beside(x))
                    inner.push_back(dx);
                if (y % n == 0)
                    seam.push_back(dy);
                else if (beside(y))
                    inner.push_back(dy);
            }
        auto mean = [](const std::vector<float>& v) {
            double s = 0;
            for (float x : v)
                s += x;
            return v.empty() ? 0.0f : static_cast<float>(s / v.size());
        };
        const float i = mean(inner);
        return i > 0 ? mean(seam) / i : 99.0f;
    };
    std::vector<uint8_t> grid16, grid64;
    int cut16 = 0, bad16 = 0, cut64 = 0, bad64 = 0;
    layout(field, 16, grid16, cut16, bad16);
    layout(field, 64, grid64, cut64, bad64);
    const float ratio16 = seamRatio(grid16, 16), ratio64 = seamRatio(grid64, 64);
    // the squares cut are exactly those with a hill corner (the engine asks only for those; the rest stay stock)
    int masked = 0, maskOk = 1, cutWithout = 0;
    std::vector<uint8_t> tile(64 * 64);
    for (int sy = 0; sy <= H; ++sy)
        for (int sx = 0; sx <= W; ++sx)
        {
            int c[4][2], mask = 0;
            TileSets::Corners(sx, sy, H, c);
            for (int k = 0; k < 4; ++k)
            {
                const int i = PlotIndex(map, c[k][0], c[k][1]);
                mask |= (i >= 0 && map.hills[i]) ? 1 << k : 0;
            }
            masked += mask ? 1 : 0;
            maskOk &= field.Mask(sx, sy) == mask ? 1 : 0;
            cutWithout += (!mask && field.Tile(sx, sy, 16, tile.data())) ? 1 : 0;
        }
    Field none;  // no stamps: nothing cut, every hill square keeps its stock tile
    none.Build(map);
    int noneCut = 0;
    for (int sy = 0; sy <= H; ++sy)
        for (int sx = 0; sx <= W; ++sx)
            noneCut += none.Tile(sx, sy, 16, tile.data()) ? 1 : 0;
    const bool stock = cutWithout == 0 && cut16 == masked && cut64 == masked && noneCut == 0 && !none.Usable();
    // multi-plot stamps; every hill plot in exactly one placement
    int sizes[4] = {}, owned = 0;
    for (const auto& p : field.Placements())
        ++sizes[(std::min)(3, static_cast<int>(p.frame.size()))], owned += static_cast<int>(p.plots.size());
    const bool multi = sizes[3] >= 3 && sizes[2] >= 1 && owned == static_cast<int>(hills.size());
    // a lone hill at a square's TL corner (mask 1): high at pixel (0, 0), nothing at the other three corners
    int c[4][2];
    TileSets::Corners(9, 4, H, c);  // square (9, 4): TL = plot (8, 14), a lone hill
    field.Tile(9, 4, 16, tile.data());
    const bool orient = c[0][0] == 8 && c[0][1] == 14 && field.Mask(9, 4) == 1 && tile[0] > 120 && tile[255] == 0 &&
                        tile[15] == 0 && tile[240] == 0;
    // deterministic: another field on the same map (hills listed in another order), and after another map, the same
    auto hashOf = [&](const Field& f) {
        uint32_t h = 2166136261u;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
                if (f.Tile(sx, sy, 16, tile.data()))
                    for (int k = 0; k < 256; ++k)
                        h = (h ^ tile[k]) * 16777619u;
        return h;
    };
    std::vector<std::array<int, 2>> reversed(hills.rbegin(), hills.rend());
    std::vector<std::array<int, 2>> moved = hills;
    moved[0] = {1, 1};
    Field again;
    again.SetStamps(SyntheticStamps());
    again.Build(makeMap(24, 18, false, moved));
    const uint32_t other = hashOf(again);
    again.Build(makeMap(24, 18, false, reversed));
    const uint32_t h1 = hashOf(field), h2 = hashOf(again);
    const bool deterministic = h1 == h2 && h1 != other;
    // a plot changed: only the four squares round it change their tiles
    auto tilesOf = [&](const Field& f) {
        std::vector<std::vector<uint8_t>> all;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
            {
                std::vector<uint8_t> t(256, 0);
                if (!f.Tile(sx, sy, 16, t.data()))
                    t.clear();
                all.push_back(t);
            }
        return all;
    };
    auto onlyRound = [&](const std::vector<std::vector<uint8_t>>& a, const std::vector<std::vector<uint8_t>>& b, int px,
                         int py, int& changed) {
        changed = 0;
        bool ok = true;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
                if (a[sy * (W + 1) + sx] != b[sy * (W + 1) + sx])
                {
                    ++changed;
                    int cc[4][2];
                    TileSets::Corners(sx, sy, H, cc);
                    bool round = false;
                    for (int k = 0; k < 4; ++k)
                        round = round || (cc[k][0] == px && cc[k][1] == py);
                    ok = ok && round;
                }
        return ok;
    };
    const auto before = tilesOf(field);
    Field local = field;
    Map added = map;
    added.hills[6 * W + 7] = 1;  // a new hill at (7, 6), beside nothing
    const int how1 = local.Update(added);
    int changed1 = 0, changed2 = 0;
    const bool add = how1 == 1 && onlyRound(before, tilesOf(local), 7, 6, changed1) && changed1 == 4;
    Field local2 = field;
    Map removed = map;
    removed.hills[3 * W + 4] = 0;  // the middle of the three-plot run
    const int how2 = local2.Update(removed);
    const bool remove = how2 == 1 && onlyRound(before, tilesOf(local2), 4, 3, changed2) && changed2 > 0;
    Field big = field;
    const int how3 = big.Update(makeMap(24, 18, false, {{1, 1}, {2, 1}, {3, 1}, {1, 5}, {2, 6}, {7, 7}, {7, 8}, {8, 8},
                                                       {20, 3}, {20, 4}, {16, 16}, {17, 16}}));
    const bool localOk = add && remove && how3 == 2 && field.Update(map) == 0;
    // a map wrapping east-west, with a run of hills across the seam: one stamp across it, and squares 0 and W the same
    const Map wrap = makeMap(12, 8, true, {{10, 4}, {11, 4}, {0, 4}, {5, 2}});
    Field wf;
    wf.SetStamps(SyntheticStamps());
    wf.Build(wrap);
    bool across = false;
    for (const auto& p : wf.Placements())
    {
        int lo = 99, hi = -99;
        for (const auto& f : p.frame)
            lo = (std::min)(lo, f[0]), hi = (std::max)(hi, f[0]);
        across = across || (p.frame.size() >= 2 && (lo < 0 || hi >= wrap.width));
    }
    bool same = true;
    std::vector<uint8_t> t0(256), tw(256);
    for (int sy = 0; sy <= wrap.height; ++sy)
    {
        const bool a = wf.Tile(0, sy, 16, t0.data()), b = wf.Tile(wrap.width, sy, 16, tw.data());
        same = same && a == b && (!a || t0 == tw);
    }
    const bool wrapped = across && same;
    // the tile as the engine reads it: texture +0x38 -> pixel data; pixels + offset, width, height, bytes a pixel
    EngineTile et;
    uint8_t* px = et.Pixels(32);
    int ew = 0, eh = 0, ebpp = 0;
    const auto* texture = static_cast<const uint8_t*>(et.Texture());
    uint32_t data, base, offs;
    memcpy(&data, texture + 0x38, 4);
    memcpy(&base, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(data)) + 0x50, 4);
    memcpy(&offs, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(data)) + 0x5c, 4);
    const uint32_t first = base + *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(offs));
    const bool engine = TileFormat(et.Texture(), ew, eh, ebpp) && ew == 32 && eh == 32 && ebpp == 1 &&
                        first == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(px)) &&
                        !TileFormat(nullptr, ew, eh, ebpp);
    // a Huge map's worth (128 x 80, wrapping): fitted, and every hill square cut at 64 x 64
    std::vector<std::array<int, 2>> many;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 128; ++x)
            if (ValueNoise(x * 0.35f, y * 0.35f, 99) > 0.62f && Unit(Mix(7, static_cast<uint32_t>(y * 128 + x))) < 0.75f)
                many.push_back({x, y});
    const Map huge = makeMap(128, 80, true, many);
    LARGE_INTEGER f0, q1, q2, q3;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&q1);
    Field hf;
    hf.SetStamps(SyntheticStamps());
    hf.Build(huge);
    QueryPerformanceCounter(&q2);
    int hugeCut = 0;
    for (int sy = 0; sy <= 80; ++sy)
        for (int sx = 0; sx <= 128; ++sx)
            hugeCut += hf.Tile(sx, sy, 64, tile.data()) ? 1 : 0;
    QueryPerformanceCounter(&q3);
    const int fitMs = static_cast<int>((q2.QuadPart - q1.QuadPart) * 1000 / f0.QuadPart);
    const int cutMs = static_cast<int>((q3.QuadPart - q2.QuadPart) * 1000 / f0.QuadPart);
    printf("HILLFIELD exact16=%d exact64=%d seam16=%d seam64=%d stock=%d mask=%d multi=%d orient=%d deterministic=%d "
           "local=%d wrap=%d engine=%d hugehills=%d hugecut=%d fitms=%d cutms=%d\n",
           bad16 == 0 && cut16 > 0, bad64 == 0 && cut64 > 0, static_cast<int>(ratio16 * 100 + 0.5f),
           static_cast<int>(ratio64 * 100 + 0.5f), stock, maskOk, multi, orient, deterministic, localOk, wrapped,
           engine, static_cast<int>(many.size()), hugeCut, fitMs, cutMs);
    printf("HILLFIELD placements: %d three-plot, %d two-plot, %d one-plot; squares cut %d of %d with a hill corner; "
           "changed squares %d (add), %d (remove); updates %d %d %d; gain %.2f\n",
           sizes[3], sizes[2], sizes[1], cut16, masked, changed1, changed2, how1, how2, how3, field.Gain());
    return 0;
}


// test_proxy jigsaw [pieces folder]: jigsaw tile sets (jigsaw.h) with made-up pieces on made-up maps (or, given a
// folder with pieces.xml, with those). Prints "JIGSAW name=value ..." for run_tests.py.
static std::vector<uint8_t> JigsawDds(int w, int h, uint32_t flags, uint32_t four, uint32_t bits, uint32_t rmask,
                                      const std::vector<uint8_t>& body, uint32_t dxgi = 0)
{
    std::vector<uint8_t> d(128 + (dxgi ? 20 : 0), 0);
    auto put = [&](size_t at, uint32_t v) { memcpy(d.data() + at, &v, 4); };
    memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x100F);
    put(12, h);
    put(16, w);
    put(76, 32);
    put(80, flags);
    put(84, four);
    put(88, bits);
    put(92, rmask);
    if (dxgi)
        put(128, dxgi);
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// A made-up piece for a layout: a rounded heap over its plots, zero from a plot's distance (Chebyshev) of 1 out, as
// a jigsaw piece must be; heights up to about 55 levels.
static Jigsaw::Piece MadeUpPiece(const char* name, std::vector<std::array<int, 2>> plots, uint32_t seed)
{
    int lo[2] = {99, 99}, hi[2] = {-99, -99};
    for (const auto& q : plots)
        for (int a = 0; a < 2; ++a)
            lo[a] = (std::min)(lo[a], q[a]), hi[a] = (std::max)(hi[a], q[a]);
    const int w = 64 * (hi[0] - lo[0] + 2), h = 64 * (hi[1] - lo[1] + 2);
    std::vector<uint16_t> px(static_cast<size_t>(w) * h, 0);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
        {
            const float X = lo[0] - 1 + c / 64.0f, Y = hi[1] + 1 - (r + 1) / 64.0f;
            float d = 2;
            for (const auto& q : plots)
                d = (std::min)(d, (std::max)(std::fabs(X - q[0]), std::fabs(Y - q[1])));
            if (d >= 1)
                continue;
            float t = 1 - d;
            t = t * t * (3 - 2 * t);
            const float wobble = 1 + 0.15f * (HillField::ValueNoise(X * 2.5f, Y * 2.5f, seed) - 0.5f);
            px[static_cast<size_t>(r) * w + c] = static_cast<uint16_t>(55 * 256 * t * wobble);
        }
    Jigsaw::Piece p;
    std::string why;
    Jigsaw::MakePiece(name, plots, w, h, std::move(px), p, why);
    return p;
}

static bool JigsawReadFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

static int JigsawTest(const wchar_t* folder)
{
    using namespace Jigsaw;
    // DDS heightmaps: L16 as it is, L8 / A8L8 / R8 / RGB red as levels (x 256), A16B16G16R16's red, DX10 R16; DXT refused
    std::vector<uint8_t> l16(8 * 4 * 2), l8(8 * 4), a8l8(8 * 4 * 2), rgb(8 * 4 * 4), rgba16(8 * 4 * 8);
    for (int i = 0; i < 32; ++i)
    {
        const uint16_t v = static_cast<uint16_t>(i * 1000);
        memcpy(&l16[i * 2], &v, 2);
        l8[i] = static_cast<uint8_t>(i * 3);
        a8l8[i * 2] = static_cast<uint8_t>(i * 3), a8l8[i * 2 + 1] = 0xff;
        rgb[i * 4 + 2] = static_cast<uint8_t>(i * 3), rgb[i * 4] = 77;  // BGRA in memory: red at +2
        memcpy(&rgba16[i * 8], &v, 2);
    }
    int w = 0, h = 0;
    std::vector<uint16_t> out;
    std::string why;
    auto same = [&](const std::vector<uint8_t>& dds, bool eight) {
        if (!DecodeDds(dds.data(), dds.size(), w, h, out, why) || w != 8 || h != 4 || out.size() != 32)
            return false;
        for (int i = 0; i < 32; ++i)
            if (out[i] != (eight ? i * 3 * 256 : i * 1000))
                return false;
        return true;
    };
    const bool dds = same(JigsawDds(8, 4, 0x20000, 0, 16, 0xffff, l16), false) && same(JigsawDds(8, 4, 0, 81, 0, 0, l16), false) &&
                     same(JigsawDds(8, 4, 0x20000, 0, 8, 0xff, l8), true) &&
                     same(JigsawDds(8, 4, 0x20001, 0, 16, 0xff, a8l8), true) &&
                     same(JigsawDds(8, 4, 0x41, 0, 32, 0xff0000, rgb), true) && same(JigsawDds(8, 4, 0, 36, 0, 0, rgba16), false) &&
                     same(JigsawDds(8, 4, 0x4, 0x30315844u, 0, 0, l16, 56), false) &&
                     same(JigsawDds(8, 4, 0x4, 0x30315844u, 0, 0, l8, 61), true);
    std::string why1, why2;
    const std::vector<uint8_t> dxt = JigsawDds(8, 4, 0x4, 0x31545844u, 0, 0, std::vector<uint8_t>(64));
    const std::vector<uint8_t> cut = JigsawDds(8, 4, 0x20000, 0, 16, 0xffff, std::vector<uint8_t>(30));
    const bool ddsRefused = !DecodeDds(dxt.data(), dxt.size(), w, h, out, why1) && !why1.empty() &&
                            !DecodeDds(cut.data(), cut.size(), w, h, out, why2) && !why2.empty();
    // pieces.xml: the height scale, layouts and their files; comments skipped; mistakes refused
    PiecesXml xml;
    const bool xmlOk =
        ParsePiecesXml("<?xml version=\"1.0\"?>\n<!-- <Layout><Plots>9,9</Plots><Piece>no.dds</Piece></Layout> -->\n"
                       "<JigsawPieces>\n\t<fHeightScale> 1.5 </fHeightScale>\n\t<Layout>\n\t\t<Type>3 L</Type>\n"
                       "\t\t<Plots>0,0 1,0\n 0,1</Plots>\n\t\t<Piece>3_L_01.dds</Piece>\n\t\t<Piece> 3_L_02.dds </Piece>\n"
                       "\t</Layout>\n\t<Layout><Plots>0,0</Plots><Piece>one.dds</Piece></Layout>\n</JigsawPieces>\n",
                       xml, why) &&
        xml.hasHeight && xml.heightScale == 1.5f && xml.pieces.size() == 3 && xml.pieces[1].file == "3_L_02.dds" &&
        xml.pieces[0].plots.size() == 3 && xml.pieces[0].plots[2][0] == 0 && xml.pieces[0].plots[2][1] == 1 &&
        xml.pieces[2].file == "one.dds";
    PiecesXml junkXml;
    std::string w1, w2, w3, w4;
    const bool xmlRefused =
        !ParsePiecesXml("<Layout><Plots>0,0</Plots><Piece>a.dds</Piece></Layout>", junkXml, w1) &&
        !ParsePiecesXml("<JigsawPieces><Layout><Plots>0;0</Plots><Piece>a.dds</Piece></Layout></JigsawPieces>", junkXml, w2) &&
        !ParsePiecesXml("<JigsawPieces><Layout><Plots>0,0 0,0</Plots><Piece>a.dds</Piece></Layout></JigsawPieces>", junkXml, w3) &&
        !ParsePiecesXml("<JigsawPieces><Layout><Plots>0,0</Plots></Layout></JigsawPieces>", junkXml, w4) &&
        !w1.empty() && !w2.empty() && !w3.empty() && !w4.empty();
    PiecesXml noHeight;
    const bool xmlDefault = ParsePiecesXml("<JigsawPieces><Layout><Plots>0,0</Plots><Piece>a.dds</Piece></Layout>"
                                           "</JigsawPieces>", noHeight, why) &&
                            !noHeight.hasHeight && noHeight.heightScale == 1.0f;
    // a piece's place from its plots, its pixels a plot from its size; a size that doesn't fit refused
    Piece made;
    std::string sizeWhy;
    const bool geometry = MakePiece("3 bent", {{0, 0}, {1, 0}, {2, 1}}, 128, 96, std::vector<uint16_t>(128 * 96), made, why) &&
                          made.west == -1 && made.north == 2 && made.ppp == 32 &&
                          !MakePiece("3 bent", {{0, 0}, {1, 0}, {2, 1}}, 128, 128, std::vector<uint16_t>(128 * 128), made,
                                     sizeWhy) &&
                          !sizeWhy.empty();
    // fields combined: the highest raised, the lowest lowered
    Combine mix;
    mix.Add(3);
    mix.Add(7);
    mix.Add(-2);
    mix.Add(-5);
    Combine none;
    const bool combine = mix.Value() == 2.0f && none.Value() == 0.0f;

    std::vector<Piece> pieces;
    const std::vector<std::pair<const char*, std::vector<std::array<int, 2>>>> layouts = {
        {"1", {{0, 0}}},
        {"2 straight", {{0, 0}, {1, 0}}},
        {"2 diagonal", {{0, 0}, {1, 1}}},
        {"3 L", {{0, 0}, {1, 0}, {0, 1}}},
        {"3 bent", {{0, 0}, {1, 0}, {2, 1}}},
        {"3 straight", {{0, 0}, {1, 0}, {2, 0}}},
        {"4 L", {{0, 0}, {1, 0}, {2, 0}, {0, 1}}},
        {"4 S", {{0, 0}, {1, 0}, {1, 1}, {2, 1}}}};
    uint32_t seed = 11;
    for (const auto& l : layouts)
        for (int v = 1; v <= 2; ++v)
            pieces.push_back(MadeUpPiece((std::string(l.first) + "_0" + std::to_string(v)).c_str(), l.second, ++seed));
    float heightScale = 1.2f;
    bool real = false;
    if (folder && *folder)
    {
        std::vector<uint8_t> bytes;
        PiecesXml theirs;
        real = JigsawReadFile(std::wstring(folder) + L"\\pieces.xml", bytes) &&
               ParsePiecesXml(std::string(bytes.begin(), bytes.end()), theirs, why);
        std::vector<Piece> read;
        int left = 0;
        for (const PieceFile& pf : theirs.pieces)
        {
            std::vector<uint8_t> file;
            std::vector<uint16_t> px;
            Piece p;
            std::string bad;
            if (JigsawReadFile(std::wstring(folder) + L"\\" + std::wstring(pf.file.begin(), pf.file.end()), file) &&
                DecodeDds(file.data(), file.size(), w, h, px, bad) && MakePiece(pf.file, pf.plots, w, h, std::move(px), p, bad))
                read.push_back(std::move(p));
            else
                ++left;
        }
        printf("JIGSAW pieces %ls: %s, %u pieces read, %d left out, height %.2f\n", folder, real ? "pieces.xml read" : why.c_str(),
               static_cast<unsigned>(read.size()), left, theirs.heightScale);
        real = real && !read.empty() && left == 0;
        if (real)
            pieces = std::move(read), heightScale = theirs.heightScale;
    }
    const float units = heightScale / kValuePerLevel;
    auto makeMap = [](int mw, int mh, bool wrapX, const std::vector<std::array<int, 2>>& plots) {
        Map m;
        m.width = mw;
        m.height = mh;
        m.wrapX = wrapX;
        m.hills.assign(static_cast<size_t>(mw) * mh, 0);
        for (const auto& p : plots)
            m.hills[p[1] * mw + p[0]] = 1;
        return m;
    };
    // ridge bands 1 to 3 plots wide running NW-SE (as Arrakis' dunes), with some lone plots
    std::vector<std::array<int, 2>> set;
    const int W = 30, H = 22;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float band = std::fmod(x + y * 1.88f, 9.0f);
            const float width = 1.0f + 2.0f * HillField::ValueNoise(x * 0.2f, y * 0.2f, 5);
            if (band < width * 1.6f || HillField::Unit(HillField::Mix(3, y * W + x)) < 0.03f)
                set.push_back({x, y});
        }
    const Map map = makeMap(W, H, false, set);
    Field field;
    field.SetPieces(pieces, units);
    field.Build(map);
    // every square's tile laid out as the engine lays them, against the field sampled there
    auto exact = [&](const Field& f, int n, int& cutCount) {
        std::vector<float> t(static_cast<size_t>(n) * n);
        int bad = 0;
        cutCount = 0;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
                if (f.Tile(sx, sy, n, t.data()))
                {
                    ++cutCount;
                    for (int j = 0; j < n; ++j)
                        for (int i = 0; i < n; ++i)
                        {
                            const float X = static_cast<float>(n * sx + i) / n - 1, Y = H - static_cast<float>(n * sy + j) / n;
                            bad += std::fabs(t[j * n + i] - f.At(X, Y)) > 1e-4f ? 1 : 0;
                        }
                }
        return bad == 0 && cutCount > 0;
    };
    int cut64 = 0, cut16 = 0;
    const bool exact64 = exact(field, 64, cut64), exact16 = exact(field, 16, cut16);
    int touching = 0;
    bool mask = true;
    for (int sy = 0; sy <= H; ++sy)
        for (int sx = 0; sx <= W; ++sx)
        {
            int c[4][2], m = 0;
            TileSets::Corners(sx, sy, H, c);
            for (int k = 0; k < 4; ++k)
            {
                const int i = PlotIndex(map, c[k][0], c[k][1]);
                m |= (i >= 0 && map.hills[i]) ? 1 << k : 0;
            }
            touching += m ? 1 : 0;
            mask = mask && field.Mask(sx, sy) == m;
        }
    // placements: exact lattice moves of a layout's plots; every plot of the set in one
    int sizes[17] = {}, owned = 0;
    bool lattice = true;
    std::vector<int> covered(map.hills.size(), 0);
    for (const auto& p : field.Placements())
    {
        ++sizes[(std::min)(16, static_cast<int>(p.frame.size()))];
        const int* s = kSym[p.sym];
        const auto& plots = field.Pieces()[p.piece].plots;
        lattice = lattice && p.sym >= 0 && p.sym < 8 && plots.size() == p.frame.size();
        for (size_t k = 0; lattice && k < plots.size(); ++k)
            lattice = p.frame[k][0] == s[0] * plots[k][0] + s[1] * plots[k][1] + p.tx &&
                      p.frame[k][1] == s[2] * plots[k][0] + s[3] * plots[k][1] + p.ty;
        for (int i : p.plots)
            ++covered[i], ++owned;
    }
    bool once = owned == static_cast<int>(set.size());
    for (size_t i = 0; i < covered.size(); ++i)
        once = once && covered[i] == (map.hills[i] ? 1 : 0);
    const int multiPlots = sizes[2] * 2 + sizes[3] * 3 + sizes[4] * 4;
    // one massif: the field between any two neighbouring plots of the set never drops to the base
    float lowest = 1e9f;
    for (const auto& a : set)
        for (const auto& b : set)
            if ((a[0] != b[0] || a[1] != b[1]) && std::abs(a[0] - b[0]) <= 1 && std::abs(a[1] - b[1]) <= 1)
                for (int k = 1; k < 8; ++k)
                {
                    const float t = k / 8.0f;
                    lowest = (std::min)(lowest, field.At(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t));
                }
    float peak = 0;
    for (const auto& a : set)
        peak = (std::max)(peak, field.At(static_cast<float>(a[0]), static_cast<float>(a[1])));
    // the base stamp's mask: a set's corners as water, as land, or kept; the tile added after; a set's corner the
    // engine has otherwise than the map as read leaves the square to the engine
    bool a1 = false, a2 = false, a3 = false, a4 = true, a5 = true;
    const bool base = BaseMask(15, 5, 5 | 2, 5, 0, a1) == 10 && a1 &&  // two land corners of a water set: out
                      BaseMask(0, 3, 0, 0, 3, a2) == 3 && a2 &&         // two water corners of a land set: in
                      BaseMask(7, 1, 1, 0, 0, a3) == 7 && a3 &&         // kept as they are
                      BaseMask(7, 0, 0, 0, 0, a4) == 7 && !a4 &&        // no set's corners: the engine's alone
                      BaseMask(3, 4, 4, 4, 0, a5) == 3 && !a5;          // a land corner the engine counts as water
    // the tile added into the engine's byte grid at the square's place, clamped 0..255; refused if it doesn't fit
    std::vector<uint8_t> cells(200 * 100, 40);
    cells[(1 * 16 + 2) * 200 + 2 * 16 + 3] = 250;
    std::vector<float> units16(256, 0.0f);
    units16[2 * 16 + 3] = 20.0f;
    units16[0] = 7.4f;
    units16[5] = -50.0f;
    const bool fits = AddTile(cells.data(), 200, 100, 2 * 16, 1 * 16, 16, units16.data());
    const bool grid = fits && cells[(1 * 16) * 200 + 2 * 16] == 47 && cells[(1 * 16 + 2) * 200 + 2 * 16 + 3] == 255 &&
                      cells[(1 * 16) * 200 + 2 * 16 + 5] == 0 && cells[(1 * 16) * 200 + 2 * 16 + 1] == 40 &&
                      cells[0] == 40 && !AddTile(cells.data(), 200, 100, 190, 0, 16, units16.data()) &&
                      !AddTile(cells.data(), 200, 100, 0, 90, 16, units16.data());
    // the grid object as the engine's stamps read it
    struct FakeInner
    {
        uint8_t pad[0x50];
        int32_t rowBytes, rows;
        uint32_t cells;
    } inner = {};
    struct FakeGrid
    {
        uint8_t pad0[4];
        uint32_t inner;
        uint8_t pad1[0x28];
        int32_t cellX, cellY;
    } fake = {};
    static_assert(offsetof(FakeInner, rowBytes) == 0x50 && offsetof(FakeInner, cells) == 0x58, "");
    static_assert(offsetof(FakeGrid, cellX) == 0x30 && offsetof(FakeGrid, cellY) == 0x34, "");
    inner.rowBytes = 200;
    inner.rows = 100;
    inner.cells = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cells.data()));
    fake.inner = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&inner));
    fake.cellX = fake.cellY = 17;
    GridView view;
    const bool gridView = ReadGrid(&fake, view) && view.cells == cells.data() && view.rowBytes == 200 &&
                          view.rows == 100 && view.cellX == 16 && !ReadGrid(nullptr, view);
    // a plot changed: only the four squares round it change
    auto tiles = [&](const Field& f) {
        std::vector<std::vector<float>> all;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
            {
                std::vector<float> t(256, 0.0f);
                if (!f.Tile(sx, sy, 16, t.data()))
                    t.clear();
                all.push_back(t);
            }
        return all;
    };
    auto onlyRound = [&](const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b, int px,
                         int py, int& changed) {
        changed = 0;
        bool ok = true;
        for (int sy = 0; sy <= H; ++sy)
            for (int sx = 0; sx <= W; ++sx)
                if (a[sy * (W + 1) + sx] != b[sy * (W + 1) + sx])
                {
                    ++changed;
                    int cc[4][2];
                    TileSets::Corners(sx, sy, H, cc);
                    bool round = false;
                    for (int k = 0; k < 4; ++k)
                        round = round || (cc[k][0] == px && cc[k][1] == py);
                    ok = ok && round;
                }
        return ok;
    };
    const auto before = tiles(field);
    int gone = -1;
    for (const auto& p : field.Placements())
        if (gone < 0 && p.frame.size() >= 3)
            gone = p.plots[1];
    int free = -1;
    for (int y = 2; y < H - 2 && free < 0; ++y)
        for (int x = 2; x < W - 2 && free < 0; ++x)
        {
            bool alone = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    alone = alone && !map.hills[(y + dy) * W + x + dx];
            if (alone)
                free = y * W + x;
        }
    Field f1 = field, f2 = field;
    Map m1 = map, m2 = map;
    if (gone >= 0)
        m1.hills[gone] = 0;
    if (free >= 0)
        m2.hills[free] = 1;
    int changed1 = 0, changed2 = 0;
    const int how1 = f1.Update(m1), how2 = f2.Update(m2);
    const bool local = gone >= 0 && free >= 0 && how1 == 1 && how2 == 1 &&
                       onlyRound(before, tiles(f1), gone % W, gone / W, changed1) && changed1 > 0 &&
                       onlyRound(before, tiles(f2), free % W, free / W, changed2) && changed2 == 4 &&
                       f1.Mask(gone % W + 1, H - gone / W) != field.Mask(gone % W + 1, H - gone / W) &&
                       field.Update(map) == 0;
    Field f3 = field;
    const bool refit = f3.Update(makeMap(W, H, false, {{1, 1}, {2, 1}, {5, 5}})) == 2 && f3.Placements().size() == 2;
    // deterministic: the same plots listed backwards give the same tiles; other plots, others
    auto hashOf = [&](const Field& f) {
        uint32_t hv = 2166136261u;
        for (const auto& t : tiles(f))
            for (float v : t)
                hv = (hv ^ static_cast<uint32_t>(std::lround(v * 16))) * 16777619u;
        return hv;
    };
    std::vector<std::array<int, 2>> reversed(set.rbegin(), set.rend()), moved = set;
    moved[3] = {0, 0};
    Field again;
    again.SetPieces(pieces, units);
    again.Build(makeMap(W, H, false, moved));
    const uint32_t other = hashOf(again);
    again.Build(makeMap(W, H, false, reversed));
    const bool deterministic = hashOf(again) == hashOf(field) && other != hashOf(field);
    // a map wrapping east-west with a pair either side of the seam: one piece across it, squares 0 and W the same
    const Map wrap = makeMap(12, 8, true, {{11, 4}, {0, 4}, {5, 2}});
    Field wf;
    wf.SetPieces(pieces, units);
    wf.Build(wrap);
    bool across = false;
    for (const auto& p : wf.Placements())
    {
        int lo = 99, hi = -99;
        for (const auto& f : p.frame)
            lo = (std::min)(lo, f[0]), hi = (std::max)(hi, f[0]);
        across = across || (p.frame.size() >= 2 && (lo < 0 || hi >= wrap.width));
    }
    bool sameEdge = true;
    std::vector<float> t0(256), tw(256);
    for (int sy = 0; sy <= wrap.height; ++sy)
    {
        const bool a = wf.Tile(0, sy, 16, t0.data()), b = wf.Tile(wrap.width, sy, 16, tw.data());
        sameEdge = sameEdge && a == b && (!a || t0 == tw);
    }
    // the hook site: a JMP to the thunk and a NOP over SUB ESP,0x10; CMP EDI,1, only over those bytes
    const uint32_t imgBase = 0x400000, thunk = 0x10006000;
    std::vector<uint8_t> img(0x80000, 0xcc);
    const auto sites = Sites(imgBase, thunk);
    memcpy(img.data() + kBaseStamp, sites[0].original.data(), sites[0].original.size());
    std::vector<uint8_t> otherImg = img;
    otherImg[kBaseStamp + 5] = 0x02;
    int writes = 0;
    auto write = [&](std::vector<uint8_t>& im) {
        return [&](uint32_t rva, const uint8_t* b, size_t n) { memcpy(im.data() + rva, b, n), ++writes; };
    };
    const std::string siteWhy = TerrainPatch::ApplySites(img.data(), img.size(), sites, write(img));
    int32_t rel;
    memcpy(&rel, img.data() + kBaseStamp + 1, 4);
    const bool hooked = siteWhy.empty() && writes == 1 && img[kBaseStamp] == 0xE9 &&
                        rel == static_cast<int32_t>(thunk - (imgBase + kBaseStamp + 5)) && img[kBaseStamp + 5] == 0x90;
    writes = 0;
    const std::string otherWhy = TerrainPatch::ApplySites(otherImg.data(), otherImg.size(), sites, write(otherImg));
    const bool siteRefused = !otherWhy.empty() && writes == 0;
    // a Huge map's worth of bands (128 x 80, wrapping): placed, and every square touching the set cut at 64 x 64
    std::vector<std::array<int, 2>> many;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 128; ++x)
            if (std::fmod(x + y * 1.88f, 9.0f) < 1.6f + 3.2f * HillField::ValueNoise(x * 0.2f, y * 0.2f, 8))
                many.push_back({x, y});
    LARGE_INTEGER f0, q1, q2, q3;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&q1);
    Field hf;
    hf.SetPieces(pieces, units);
    hf.Build(makeMap(128, 80, true, many));
    QueryPerformanceCounter(&q2);
    int hugeCut = 0;
    std::vector<float> big(64 * 64);
    for (int sy = 0; sy <= 80; ++sy)
        for (int sx = 0; sx <= 128; ++sx)
            hugeCut += hf.Tile(sx, sy, 64, big.data()) ? 1 : 0;
    QueryPerformanceCounter(&q3);
    printf("JIGSAW dds=%d ddsrefused=%d xml=%d xmlrefused=%d xmldefault=%d geometry=%d combine=%d exact64=%d exact16=%d "
           "cut=%d mask=%d lattice=%d once=%d massif=%d base=%d grid=%d gridview=%d local=%d refit=%d deterministic=%d "
           "wrap=%d hooked=%d siterefused=%d hugeplots=%d hugecut=%d fitms=%d cutms=%d real=%d\n",
           dds, ddsRefused, xmlOk, xmlRefused, xmlDefault, geometry, combine, exact64, exact16,
           cut64 == touching && cut16 == touching, mask, lattice, once, lowest > 0.5f, base, grid, gridView, local, refit,
           deterministic, across && sameEdge, hooked, siteRefused, static_cast<int>(many.size()), hugeCut,
           static_cast<int>((q2.QuadPart - q1.QuadPart) * 1000 / f0.QuadPart),
           static_cast<int>((q3.QuadPart - q2.QuadPart) * 1000 / f0.QuadPart), real);
    printf("JIGSAW placements on %d plots: %d four-plot, %d three-plot, %d two-plot, %d one-plot (%d%% of plots in "
           "multi-plot pieces); lowest between neighbours %.1f units, highest %.1f; squares %d; changed %d / %d\n",
           static_cast<int>(set.size()), sizes[4], sizes[3], sizes[2], sizes[1],
           set.empty() ? 0 : 100 * multiPlots / static_cast<int>(set.size()), lowest, peak, touching, changed1, changed2);
    return 0;
}

// test_proxy plotstamps [pieces folder]: plot stamps (plot_stamps.h) with a made-up piece (or, given a folder with
// pieces.xml, also reports its pieces). Prints "PLOTSTAMPS name=value ..." for run_tests.py.
static int PlotStampsTest(const wchar_t* folder)
{
    using namespace PlotStamps;
    // the pick: the Nth listed feature the Nth piece (wrapping past the pieces), else by plot
    const std::vector<int> features = {10, -1, 12, 13};  // -1: a name the game doesn't have keeps its place
    std::set<int> used;
    bool steady = true, inRange = true;
    for (int x = 0; x < 20; ++x)
        for (int y = 0; y < 10; ++y)
        {
            const int k = Pick(-1, features, 3, x, y);
            used.insert(k);
            inRange = inRange && k >= 0 && k < 3;
            steady = steady && Pick(7, features, 3, x, y) == k;  // an unlisted feature: by plot too
        }
    const bool pick = Pick(10, features, 3, 4, 4) == 0 && Pick(12, features, 3, 4, 4) == 2 &&
                      Pick(13, features, 3, 4, 4) == 0 && Pick(10, features, 0, 4, 4) == -1 && used.size() == 3 &&
                      steady && inRange;

    // a one-plot piece, 64 pixels a plot (128 across: the plot's centre and one plot round it), a cone of 90 levels at
    // the centre, 0.45 plots across its foot, with a step on its north-west flank
    const int w = 128;
    std::vector<uint16_t> px(static_cast<size_t>(w) * w, 0);
    for (int r = 0; r < w; ++r)
        for (int c = 0; c < w; ++c)
        {
            const float X = -1 + c / 64.0f, Y = 1 - (r + 1) / 64.0f;  // where the pieces' pixels lie (jigsaw.h)
            const float d = std::sqrt(X * X + Y * Y);
            float v = d < 0.45f ? 90.0f * (1 - d / 0.45f) : 0.0f;
            if (X < -0.1f && X > -0.3f && Y > 0.1f && Y < 0.3f)
                v += 20;
            px[static_cast<size_t>(r) * w + c] = static_cast<uint16_t>(v * 256);
        }
    Jigsaw::Piece piece;
    std::string why;
    const bool made = Jigsaw::MakePiece("cone.dds", {{0, 0}}, w, w, px, piece, why);
    const bool orient = made && Sample(piece, -0.2f, 0.2f) > Sample(piece, 0.2f, -0.2f) + 15 * 256 &&
                        std::fabs(Sample(piece, 0.2f, 0.2f) - Sample(piece, -0.2f, -0.2f)) < 64 &&
                        Sample(piece, 0.2f, 0.2f) > 30 * 256 && Sample(piece, 0.7f, 0.0f) == 0 &&
                        Sample(piece, 1.5f, 0.0f) == 0 && Sample(piece, 0, 0) > 85 * 256;

    // the four squares round plot (5, 5) on a 12-high map: each pixel the stamp sampled there, in grid units (1/256 a
    // value: a level a grid unit); together they hold the whole stamp
    const int H = 12, px5 = 5, py5 = 5;
    const float units = 1.0f / 256;
    const Stamp one = {&piece, px5, py5, units};
    const int squares[4][2] = {{px5 + 1, H - py5}, {px5, H - py5}, {px5, H - py5 - 1}, {px5 + 1, H - py5 - 1}};
    bool exact = true, reached = true, inSquares = true;
    float highest = 0;
    for (int n : {64, 16})
        for (const auto& sq : squares)
        {
            int corners[4][2];
            TileSets::Corners(sq[0], sq[1], H, corners);
            bool corner = false;
            for (const auto& c : corners)
                corner = corner || (c[0] == px5 && c[1] == py5);
            inSquares = inSquares && corner;
            std::vector<float> tile(static_cast<size_t>(n) * n);
            Tile(&one, 1, sq[0], sq[1], H, n, tile.data());
            float most = 0;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                {
                    const float X = sq[0] - 1 + static_cast<float>(i) / n, Y = H - sq[1] - static_cast<float>(j) / n;
                    const float want = Sample(piece, X - px5, Y - py5) * units;
                    exact = exact && std::fabs(tile[j * n + i] - want) < 1e-3f;
                    most = (std::max)(most, tile[j * n + i]);
                }
            reached = reached && most > 1;
            highest = (std::max)(highest, most);
        }
    const bool peak = highest > 85 && highest <= 90.01f;

    // stamps side by side on square (6, 7) (corners TL (5, 5), TR (6, 5)), the cone and a 10-level slab over the whole
    // plot round its own: the higher of the two everywhere (the cone over the slab near its top, the slab elsewhere)
    Jigsaw::Piece slab;
    Jigsaw::MakePiece("slab.dds", {{0, 0}}, w, w, std::vector<uint16_t>(static_cast<size_t>(w) * w, 10 * 256), slab, why);
    const Stamp two[2] = {{&piece, 5, 5, units}, {&slab, 6, 5, units}};
    std::vector<float> both(32 * 32), a(32 * 32), b(32 * 32);
    Tile(two, 2, 6, H - 5, H, 32, both.data());
    Tile(&two[0], 1, 6, H - 5, H, 32, a.data());
    Tile(&two[1], 1, 6, H - 5, H, 32, b.data());
    bool combine = true, aOver = false, bOver = false;
    for (size_t i = 0; i < both.size(); ++i)
    {
        combine = combine && both[i] == (std::max)(a[i], b[i]);
        aOver = aOver || (a[i] > b[i] && b[i] > 1);
        bOver = bOver || (b[i] > a[i] && a[i] > 1);
    }
    combine = combine && aOver && bOver;

    // added into the height bytes: where ground + stamp passes 255 the sample is clipped there, and counted
    std::vector<uint8_t> cells(40 * 40, 200);
    std::vector<float> tile(16 * 16), total(16 * 16);
    Tile(&one, 1, px5, H - py5, H, 16, tile.data());
    int over = 0, under = 0;
    for (size_t i = 0; i < tile.size(); ++i)
    {
        total[i] = tile[i] + 1;  // e.g. a jigsaw piece's unit under it
        over += tile[i] > 0.5f && 200 + std::lround(total[i]) > 255 ? 1 : 0;
        under += tile[i] > 0.5f && 200 + std::lround(total[i]) <= 255 ? 1 : 0;
    }
    const int clipped = Clipped(cells.data(), 40, 40, 16, 16, 16, total.data(), tile.data());
    const bool added = Jigsaw::AddTile(cells.data(), 40, 40, 16, 16, 16, total.data());
    bool capped = true;
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i)
        {
            const int v = cells[(16 + j) * 40 + 16 + i];
            capped = capped && v == (std::min)(255, 200 + static_cast<int>(std::lround(total[j * 16 + i])));
        }
    const bool clip = over > 0 && under > 0 && clipped == over && added && capped &&
                      Clipped(cells.data(), 40, 40, 30, 30, 16, total.data(), tile.data()) == 0;  // off the grid

    if (folder)
    {
        std::vector<uint8_t> bytes;
        Jigsaw::PiecesXml xml;
        std::string bad = "pieces.xml can't be read";
        const bool real = JigsawReadFile(std::wstring(folder) + L"\\pieces.xml", bytes) &&
                          Jigsaw::ParsePiecesXml(std::string(bytes.begin(), bytes.end()), xml, bad);
        printf("PLOTSTAMPS pieces %ls: %s, fHeightScale %.2f\n", folder, real ? "pieces.xml read" : bad.c_str(),
               xml.heightScale);
        for (const Jigsaw::PieceFile& pf : xml.pieces)
        {
            std::vector<uint8_t> file;
            std::vector<uint16_t> pixels;
            int pw = 0, ph = 0;
            Jigsaw::Piece p;
            std::string pwhy = "can't be read";
            if (pf.plots.size() == 1 &&
                JigsawReadFile(std::wstring(folder) + L"\\" + std::wstring(pf.file.begin(), pf.file.end()), file) &&
                Jigsaw::DecodeDds(file.data(), file.size(), pw, ph, pixels, pwhy) &&
                Jigsaw::MakePiece(pf.file, pf.plots, pw, ph, std::move(pixels), p, pwhy))
            {
                // the stamp's foot: how far from the plot's centre it reaches, in plot widths
                float tallest = 0, reach = 0;
                for (int r = 0; r < p.h; ++r)
                    for (int c = 0; c < p.w; ++c)
                        if (const uint16_t v = p.pixels[static_cast<size_t>(r) * p.w + c])
                        {
                            tallest = (std::max)(tallest, v / 256.0f);
                            reach = (std::max)(reach, (std::max)(std::fabs(p.west + (c + 0.5f) / p.ppp),
                                                                 std::fabs(p.north - (r + 0.5f) / p.ppp)));
                        }
                printf("PLOTSTAMPS piece %s: %dx%d, %.0f pixels a plot, tallest %.1f levels, reaching %.2f plots from "
                       "the plot's centre\n",
                       pf.file.c_str(), p.w, p.h, p.ppp, tallest, reach);
            }
            else
                printf("PLOTSTAMPS piece %s left out: %s\n", pf.file.c_str(),
                       pf.plots.size() != 1 ? "not a one-plot layout" : pwhy.c_str());
        }
    }
    printf("PLOTSTAMPS pick=%d orient=%d exact=%d reached=%d squares=%d peak=%d combine=%d clip=%d\n", pick, orient,
           exact, reached, inSquares, peak, combine, clip);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && wcscmp(argv[1], L"plotstamps") == 0)
        return PlotStampsTest(argc > 2 ? argv[2] : nullptr);
    if (argc > 1 && wcscmp(argv[1], L"terrainpatch") == 0)
        return TerrainPatchTest();
    if (argc > 1 && wcscmp(argv[1], L"tilesets") == 0)
        return TileSetsTest();
    if (argc > 1 && wcscmp(argv[1], L"hillfield") == 0)
        return HillFieldTest();
    if (argc > 1 && wcscmp(argv[1], L"jigsaw") == 0)
        return JigsawTest(argc > 2 ? argv[2] : nullptr);
    if (argc > 1 && wcscmp(argv[1], L"terraincells") == 0)
        return TerrainCellsTest();
    // A trailing mod=<folder> is for the proxy, which reads the command line itself (the way BtS is started with a mod)
    while (argc > 2 && _wcsnicmp(argv[argc - 1], L"mod=", 4) == 0)
        --argc;
    if (argc < 2)
    {
        printf("usage: test_proxy <path to proxy d3d9.dll> [frames]\n");
        return 2;
    }
    int frames = argc > 2 ? _wtoi(argv[2]) : 4;
    const wchar_t* requestFile = argc > 3 ? argv[3] : nullptr;  // created after frame 5, like hooktrace.py request
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);

    // What Colonization does at startup: load d3d9.dll, check capabilities, release, FreeLibrary. Later the renderer
    // loads "d3d9.dll" by base name; that must still be the proxy, not the system DLL the proxy loaded.
    HMODULE first = LoadLibraryW(argv[1]);
    if (!first)
    {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    {
        auto check = reinterpret_cast<CreateFn>(GetProcAddress(first, "Direct3DCreate9"));
        IDirect3D9* probe = check ? check(D3D_SDK_VERSION) : nullptr;
        D3DCAPS9 caps = {};
        if (probe)
        {
            probe->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
            probe->Release();
        }
        FreeLibrary(first);
    }
    HMODULE proxy = LoadLibraryW(L"d3d9.dll");
    wchar_t loaded[MAX_PATH] = {};
    GetModuleFileNameW(proxy, loaded, MAX_PATH);
    wchar_t wanted[MAX_PATH] = {};
    GetFullPathNameW(argv[1], MAX_PATH, wanted, nullptr);
    if (!proxy || _wcsicmp(loaded, wanted) != 0)
    {
        printf("FAILED: after FreeLibrary, LoadLibrary(\"d3d9.dll\") returned %ls\n", loaded);
        return 1;
    }
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(proxy, "Direct3DCreate9"));
    if (!create || !GetProcAddress(proxy, "D3DPERF_GetStatus"))
    {
        printf("missing exports\n");
        return 1;
    }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d)
    {
        printf("Direct3DCreate9 returned NULL\n");
        return 1;
    }

    HWND wnd = CreateWindowExW(0, L"STATIC", L"GraphicsUpgrade tests", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr,
                               nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE, &pp, &dev);
    if (FAILED(hr))
        CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                                &dev));

    // The wrapper must hand back itself from QueryInterface and the wrapped IDirect3D9 from GetDirect3D.
    IDirect3DDevice9* same = nullptr;
    CHECK(dev->QueryInterface(IID_IDirect3DDevice9, reinterpret_cast<void**>(&same)));
    if (same != dev)
    {
        printf("QueryInterface returned a different pointer\n");
        return 1;
    }
    same->Release();
    IDirect3D9* parent = nullptr;
    CHECK(dev->GetDirect3D(&parent));
    if (parent != d3d)
    {
        printf("GetDirect3D returned a different pointer\n");
        return 1;
    }
    parent->Release();

    if (argc > 3 && wcscmp(argv[2], L"wavecmp") == 0)
    {
        // test_proxy <dll> wavecmp <folder of BtS's extracted shaders>
        if (WaveCompareScene(dev, argv[3]) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: wavecmp, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 3 && wcscmp(argv[2], L"border") == 0)
    {
        // test_proxy <dll> border <folder of BtS's extracted shaders>
        if (BorderScene(dev, argv[3]) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: border, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 3 && wcscmp(argv[2], L"watercmp") == 0)
    {
        // test_proxy <dll> watercmp <folder of Colonization's extracted shaders>
        if (WaterCompareScene(dev, argv[3]) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: watercmp, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 2 && wcscmp(argv[2], L"water") == 0)
    {
        if (WaterScene(dev, 3) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: water, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 3 && wcscmp(argv[2], L"surface") == 0)
    {
        // test_proxy <dll> surface <.dds listed in [water] SurfaceTextures>
        if (SurfaceScene(dev, argv[3]) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: surface, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 2 && wcscmp(argv[2], L"light") == 0)
    {
        // test_proxy <dll> light [ff [tex]] [rt | screen]
        bool ff = false, rt = false, screen = false, tex = false;
        for (int a = 3; a < argc; ++a)
        {
            ff = ff || wcscmp(argv[a], L"ff") == 0;
            tex = tex || wcscmp(argv[a], L"tex") == 0;
            rt = rt || wcscmp(argv[a], L"rt") == 0;
            screen = screen || wcscmp(argv[a], L"screen") == 0;
        }
        if (LightScene(dev, pp, ff, rt, screen, tex) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: light, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 2 && wcscmp(argv[2], L"shadow") == 0)
    {
        // test_proxy <dll> shadow [blob .dds or -] [overlay: none, river0, river1, decal, symbol, overlay, effect*, tree,
        //            building]
        //            [folder of the game's extracted shaders] [flagpole .dds: the building drawn with it]
        const wchar_t* blobArg = argc > 3 && wcscmp(argv[3], L"-") != 0 ? argv[3] : nullptr;
        if (ShadowScene(dev, pp, 6, blobArg, argc > 4 ? argv[4] : L"none", argc > 5 ? argv[5] : nullptr,
                        argc > 6 ? argv[6] : nullptr, argc > 7 ? static_cast<DWORD>(_wtoi(argv[7])) : 0) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: shadow, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 2 && wcscmp(argv[2], L"reflect") == 0)
    {
        if (ReflectScene(dev, pp, 4) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: reflect, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }

    if (GhostScene(dev) != 0)
        return 1;

    std::vector<DWORD> vsCode = Tokens(g_testVS, sizeof(g_testVS)), psCode = Tokens(g_testPS, sizeof(g_testPS));
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    CHECK(dev->CreateVertexShader(vsCode.data(), &vs));
    CHECK(dev->CreatePixelShader(psCode.data(), &ps));
    const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    CHECK(dev->CreateVertexDeclaration(elems, &decl));

    const Vertex tri[] = {{-0.8f, -0.8f, 0.5f, 0, 1}, {0, 0.8f, 0.5f, 0.5f, 0}, {0.8f, -0.8f, 0.5f, 1, 1}};
    IDirect3DVertexBuffer9* vb = nullptr;
    CHECK(dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr));
    void* p = nullptr;
    CHECK(vb->Lock(0, 0, &p, 0));
    memcpy(p, tri, sizeof(tri));
    vb->Unlock();

    // Managed texture with a known pattern, so its content hash is reproducible.
    IDirect3DTexture9* tex = nullptr;
    CHECK(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr));
    D3DLOCKED_RECT lr;
    CHECK(tex->LockRect(0, &lr, nullptr, 0));
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch)[x] = 0xFF000000u | (x * 4 << 16) | (y * 4 << 8);
    tex->UnlockRect(0);
    IDirect3DCubeTexture9* cube = nullptr;
    CHECK(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr));

    // Offscreen render target, like a reflection texture.
    IDirect3DTexture9* rtTex = nullptr;
    IDirect3DSurface9 *rtSurf = nullptr, *backbuffer = nullptr;
    CHECK(dev->CreateTexture(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex, nullptr));
    CHECK(rtTex->GetSurfaceLevel(0, &rtSurf));
    CHECK(dev->GetRenderTarget(0, &backbuffer));

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float tint[4] = {1, 0.5f, 0.25f, 1};
    const float plane[4] = {0, 0, 1, -0.25f};
    for (int f = 0; f < frames; ++f)
    {
        CHECK(dev->BeginScene());
        // Pass 1: offscreen, shader path
        CHECK(dev->SetRenderTarget(0, rtSurf));
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0));
        CHECK(dev->SetVertexDeclaration(decl));
        CHECK(dev->SetStreamSource(0, vb, 0, sizeof(Vertex)));
        CHECK(dev->SetVertexShader(vs));
        CHECK(dev->SetPixelShader(ps));
        CHECK(dev->SetVertexShaderConstantF(0, identity, 4));
        CHECK(dev->SetPixelShaderConstantF(0, tint, 1));
        CHECK(dev->SetTexture(0, tex));
        CHECK(dev->SetClipPlane(0, plane));
        CHECK(dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0));
        CHECK(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
        CHECK(dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
        // Pass 2: back buffer, sampling the offscreen target, plus a copy and a fixed-function draw
        CHECK(dev->SetRenderTarget(0, backbuffer));
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF203040, 1.0f, 0));
        RECT dst = {0, 0, 64, 64};
        CHECK(dev->StretchRect(rtSurf, nullptr, backbuffer, &dst, D3DTEXF_LINEAR));
        CHECK(dev->SetTexture(0, rtTex));
        CHECK(dev->SetTexture(1, cube));
        CHECK(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
        CHECK(dev->SetTexture(1, nullptr));
        CHECK(dev->SetVertexShader(nullptr));
        CHECK(dev->SetPixelShader(nullptr));
        CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1));
        D3DMATRIX m;
        memcpy(&m, identity, sizeof(m));
        CHECK(dev->SetTransform(D3DTS_WORLD, &m));
        m._43 = 2.0f;
        CHECK(dev->SetTransform(D3DTS_VIEW, &m));
        CHECK(dev->SetTexture(0, tex));
        CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex)));
        CHECK(dev->EndScene());
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
        if (requestFile && f == 5)
            CloseHandle(CreateFileW(requestFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    }

    backbuffer->Release();
    rtSurf->Release();
    rtTex->Release();
    cube->Release();
    tex->Release();
    vb->Release();
    decl->Release();
    ps->Release();
    vs->Release();
    ULONG devRefs = dev->Release();
    ULONG d3dRefs = d3d->Release();
    DestroyWindow(wnd);
    printf("OK: %d frames, final refs device %lu, d3d %lu\n", frames, devRefs, d3dRefs);
    return 0;
}
