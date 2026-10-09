// Frame trace: a JSON-lines record of everything one frame does, read back from the device at each call.
//
// Reading state back (rather than shadow-tracking Set* calls) keeps traces right when D3DX effects apply state
// blocks, which change device state without going through our wrapper.
#pragma once
#include "common.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ShaderInfo
{
    uint64_t hash = 0;
    std::string version;  // e.g. "vs_1_1"
    // Float registers of named constants, from the shader's constant table (CTAB); -1 if absent.
    int viewProj = -1;     // mtxViewProj (4 registers): units and ships, bones are in world space
    int worldBones = -1;   // mtxWorldBones (skinning palette)
    int world = -1;        // mtxWorld
    bool water = false;    // water plane shader (BtS mtxWaterTextureMat, Col mtxWaterTexture1)
    bool btsWater = false; // BtS's own water shader (mtxWaterTextureMat): the one the water replaces
    bool btsTerrain = false; // BtS terrain (mtxLightmap + fDetailTexScaling): receives sun shadows
    bool tree = false;     // BtS trees (ContourShader: WorldViewProj + World + windir): cast sun shadows
    // Light constants (lighting.h): group (LightGroup, -1 = none) and float registers of its diffuse, ambient and
    // direction constants (-1 if absent).
    int lightGroup = -1, lightDiffuse = -1, lightAmbient = -1, lightDir = -1;
    std::string constants; // "name cN xK, ..." for the log

    struct Constant
    {
        std::string name;
        int set;   // 0 bool, 1 int4, 2 float4, 3 sampler
        int reg;
        int count;
    };
    std::vector<Constant> table;  // the whole constant table
    // Register of a named constant in a register set (2 = float4, 3 = sampler), or -1.
    int Reg(const char* name, int set) const
    {
        for (const auto& c : table)
            if (c.set == set && c.name == name)
                return c.reg;
        return -1;
    }
};

// Content hash (64-bit FNV-1a, hex) of a lockable texture's top mip level, as traces record it; "" if it can't be
// read (D3DPOOL_DEFAULT or an unknown format).
std::string HashTextureLevel0(IDirect3DTexture9* texture);

// Fills viewProj/worldBones/constants from the CTAB comment block of a D3D9 shader token stream.
void ParseConstantTable(const DWORD* function, size_t sizeBytes, ShaderInfo& info);

// Objects the device has created, so traces can name them.
struct Registry
{
    std::unordered_map<const void*, ShaderInfo> shaders;   // vertex and pixel shaders
    std::unordered_map<const void*, uint64_t> decls;       // vertex declarations -> element hash
};

class Trace
{
public:
    explicit Trace(const Registry& registry) : m_registry(registry) {}

    bool Active() const { return m_active; }
    const std::wstring& Dir() const { return m_dir; }
    void Begin(uint32_t frame, int frames, UINT vsRegisters);
    // Called after each Present; finishes the trace once its frames are done.
    void FrameDone();

    void Event(const std::string& json);  // one JSON object without the trailing newline
    void Draw(IDirect3DDevice9* dev, const char* call, const std::string& args);

    std::string SurfaceRef(IDirect3DSurface9* surface);
    std::string TextureRef(IDirect3DBaseTexture9* texture);

private:
    std::string ShaderRef(const void* shader) const;
    int ConstantBlock(std::vector<uint8_t>& store, std::unordered_map<uint64_t, int>& index, const float* data,
                      size_t bytes);
    void Finish();

    const Registry& m_registry;
    bool m_active = false;
    uint32_t m_firstFrame = 0;
    int m_framesLeft = 0;
    int m_draws = 0;
    UINT m_vsRegisters = 256;
    std::wstring m_dir;
    std::string m_lines;
    std::unordered_set<const void*> m_described;
    std::vector<uint8_t> m_vsConsts, m_psConsts;
    std::unordered_map<uint64_t, int> m_vsIndex, m_psIndex;
};

static const UINT kPsRegisters = 32;  // ps_2_x maximum; both games top out at ps_2_0
