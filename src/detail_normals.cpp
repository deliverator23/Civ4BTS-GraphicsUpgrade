#include "detail_normals.h"

#include <algorithm>

DetailNormals::Use DetailNormals::For(IDirect3DBaseTexture9* detail)
{
    Use use;
    if (!detail || detail->GetType() != D3DRTYPE_TEXTURE)
        return use;
    auto it = m_cache.find(detail);
    if (it != m_cache.end())
        return it->second;
    if (!m_loaded)
        Load();
    D3DSURFACE_DESC d = {};
    if (SUCCEEDED(static_cast<IDirect3DTexture9*>(detail)->GetLevelDesc(0, &d)) && d.Width && d.Height)
        use.texel[0] = 1.0f / d.Width, use.texel[1] = 1.0f / d.Height;
    for (auto& s : m_shipped)
    {
        if (s->failed || !s->match.Contains(detail))
            continue;
        if (!s->texture)
        {
            // d3dx9_33.dll ships with the game (the water loads its textures the same way).
            HMODULE d3dx = GetModuleHandleW(L"d3dx9_33.dll");
            if (!d3dx)
                d3dx = LoadLibraryW(L"d3dx9_33.dll");
            using TexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DTexture9**);
            auto texFromMem =
                d3dx ? reinterpret_cast<TexFn>(GetProcAddress(d3dx, "D3DXCreateTextureFromFileInMemory")) : nullptr;
            const ULONG before = DeviceRefs(m_dev);
            if (!texFromMem ||
                FAILED(texFromMem(m_dev, s->dds.data(), static_cast<UINT>(s->dds.size()), &s->texture)))
            {
                s->failed = true;
                s->texture = nullptr;
                Log::Write("detail normals: %ls can't be loaded; %ls gets normals from its brightness",
                           s->normal.c_str(), s->detail.c_str());
                continue;
            }
            m_internalRefs += DeviceRefs(m_dev) - before;
            s->dds.clear();
            s->dds.shrink_to_fit();
            Log::Write("detail normals: %ls in use for %ls", s->normal.c_str(), s->detail.c_str());
        }
        use.normalMap = s->texture;
        break;
    }
    m_cache.emplace(detail, use);
    return use;
}

void DetailNormals::Load()
{
    m_loaded = true;
    // The terrains' detail textures, from the art defines the game uses (the running mod's, if it has its own).
    std::vector<uint8_t> xml;
    std::wstring from;
    if (!ReadAsset(L"XML\\Art\\CIV4ArtDefines_Terrain.xml", xml, from))
    {
        Log::Write("detail normals: CIV4ArtDefines_Terrain.xml not found; normals come from the detail textures");
        return;
    }
    const std::string text(xml.begin(), xml.end());
    std::vector<std::string> details;
    for (size_t at = text.find("<Detail>"); at != std::string::npos; at = text.find("<Detail>", at))
    {
        at += 8;
        const size_t end = text.find("</Detail>", at);
        if (end == std::string::npos)
            break;
        std::string path = text.substr(at, end - at);
        path.erase(0, path.find_first_not_of(" \t\r\n"));
        path.erase(path.find_last_not_of(" \t\r\n") + 1);
        std::replace(path.begin(), path.end(), '/', '\\');
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(tolower(c)); });
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0 &&
            std::find(details.begin(), details.end(), lower) == details.end())
            details.push_back(lower);
    }
    size_t shipped = 0;
    for (const std::string& detail : details)
    {
        auto s = std::make_unique<Shipped>();
        s->detail = Widen(detail);
        s->normal = s->detail.substr(0, s->detail.size() - 4) + L"_normal.dds";
        std::wstring normalFrom;
        if (!ReadAsset(s->normal, s->dds, normalFrom))
            continue;
        s->match.Load(s->detail, "detail normals");
        m_shipped.push_back(std::move(s));
        ++shipped;
        Log::Write("detail normals: %ls has a normal map (%ls)", m_shipped.back()->detail.c_str(), normalFrom.c_str());
    }
    Log::Write("detail normals: %zu detail textures in %ls, %zu with their own normal map; the rest get normals from "
               "their brightness",
               details.size(), from.c_str(), shipped);
}

void DetailNormals::ReleaseAll()
{
    for (auto& s : m_shipped)
        if (s->texture)
        {
            s->texture->Release();
            s->texture = nullptr;
        }
    m_shipped.clear();
    m_cache.clear();
    m_loaded = false;
    m_internalRefs = 0;
}
