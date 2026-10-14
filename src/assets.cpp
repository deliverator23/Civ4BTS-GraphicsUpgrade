#include "assets.h"
#include "trace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    bool ok = size > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// FPK layout: u32 4, "FPK_", u8, u32 count, then per entry u32 name length, name bytes stored +1, padding to 4,
// u64 time, u32 size, u32 offset.
std::vector<FpkEntry> FpkEntries(const std::wstring& fpk)
{
    std::vector<FpkEntry> entries;
    FILE* f = nullptr;
    if (_wfopen_s(&f, fpk.c_str(), L"rb") || !f)
        return entries;
    uint8_t head[13];
    if (fread(head, 1, 13, f) == 13 && memcmp(head + 4, "FPK_", 4) == 0)
    {
        uint32_t count;
        memcpy(&count, head + 9, 4);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t len = 0;
            if (fread(&len, 4, 1, f) != 1 || len > 1024)
                break;
            FpkEntry e;
            e.name.resize(len);
            if (fread(&e.name[0], 1, len, f) != len)
                break;
            for (auto& c : e.name)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c - 1)));
            fseek(f, (4 - len % 4) % 4, SEEK_CUR);
            uint8_t rest[16];
            if (fread(rest, 1, 16, f) != 16)
                break;
            memcpy(&e.size, rest + 8, 4);
            memcpy(&e.offset, rest + 12, 4);
            entries.push_back(std::move(e));
        }
    }
    fclose(f);
    return entries;
}

bool ReadFpkEntry(const std::wstring& fpk, const FpkEntry& entry, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, fpk.c_str(), L"rb") || !f)
        return false;
    out.resize(entry.size);
    bool ok = fseek(f, static_cast<long>(entry.offset), SEEK_SET) == 0 && fread(out.data(), 1, entry.size, f) == entry.size;
    fclose(f);
    return ok;
}

bool ReadFromFpk(const std::wstring& fpk, const std::string& name, std::vector<uint8_t>& out)
{
    for (const FpkEntry& e : FpkEntries(fpk))
        if (e.name == name)
            return ReadFpkEntry(fpk, e, out);
    return false;
}

namespace
{
std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

bool IsHash(const std::wstring& s)
{
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswxdigit(c) != 0; });
}

bool HasDdsExtension(const std::string& name)
{
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".dds") == 0;
}

// The Assets folders the game reads art from, the running mod's first.
std::vector<std::wstring> AssetDirs()
{
    std::vector<std::wstring> dirs;
    if (!g_config.modDir.empty())
        dirs.push_back(g_config.modDir + L"\\Assets");
    const std::wstring bts = ProxyDir();
    dirs.push_back(bts + L"\\Assets");
    dirs.push_back(bts + L"\\..\\Warlords\\Assets");
    dirs.push_back(bts + L"\\..\\Assets");
    return dirs;
}

