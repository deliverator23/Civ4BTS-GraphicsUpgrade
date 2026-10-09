#include "trace.h"

#include <cstdio>
#include <cstring>

namespace
{
std::string Num(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string Floats(const float* v, int n)
{
    std::string s = "[";
    for (int i = 0; i < n; ++i)
        s += (i ? "," : "") + Num(v[i]);
    return s + "]";
}

// Bytes per row and number of rows of a mip level, for content hashing. False for formats we don't know.
bool Layout(D3DFORMAT format, UINT w, UINT h, UINT& rowBytes, UINT& rows)
{
    UINT bw = (w + 3) / 4 ? (w + 3) / 4 : 1, bh = (h + 3) / 4 ? (h + 3) / 4 : 1;
    switch (format)
    {
    case D3DFMT_DXT1:
        rowBytes = bw * 8, rows = bh;
        return true;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        rowBytes = bw * 16, rows = bh;
        return true;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:
    case D3DFMT_R32F:
        rowBytes = w * 4, rows = h;
        return true;
    case D3DFMT_R8G8B8:
        rowBytes = w * 3, rows = h;
        return true;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_L16:
    case D3DFMT_R16F:
        rowBytes = w * 2, rows = h;
        return true;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
        rowBytes = w, rows = h;
        return true;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
        rowBytes = w * 8, rows = h;
        return true;
    case D3DFMT_A32B32G32R32F:
        rowBytes = w * 16, rows = h;
        return true;
    default:
        return false;
    }
}

std::string HashLocked(const D3DLOCKED_RECT& lr, D3DFORMAT format, UINT w, UINT h)
{
    UINT rowBytes = 0, rows = 0;
    if (!Layout(format, w, h, rowBytes, rows))
        return "";
    uint64_t hash = 1469598103934665603ull;
    const uint8_t* p = static_cast<const uint8_t*>(lr.pBits);
    for (UINT r = 0; r < rows; ++r)
        hash = Fnv1a(p + static_cast<size_t>(r) * lr.Pitch, rowBytes, hash);
    return Hex64(hash);
}

struct NamedState
{
    const char* name;
    D3DRENDERSTATETYPE state;
};

const NamedState kRenderStates[] = {
    {"cull", D3DRS_CULLMODE},          {"z", D3DRS_ZENABLE},
    {"zwrite", D3DRS_ZWRITEENABLE},    {"zfunc", D3DRS_ZFUNC},
    {"blend", D3DRS_ALPHABLENDENABLE}, {"src", D3DRS_SRCBLEND},
    {"dst", D3DRS_DESTBLEND},          {"blendop", D3DRS_BLENDOP},
    {"atest", D3DRS_ALPHATESTENABLE},  {"aref", D3DRS_ALPHAREF},
    {"afunc", D3DRS_ALPHAFUNC},        {"clip", D3DRS_CLIPPLANEENABLE},
    {"fog", D3DRS_FOGENABLE},          {"srgbw", D3DRS_SRGBWRITEENABLE},
    {"cwrite", D3DRS_COLORWRITEENABLE}, {"stencil", D3DRS_STENCILENABLE},
    {"vblend", D3DRS_VERTEXBLEND},     {"ivblend", D3DRS_INDEXEDVERTEXBLENDENABLE},
    {"light", D3DRS_LIGHTING},         {"fill", D3DRS_FILLMODE},
    {"scissor", D3DRS_SCISSORTESTENABLE},
};
}

std::string HashTextureLevel0(IDirect3DTexture9* texture)
{
    D3DSURFACE_DESC d = {};
    if (!texture || FAILED(texture->GetLevelDesc(0, &d)) || d.Pool == D3DPOOL_DEFAULT)
        return "";
    D3DLOCKED_RECT lr;
    if (FAILED(texture->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
        return "";
    std::string hash = HashLocked(lr, d.Format, d.Width, d.Height);
    texture->UnlockRect(0);
    return hash;
}

void ParseConstantTable(const DWORD* function, size_t sizeBytes, ShaderInfo& info)
{
    // Comment tokens follow the version token; the constant table is the one tagged 'CTAB'. Inside it, offsets are
    // relative to the start of the D3DXSHADER_CONSTANTTABLE header (just after the tag).
    size_t count = sizeBytes / 4;
    for (size_t i = 1; i < count;)
    {
        DWORD token = function[i];
        if ((token & 0xFFFF) != 0xFFFE)
            break;  // comments come first; the first instruction ends the search
        size_t len = (token >> 16) & 0x7FFF;
        if (len >= 8 && i + 1 + len <= count && function[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B'))
        {
            const uint8_t* base = reinterpret_cast<const uint8_t*>(&function[i + 2]);
            size_t size = (len - 1) * 4;
            DWORD numConstants = *reinterpret_cast<const DWORD*>(base + 12);
            DWORD infoOffset = *reinterpret_cast<const DWORD*>(base + 16);
            for (DWORD c = 0; c < numConstants && infoOffset + (c + 1) * 20 <= size; ++c)
            {
                const uint8_t* ci = base + infoOffset + c * 20;
                DWORD nameOffset = *reinterpret_cast<const DWORD*>(ci);
                WORD set = *reinterpret_cast<const WORD*>(ci + 4);
                WORD reg = *reinterpret_cast<const WORD*>(ci + 6);
                WORD regs = *reinterpret_cast<const WORD*>(ci + 8);
                if (nameOffset >= size)
                    continue;
                std::string name(reinterpret_cast<const char*>(base + nameOffset),
                                 strnlen(reinterpret_cast<const char*>(base + nameOffset), size - nameOffset));
                static const char kSets[] = {'b', 'i', 'c', 's'};
                char buf[96];
                snprintf(buf, sizeof(buf), "%s%s %c%u x%u", info.constants.empty() ? "" : ", ", name.c_str(),
                         set < 4 ? kSets[set] : '?', reg, regs);
                info.constants += buf;
                info.table.push_back({name, set, reg, regs});
                if (set == 2 && name == "mtxWaterTextureMat")
                    info.btsWater = true;
                if (set == 2 && name == "mtxViewProj")
                    info.viewProj = reg;
                if (set == 2 && name == "mtxWorldBones")
                    info.worldBones = reg;
                if (set == 2 && name == "mtxWorld")
                    info.world = reg;
                if (set == 2 && (name == "mtxWaterTextureMat" || name == "mtxWaterTexture1"))
                    info.water = true;
            }
            info.btsTerrain = info.Reg("mtxLightmap", 2) >= 0 && info.Reg("fDetailTexScaling", 2) >= 0;
            info.tree = info.Reg("windir", 2) >= 0 && info.Reg("WorldViewProj", 2) >= 0 && info.Reg("World", 2) >= 0;
            struct LightNames
            {
                int group;
                const char *diffuse, *ambient, *dir;
            };
            static const LightNames kLightNames[3] = {
                {kLightUnit, "f3UnitLightDiffuse", "f3UnitAmbientColor", "f3UnitLightDir"},
                {kLightMech, "f3MechLightDiffuse", "f3MechAmbientColor", "f3MechLightDir"},
                {kLightRiver, "f3SunLightDiffuse", "f3SunAmbientColor", "f3SunLightDir"}};  // SunLight.nif: rivers
            for (const LightNames& n : kLightNames)
            {
                int diffuse = info.Reg(n.diffuse, 2), ambient = info.Reg(n.ambient, 2);
                if (info.lightGroup >= 0 || (diffuse < 0 && ambient < 0))
                    continue;
                info.lightGroup = n.group;
                info.lightDiffuse = diffuse;
                info.lightAmbient = ambient;
                info.lightDir = info.Reg(n.dir, 2);
            }
            return;
        }
        i += 1 + len;
    }
}

void Trace::Begin(uint32_t frame, int frames, UINT vsRegisters)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"%04d%02d%02d_%02d%02d%02d_f%u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, frame);
    m_dir = Log::GameDir() + L"\\traces\\" + name;
    EnsureDir(m_dir);
    m_active = true;
    m_firstFrame = frame;
    m_framesLeft = frames > 0 ? frames : 1;
    m_draws = 0;
    m_vsRegisters = vsRegisters;
    m_lines.clear();
    m_described.clear();
    m_vsConsts.clear();
    m_psConsts.clear();
    m_vsIndex.clear();
    m_psIndex.clear();
    Log::Write("trace started at frame %u (%d frame(s)) -> %ls", frame, m_framesLeft, m_dir.c_str());
}

void Trace::FrameDone()
{
    if (!m_active)
        return;
    Event("{\"ev\":\"present\"}");
    if (--m_framesLeft <= 0)
        Finish();
}

void Trace::Event(const std::string& json)
{
    if (!m_active)
        return;
    m_lines += json;
    m_lines += '\n';
}

std::string Trace::ShaderRef(const void* shader) const
{
    if (!shader)
        return "null";
    auto it = m_registry.shaders.find(shader);
    if (it == m_registry.shaders.end())
        return "\"unknown:" + Ptr(shader) + "\"";
    return "\"" + it->second.version + ":" + Hex64(it->second.hash) + "\"";
}

std::string Trace::SurfaceRef(IDirect3DSurface9* surface)
{
    if (!surface)
        return "null";
    if (m_active && m_described.insert(surface).second)
    {
        D3DSURFACE_DESC d = {};
        surface->GetDesc(&d);
        std::string container = "null";
        IDirect3DBaseTexture9* tex = nullptr;
        if (SUCCEEDED(surface->GetContainer(IID_IDirect3DBaseTexture9, reinterpret_cast<void**>(&tex))) && tex)
        {
            container = "\"" + Ptr(tex) + "\"";
            tex->Release();
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"ev\":\"surf\",\"ptr\":\"%s\",\"w\":%u,\"h\":%u,\"fmt\":%u,\"usage\":%u,\"pool\":%u,\"ms\":%u,"
                 "\"tex\":%s}",
                 Ptr(surface).c_str(), d.Width, d.Height, d.Format, d.Usage, d.Pool, d.MultiSampleType,
                 container.c_str());
        Event(buf);
    }
    return "\"" + Ptr(surface) + "\"";
}

std::string Trace::TextureRef(IDirect3DBaseTexture9* texture)
{
    if (!texture)
        return "null";
    if (m_active && m_described.insert(texture).second)
    {
        D3DRESOURCETYPE type = texture->GetType();
        D3DSURFACE_DESC d = {};
        std::string kind = "other", surf0 = "null", hash;
        if (type == D3DRTYPE_TEXTURE)
        {
            auto* t = static_cast<IDirect3DTexture9*>(texture);
            kind = "2d";
            t->GetLevelDesc(0, &d);
            IDirect3DSurface9* s = nullptr;
            if (SUCCEEDED(t->GetSurfaceLevel(0, &s)) && s)
            {
                surf0 = "\"" + Ptr(s) + "\"";
                s->Release();
            }
            D3DLOCKED_RECT lr;
            if (g_config.hashTextures && d.Pool != D3DPOOL_DEFAULT &&
                SUCCEEDED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                hash = HashLocked(lr, d.Format, d.Width, d.Height);
                t->UnlockRect(0);
            }
        }
        else if (type == D3DRTYPE_CUBETEXTURE)
        {
            auto* t = static_cast<IDirect3DCubeTexture9*>(texture);
            kind = "cube";
            t->GetLevelDesc(0, &d);
            D3DLOCKED_RECT lr;
            if (g_config.hashTextures && d.Pool != D3DPOOL_DEFAULT &&
                SUCCEEDED(t->LockRect(D3DCUBEMAP_FACE_POSITIVE_X, 0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                hash = HashLocked(lr, d.Format, d.Width, d.Height);
                t->UnlockRect(D3DCUBEMAP_FACE_POSITIVE_X, 0);
            }
        }
        else if (type == D3DRTYPE_VOLUMETEXTURE)
        {
            kind = "volume";
        }
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "{\"ev\":\"tex\",\"ptr\":\"%s\",\"type\":\"%s\",\"w\":%u,\"h\":%u,\"levels\":%u,\"fmt\":%u,"
                 "\"usage\":%u,\"pool\":%u,\"surf0\":%s,\"hash\":\"%s\"}",
                 Ptr(texture).c_str(), kind.c_str(), d.Width, d.Height, texture->GetLevelCount(), d.Format, d.Usage,
                 d.Pool, surf0.c_str(), hash.c_str());
        Event(buf);
    }
    return "\"" + Ptr(texture) + "\"";
}

int Trace::ConstantBlock(std::vector<uint8_t>& store, std::unordered_map<uint64_t, int>& index, const float* data,
                         size_t bytes)
{
    uint64_t h = Fnv1a(data, bytes);
    auto it = index.find(h);
    if (it != index.end())
        return it->second;
    int id = static_cast<int>(store.size() / bytes);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    store.insert(store.end(), p, p + bytes);
    index.emplace(h, id);
    return id;
}

void Trace::Draw(IDirect3DDevice9* dev, const char* call, const std::string& args)
{
    if (!m_active)
        return;
    std::string j = "{\"ev\":\"draw\",\"i\":" + std::to_string(m_draws++) + ",\"call\":\"" + call + "\"," + args;

    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    j += ",\"vs\":" + ShaderRef(vs) + ",\"ps\":" + ShaderRef(ps);

    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl)
    {
        auto it = m_registry.decls.find(decl);
        j += ",\"decl\":\"" + (it != m_registry.decls.end() ? Hex64(it->second) : "unknown:" + Ptr(decl)) + "\"";
        decl->Release();
    }
    DWORD fvf = 0;
    dev->GetFVF(&fvf);
    if (fvf)
        j += ",\"fvf\":" + std::to_string(fvf);

    j += ",\"streams\":[";
    bool first = true;
    for (UINT s = 0; s < 4; ++s)
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT offset = 0, stride = 0, freq = 1;
        if (SUCCEEDED(dev->GetStreamSource(s, &vb, &offset, &stride)) && vb)
        {
            dev->GetStreamSourceFreq(s, &freq);
            char buf[128];
            snprintf(buf, sizeof(buf), "%s{\"s\":%u,\"vb\":\"%s\",\"off\":%u,\"stride\":%u,\"freq\":%u}", first ? "" : ",",
                     s, Ptr(vb).c_str(), offset, stride, freq);
            j += buf;
            first = false;
            vb->Release();
        }
    }
    j += "]";
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->GetIndices(&ib)) && ib)
    {
        D3DINDEXBUFFER_DESC d = {};
        ib->GetDesc(&d);
        j += ",\"ib\":\"" + Ptr(ib) + "\",\"ibfmt\":" + std::to_string(d.Format);
        ib->Release();
    }

    j += ",\"rt\":[";
    for (DWORD i = 0; i < 4; ++i)
    {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(i, &s)) && s)
        {
            j += (i ? "," : "") + SurfaceRef(s);
            s->Release();
        }
        else if (i == 0)
            j += "null";
    }
    j += "]";
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds)
    {
        j += ",\"ds\":" + SurfaceRef(ds);
        ds->Release();
    }
    D3DVIEWPORT9 vp = {};
    dev->GetViewport(&vp);
    char vpbuf[128];
    snprintf(vpbuf, sizeof(vpbuf), ",\"vp\":[%u,%u,%u,%u,%s,%s]", vp.X, vp.Y, vp.Width, vp.Height,
             Num(vp.MinZ).c_str(), Num(vp.MaxZ).c_str());
    j += vpbuf;

    j += ",\"tex\":{";
    first = true;
    static const DWORD kStages[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                    D3DVERTEXTEXTURESAMPLER0, D3DVERTEXTEXTURESAMPLER1,
                                    D3DVERTEXTEXTURESAMPLER2, D3DVERTEXTEXTURESAMPLER3};
    for (DWORD stage : kStages)
    {
        IDirect3DBaseTexture9* t = nullptr;
        if (SUCCEEDED(dev->GetTexture(stage, &t)) && t)
        {
            DWORD srgb = 0, au = 0, av = 0, minf = 0, magf = 0, mipf = 0;
            dev->GetSamplerState(stage, D3DSAMP_SRGBTEXTURE, &srgb);
            dev->GetSamplerState(stage, D3DSAMP_ADDRESSU, &au);
            dev->GetSamplerState(stage, D3DSAMP_ADDRESSV, &av);
            dev->GetSamplerState(stage, D3DSAMP_MINFILTER, &minf);
            dev->GetSamplerState(stage, D3DSAMP_MAGFILTER, &magf);
            dev->GetSamplerState(stage, D3DSAMP_MIPFILTER, &mipf);
            char buf[160];
            snprintf(buf, sizeof(buf), "%s\"%u\":{\"t\":%s,\"srgb\":%u,\"addr\":[%u,%u],\"filt\":[%u,%u,%u]}",
                     first ? "" : ",", stage, TextureRef(t).c_str(), srgb, au, av, minf, magf, mipf);
            j += buf;
            first = false;
            t->Release();
        }
    }
    j += "}";

    j += ",\"rs\":{";
    first = true;
    for (const auto& rs : kRenderStates)
    {
        DWORD v = 0;
        dev->GetRenderState(rs.state, &v);
        j += std::string(first ? "" : ",") + "\"" + rs.name + "\":" + std::to_string(v);
        first = false;
    }
    j += "}";

    if (vs)
    {
        std::vector<float> c(m_vsRegisters * 4);
        if (SUCCEEDED(dev->GetVertexShaderConstantF(0, c.data(), m_vsRegisters)))
            j += ",\"vsc\":" + std::to_string(ConstantBlock(m_vsConsts, m_vsIndex, c.data(), c.size() * 4));
    }
    else
    {
        // Fixed-function vertex path: the transforms are what we need for the reflection pass.
        D3DMATRIX m;
        if (SUCCEEDED(dev->GetTransform(D3DTS_WORLD, &m)))
            j += ",\"world\":" + Floats(&m._11, 16);
        if (SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &m)))
            j += ",\"view\":" + Floats(&m._11, 16);
        if (SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &m)))
            j += ",\"proj\":" + Floats(&m._11, 16);
        DWORD blend = 0;
        dev->GetRenderState(D3DRS_VERTEXBLEND, &blend);
        if (blend != D3DVBF_DISABLE)
        {
            j += ",\"worlds\":[";
            for (int w = 1; w < 4; ++w)
            {
                dev->GetTransform(D3DTS_WORLDMATRIX(w), &m);
                j += (w > 1 ? "," : "") + Floats(&m._11, 16);
            }
            j += "]";
        }
        // Fixed-function lighting: the enabled lights (index, type, diffuse, ambient), material and global ambient.
        DWORD lighting = FALSE;
        dev->GetRenderState(D3DRS_LIGHTING, &lighting);
        if (lighting)
        {
            j += ",\"lights\":[";
            bool firstLight = true;
            for (DWORD i = 0; i < 64; ++i)
            {
                BOOL on = FALSE;
                D3DLIGHT9 l;
                if (FAILED(dev->GetLightEnable(i, &on)) || !on || FAILED(dev->GetLight(i, &l)))
                    continue;
                char buf[160];
                snprintf(buf, sizeof(buf), "%s{\"i\":%u,\"type\":%u,\"d\":[%g,%g,%g],\"a\":[%g,%g,%g]}",
                         firstLight ? "" : ",", i, l.Type, l.Diffuse.r, l.Diffuse.g, l.Diffuse.b, l.Ambient.r,
                         l.Ambient.g, l.Ambient.b);
                j += buf;
                firstLight = false;
            }
            D3DMATERIAL9 mat = {};
            DWORD ambient = 0;
            dev->GetMaterial(&mat);
            dev->GetRenderState(D3DRS_AMBIENT, &ambient);
            char buf[200];
            snprintf(buf, sizeof(buf), "],\"mat\":{\"d\":[%g,%g,%g],\"a\":[%g,%g,%g],\"e\":[%g,%g,%g]},\"amb\":\"%08x\"",
                     mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Ambient.r, mat.Ambient.g, mat.Ambient.b,
                     mat.Emissive.r, mat.Emissive.g, mat.Emissive.b, ambient);
            j += buf;
        }
    }
    if (ps)
    {
        float c[kPsRegisters * 4];
        if (SUCCEEDED(dev->GetPixelShaderConstantF(0, c, kPsRegisters)))
            j += ",\"psc\":" + std::to_string(ConstantBlock(m_psConsts, m_psIndex, c, sizeof(c)));
    }
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    j += "}";
    Event(j);
}

void Trace::Finish()
{
    m_active = false;
    WriteBytes(m_dir + L"\\trace.jsonl", m_lines.data(), m_lines.size());
    WriteBytes(m_dir + L"\\consts_vs.bin", m_vsConsts.data(), m_vsConsts.size());
    WriteBytes(m_dir + L"\\consts_ps.bin", m_psConsts.data(), m_psConsts.size());
    char meta[512];
    snprintf(meta, sizeof(meta),
             "{\"game\":\"%s\",\"first_frame\":%u,\"draws\":%d,\"vs_registers\":%u,\"ps_registers\":%u,"
             "\"vs_blocks\":%zu,\"ps_blocks\":%zu}\n",
             Log::GameId().c_str(), m_firstFrame, m_draws, m_vsRegisters, kPsRegisters, m_vsIndex.size(),
             m_psIndex.size());
    WriteBytes(m_dir + L"\\meta.json", meta, strlen(meta));
    Log::Write("trace written: %d draws, %zu vs / %zu ps constant blocks, %zu bytes of events", m_draws,
               m_vsIndex.size(), m_psIndex.size(), m_lines.size());
    MessageBeep(MB_ICONASTERISK);  // audible confirmation in game
    m_lines.clear();
    m_lines.shrink_to_fit();
}
