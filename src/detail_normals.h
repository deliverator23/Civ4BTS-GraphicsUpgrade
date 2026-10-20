// Detail normals: small bumps on the ground, tiled like BtS's terrain detail textures and lit by the sun as it is now
// (terrain_shadow.hlsl).
//
// Each terrain's detail texture (CIV4ArtDefines_Terrain.xml, <Detail>) can have a normal map beside it, the same path
// with _normal before .dds (e.g. Art/Terrain/Textures/detail/desertdetail_normal.dds), in the running mod or the game,
// loose or packed. It's tangent space: red along the texture's u, green up the image (DirectX), blue out of the ground.
// The detail texture bound for a terrain draw is recognised by its content (TextureList), so the right map is found
// however the game loaded it. A detail texture without one gets normals made from its own brightness, read as height,
// in the shader ([textures] DetailBumpiness).
//
// Blend sheets too: a terrain's <Path> (its blend sheet) can have a normal map beside it the same way (e.g.
// Art/Terrain/Textures/blend/rockblend_coast_normal.dds), the same layout as the sheet, used for the sheet and its
// <Grid> (recognised by content), read at the sheet's own texture coordinates ([textures] BlendNormalStrength). It
// may be an ordinary normal map or DXT5nm (x in alpha, y in green; DXT5 with the DDS's DDPF_NORMAL flag).
#pragma once
#include "assets.h"
#include "common.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class DetailNormals
{
public:
    explicit DetailNormals(IDirect3DDevice9* device) : m_dev(device) {}
    ~DetailNormals() { ReleaseAll(); }

    bool enabled = g_config.detailNormals;  // [textures] DetailNormals, switched in game by DetailNormalsToggleKey

    // What to use with this detail texture (BtS's s3 for a terrain draw): a normal map (null: none shipped) and the
    // size of one texel in uv. Looks the settings' files up on first use.
    struct Use
    {
        IDirect3DTexture9* normalMap = nullptr;
        float texel[2] = {};
    };
    Use For(IDirect3DBaseTexture9* detail);
    // The normal map for this blend sheet (BtS's s0 for a terrain draw; null if none was shipped), and whether it's
    // DXT5nm.
    struct SheetUse
    {
        IDirect3DTexture9* map = nullptr;
        bool dxt5nm = false;
    };
    SheetUse ForBlend(IDirect3DBaseTexture9* sheet);
    void ForgetTexture(const void* texture)
    {
        m_cache.erase(texture);
        m_blendCache.erase(texture);
    }
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    void Load();

    struct Shipped
    {
        std::wstring detail, normal;  // paths under Assets
        TextureList match;            // the detail texture
        std::vector<uint8_t> dds;     // the normal map, until its texture is made
        IDirect3DTexture9* texture = nullptr;
        bool failed = false;
        bool dxt5nm = false;
    };
    IDirect3DDevice9* m_dev;
    bool m_loaded = false;
    std::vector<std::unique_ptr<Shipped>> m_shipped;
    std::unordered_map<const void*, Use> m_cache;
    std::vector<std::unique_ptr<Shipped>> m_blendShipped;  // detail = the sheet's <Path> (and its <Grid>)
    std::unordered_map<const void*, SheetUse> m_blendCache;
    // The normal map's texture, made on first use (d3dx9_33); null if it can't be (logged).
    IDirect3DTexture9* Texture(Shipped& s, const char* what);
    ULONG m_internalRefs = 0;
};