std::vector<std::wstring> FilesIn(const std::wstring& dir, const wchar_t* pattern)
{
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return files;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(dir + L"\\" + fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

// Little-endian values from a file in memory; reading past its end makes ok false (and gives zeros).
struct Reader
{
    const std::vector<uint8_t>& d;
    size_t at = 0;
    bool ok = true;

    void Bytes(void* out, size_t n)
    {
        if (!ok || n > d.size() - at)
        {
            ok = false;
            memset(out, 0, n);
            return;
        }
        memcpy(out, d.data() + at, n);
        at += n;
    }
    uint8_t U8()
    {
        uint8_t v;
        Bytes(&v, 1);
        return v;
    }
    uint16_t U16()
    {
        uint16_t v;
        Bytes(&v, 2);
        return v;
    }
    uint32_t U32()
    {
        uint32_t v;
        Bytes(&v, 4);
        return v;
    }
    float F32()
    {
        float v;
        Bytes(&v, 4);
        return v;
    }
    void Skip(size_t n)
    {
        if (!ok || n > d.size() - at)
            ok = false;
        else
            at += n;
    }
    std::string SizedString()  // u32 length, then the characters
    {
        uint32_t n = U32();
        if (n > d.size() - at)
        {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(d.data() + at), n);
        at += n;
        return s;
    }
    std::vector<uint32_t> Refs()  // u32 count, then that many block references
    {
        uint32_t n = U32();
        std::vector<uint32_t> refs;
        if (n > (d.size() - at) / 4)
            ok = false;
        else
            for (uint32_t i = 0; i < n; ++i)
                refs.push_back(U32());
        return refs;
    }
};
}

bool ReadAsset(const std::wstring& name, std::vector<uint8_t>& out, std::wstring& from, bool modOnly)
{
    std::wstring path = name;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::string packed;
    for (wchar_t c : path)
        packed += static_cast<char>(towlower(c));
    std::vector<std::wstring> dirs = AssetDirs();
    if (modOnly)
        dirs.resize(g_config.modDir.empty() ? 0 : 1);
    for (const std::wstring& dir : dirs)
    {
        if (ReadFileBytes(dir + L"\\" + path, out))
        {
            from = dir + L"\\" + path;
            return true;
        }
        for (const std::wstring& fpk : FilesIn(dir, L"*.fpk"))
            if (ReadFromFpk(fpk, packed, out))
            {
                from = fpk;
                return true;
            }
    }
    return false;
}

bool ReadNifLights(const std::vector<uint8_t>& nif, float sun[3], float ambient[3], float direction[3])
{
    // Header: a text line, the version, (20.0.0.3 on) the byte order, the user version, the blocks' types.
    Reader r{nif};
    const uint8_t* end = static_cast<const uint8_t*>(memchr(nif.data(), '\n', (std::min)(nif.size(), size_t(64))));
    if (!end || nif.size() < 20 || memcmp(nif.data(), "Gamebryo File Format", 20) != 0)
        return false;
    r.at = end - nif.data() + 1;
    const uint32_t version = r.U32();
    if (version < 0x0A010000 || version > 0x14000005)
        return false;
    if (version >= 0x14000003 && r.U8() != 1)  // big-endian
        return false;
    if (r.U32() >= 10)  // user version: 10 and up are Bethesda's, with more header fields
        return false;
    const uint32_t blockCount = r.U32();
    std::vector<std::string> types(r.U16());
    for (std::string& type : types)
        type = r.SizedString();
    std::vector<uint16_t> blockTypes(blockCount);
    for (uint16_t& t : blockTypes)
        t = r.U16();
    for (uint32_t groups = r.U32(); groups && r.ok; --groups)
        r.U32();

    // The blocks: only what a light rig has.
    struct Block
    {
        float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major; applied as R x v
        int parent = -1;
        bool directional = false, light = false;
        float dimmer = 0, ambient[3] = {}, diffuse[3] = {};
    };
    std::vector<Block> blocks(blockCount);
    for (uint32_t b = 0; b < blockCount && r.ok; ++b)
    {
        if (version <= 0x0A01006A)
            r.U32();  // a zero before each block
        if (blockTypes[b] >= types.size())
            return false;
        const std::string& type = types[blockTypes[b]];
        // NiObjectNET: name, extra data, controller
        r.SizedString();
        r.Refs();
        r.U32();
        Block& block = blocks[b];
        const bool node = type == "NiNode";
        block.directional = type == "NiDirectionalLight";
        block.light = block.directional || type == "NiAmbientLight";
        if (node || block.light)
        {
            // NiAVObject: flags, translation, rotation, scale, properties, collision object
            r.U16();
            r.Skip(12);
            for (float& m : block.rotation)
                m = r.F32();
            r.F32();
            r.Refs();
            r.U32();
            if (node)
            {
                for (uint32_t child : r.Refs())
                    if (child < blockCount)
                        blocks[child].parent = static_cast<int>(b);
                r.Refs();  // effects
            }
            else
            {
                // NiDynamicEffect: switch state (10.1.0.106 on), affected nodes; NiLight: dimmer, colours
                if (version >= 0x0A01006A)
                    r.U8();
                r.Refs();
                block.dimmer = r.F32();
                for (float& c : block.ambient)
                    c = r.F32();
                for (float& c : block.diffuse)
                    c = r.F32();
                r.Skip(12);  // specular
            }
        }
        else if (type == "NiZBufferProperty")
            r.U16(), r.U32();  // flags, function
        else if (type == "NiVertexColorProperty")
            r.U16(), r.U32(), r.U32();  // flags, vertex mode, lighting mode
        else
            return false;
    }
    // Footer: the roots. Ending exactly there confirms the blocks were read right.
    r.Refs();
    if (!r.ok || r.at != nif.size())
        return false;

    int sunBlock = -1;
    for (int c = 0; c < 3; ++c)
        ambient[c] = 0;
    for (uint32_t b = 0; b < blockCount; ++b)
    {
        if (!blocks[b].light)
            continue;
        for (int c = 0; c < 3; ++c)
            ambient[c] += blocks[b].ambient[c] * blocks[b].dimmer;
        if (blocks[b].directional && sunBlock < 0)
            sunBlock = static_cast<int>(b);
    }
    if (sunBlock < 0)
        return false;
    for (int c = 0; c < 3; ++c)
        sun[c] = blocks[sunBlock].diffuse[c] * blocks[sunBlock].dimmer;
    // A directional light shines along its x axis: the first column of its rotation in the scene (each node's
    // rotation applied after its children's).
    float d[3] = {blocks[sunBlock].rotation[0], blocks[sunBlock].rotation[3], blocks[sunBlock].rotation[6]};
    int depth = 0;
    for (int p = blocks[sunBlock].parent; p >= 0 && depth < static_cast<int>(blockCount); p = blocks[p].parent, ++depth)
    {
        const float* m = blocks[p].rotation;
        const float v[3] = {d[0], d[1], d[2]};
        for (int i = 0; i < 3; ++i)
            d[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
    }
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < 1e-6f)
        return false;
    for (int i = 0; i < 3; ++i)
        direction[i] = d[i] / length;
    return true;
}

void TextureList::Load(const std::wstring& entries, const char* setting)
{
    m_hashes.clear();
    m_sizes.clear();
    m_anySize = false;
    m_seen.clear();
    std::unordered_map<std::wstring, std::vector<FpkEntry>> fpks;  // each archive's index, read once
    size_t start = 0;
    while (start <= entries.size())
    {
        size_t end = entries.find(L';', start);
        std::wstring entry = Trim(entries.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        start = end == std::wstring::npos ? entries.size() + 1 : end + 1;
        std::replace(entry.begin(), entry.end(), L'/', L'\\');
        while (!entry.empty() && entry.back() == L'\\')
            entry.pop_back();
        if (entry.empty())
            continue;
        if (IsHash(entry))
        {
            std::string hash;
            for (wchar_t c : entry)
                hash += static_cast<char>(towlower(c));
            m_hashes.insert(hash);
            m_anySize = true;
            continue;
        }

        int found = 0;
        std::vector<uint8_t> bytes;
        auto loose = [&](const std::wstring& path) {
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES)
                return;
            std::vector<std::wstring> files = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? FilesIn(path, L"*.dds")
                                                                                  : std::vector<std::wstring>{path};
            for (const std::wstring& file : files)
                found += ReadFileBytes(file, bytes) && AddDds(bytes);
        };
        if (entry.find(L':') != std::wstring::npos)
            loose(entry);
        else
        {
            std::string name;
            for (wchar_t c : entry)
                name += static_cast<char>(towlower(c));
            for (const std::wstring& dir : AssetDirs())
            {
                loose(dir + L"\\" + entry);
                for (const std::wstring& fpk : FilesIn(dir, L"*.fpk"))
                {
                    auto it = fpks.find(fpk);
                    if (it == fpks.end())
                        it = fpks.emplace(fpk, FpkEntries(fpk)).first;
                    for (const FpkEntry& e : it->second)
                    {
                        // The file itself, or a .dds directly in the folder.
                        const bool inFolder = e.name.size() > name.size() + 1 && e.name.compare(0, name.size(), name) == 0 &&
                                              e.name[name.size()] == '\\' &&
                                              e.name.find('\\', name.size() + 1) == std::string::npos && HasDdsExtension(e.name);
                        if ((e.name == name || inFolder) && ReadFpkEntry(fpk, e, bytes))
                            found += AddDds(bytes);
                    }
                }
            }
        }
        if (found)
            Log::Write("settings: %s: %ls: %d texture%s", setting, entry.c_str(), found, found == 1 ? "" : "s");
        else
            Log::Write("settings: %s: %ls: no .dds textures found", setting, entry.c_str());
    }
}

bool TextureList::AddDds(const std::vector<uint8_t>& d)
{
    auto u32 = [&](size_t at) {
        uint32_t v;
        memcpy(&v, d.data() + at, 4);
        return v;
    };
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0)
        return false;
    uint32_t h = u32(12), w = u32(16), mips = (std::max)(u32(28), 1u);
    const uint32_t pfFlags = u32(80), bits = u32(88), caps2 = u32(112);
    const std::string fourCC(reinterpret_cast<const char*>(d.data() + 84), 4);
    if (caps2 & (0x200 | 0x200000))  // cube map or volume
        return false;
    uint32_t block = 0;
    if (pfFlags & 0x4)
    {
        if (fourCC == "DXT1")
            block = 8;
        else if (fourCC == "DXT2" || fourCC == "DXT3" || fourCC == "DXT4" || fourCC == "DXT5")
            block = 16;
        else
            return false;
    }
    else if (bits == 0 || bits % 8)
        return false;
    // A texture's hash covers its top level's rows (HashTextureLevel0); in a file they're stored back to back.
    size_t at = 128;
    for (uint32_t m = 0; m < mips; ++m)
    {
        const size_t size = block ? static_cast<size_t>((w + 3) / 4) * block * ((h + 3) / 4)
                                  : static_cast<size_t>(w) * (bits / 8) * h;
        if (at + size > d.size())
            break;
        m_hashes.insert(Hex64(Fnv1a(d.data() + at, size)));
        m_sizes.insert(w << 16 | h);
        at += size;
        w = (std::max)(w / 2, 1u);
        h = (std::max)(h / 2, 1u);
    }
    return true;
}

bool TextureList::Contains(IDirect3DBaseTexture9* base)
{
    if (m_hashes.empty() || !base || base->GetType() != D3DRTYPE_TEXTURE)
        return false;
    auto it = m_seen.find(base);
    if (it != m_seen.end())
        return it->second;
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC d = {};
    if (FAILED(tex->GetLevelDesc(0, &d)) || (!m_anySize && !m_sizes.count(d.Width << 16 | d.Height)))
        return false;  // cheap to check again; not worth caching
    const bool listed = m_hashes.count(HashTextureLevel0(tex)) != 0;
    m_seen.emplace(base, listed);
    return listed;
}
