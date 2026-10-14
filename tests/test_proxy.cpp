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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
#include "water_vs.h"

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
        circlePixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) +
                                                     static_cast<UINT>(bd.Height * 0.725f) * lr.Pitch)[bd.Width / 4];
        hiddenPixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) +
                                                     static_cast<UINT>(bd.Height * 0.775f) * lr.Pitch)[bd.Width / 4];
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("WATER pixel=%08lx restored=%d circle=%08lx hidden=%08lx\n", static_cast<unsigned long>(pixel), restored,
           static_cast<unsigned long>(circlePixel), static_cast<unsigned long>(hiddenPixel));
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
//   vanish           no extra draws; frame 4 (after the Reset) draws nothing at all (a full-screen menu) and frame 5
//                    has no caster: the ground mustn't show frame 3's shadow
//   building         a lit fixed-function mesh (normals, depth writes) 100 units up at world (200,300), like a
//                    building: with [shadows] BuildingShadows=1 its shadow lands at (100,300); adds " bld=."
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
               building = wcscmp(overlay, L"building") == 0, ridge = wcscmp(overlay, L"ridge") == 0,
               vanish = wcscmp(overlay, L"vanish") == 0, waterOverlay = wcscmp(overlay, L"water") == 0,
               slope = wcscmp(overlay, L"slope") == 0 || ridge;
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
    if (waterOverlay)
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
        if (slope)
        {
            CHECK(dev->SetStreamSource(0, slopeVb, 0, 28));
            CHECK(dev->SetIndices(slopeIb));
            CHECK(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 25, 0, 32));
            CHECK(dev->SetStreamSource(0, nullptr, 0, 0));
            CHECK(dev->SetIndices(nullptr));
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

        if (waterOverlay)
        {
            // x, y, z, coast uv, fog uv
            const float wq[6][7] = {{-500, -500, 1, 0, 0, 0, 0}, {-500, 500, 1, 0, 0, 0, 0}, {500, 500, 1, 0, 0, 0, 0},
                                    {-500, -500, 1, 0, 0, 0, 0}, {500, 500, 1, 0, 0, 0, 0},  {500, -500, 1, 0, 0, 0, 0}};
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
static int LightScene(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS& pp, bool ff)
{
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
    CHECK(dev->CreateOffscreenPlainSurface(pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
                                          D3DPOOL_SYSTEMMEM, &sys, nullptr));
    const float bones[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float light[12] = {0, 0, -1, 0, 0.5f, 0.5f, 0.5f, 0, 0.1f, 0.2f, 0.3f, 0};  // direction, diffuse, ambient
    // x, y, z, normal (facing +z, towards the light)
    const float tri[3][6] = {{-0.5f, -0.5f, 0.5f, 0, 0, 1}, {0, 0.5f, 0.5f, 0, 0, 1}, {0.5f, -0.5f, 0.5f, 0, 0, 1}};
    DWORD pixel = 0;
    int restored = 1;
    for (int f = 0; f < 2; ++f)
    {
        CHECK(dev->BeginScene());
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0));
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
            CHECK(dev->SetVertexShader(nullptr));
            CHECK(dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL));
            CHECK(dev->SetTransform(D3DTS_WORLD, &id));
            CHECK(dev->SetTransform(D3DTS_VIEW, &id));
            CHECK(dev->SetTransform(D3DTS_PROJECTION, &id));
            CHECK(dev->SetLight(11, &sun));
            CHECK(dev->LightEnable(11, TRUE));
            CHECK(dev->SetMaterial(&material));
            CHECK(dev->SetRenderState(D3DRS_AMBIENT, 0x00102030));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, TRUE));
            CHECK(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 24));
            CHECK(dev->SetRenderState(D3DRS_LIGHTING, FALSE));
            D3DLIGHT9 after = {};
            DWORD ambient = 0;
            dev->GetLight(11, &after);
            dev->GetRenderState(D3DRS_AMBIENT, &ambient);
            if (after.Diffuse.r != 0.5f || after.Direction.z != -1 || ambient != 0x00102030)
                restored = 0;
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
        CHECK(dev->EndScene());
        CHECK(dev->GetRenderTargetData(bb, sys));
        D3DLOCKED_RECT lr;
        CHECK(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        pixel = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) +
                                                (pp.BackBufferHeight / 2) * lr.Pitch)[pp.BackBufferWidth / 2];
        sys->UnlockRect();
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    printf("LIGHT pixel=%08lx restored=%d\n", static_cast<unsigned long>(pixel), restored);
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    sys->Release();
    bb->Release();
    decl->Release();
    vs->Release();
    return 0;
}

// Our water shader (shaders\water.hlsl) against the original, Colonization's (the game's compiled
// shaders, extracted from its compiled effects into colDir): both drawn over a 16x16 grid of water with the same random textures
// (normal map, coast fade, fog, grid, environment cube, reflection, refraction) and realistic constants, a different
// view direction at every vertex. Prints "WATERCMP max=. differing=. pixels=. colours=." (the largest difference in
// any channel, how many pixels differ at all, and how many distinct colours the original drew, to show the picture
// isn't flat).
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

int wmain(int argc, wchar_t** argv)
{
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
        if (LightScene(dev, pp, argc > 3 && wcscmp(argv[3], L"ff") == 0) != 0)
            return 1;
        ULONG devRefs = dev->Release();
        ULONG d3dRefs = d3d->Release();
        DestroyWindow(wnd);
        printf("OK: light, final refs device %lu, d3d %lu\n", devRefs, d3dRefs);
        return 0;
    }
    if (argc > 2 && wcscmp(argv[2], L"shadow") == 0)
    {
        // test_proxy <dll> shadow [blob .dds or -] [overlay: none, river0, river1, decal, symbol, overlay, tree,
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
