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
        if (!Texture(*s, "detail normals"))
            continue;
        use.normalMap = s->texture;
        break;
    }
    m_cache.emplace(detail, use);
    return use;
}

DetailNormals::SheetUse DetailNormals::ForBlend(IDirect3DBaseTexture9* sheet)
{
    SheetUse use;
    if (!sheet || sheet->GetType() != D3DRTYPE_TEXTURE)
        return use;
    auto it = m_blendCache.find(sheet);
    if (it != m_blendCache.end())
        return it->second;
    if (!m_loaded)
        Load();
    for (auto& s : m_blendShipped)
        if (!s->failed && s->match.Contains(sheet) && (use.map = Texture(*s, "blend normals")) != nullptr)
        {
            use.dxt5nm = s->dxt5nm;
            break;
        }
    m_blendCache.emplace(sheet, use);
    return use;
}

IDirect3DTexture9* DetailNormals::Texture(Shipped& s, const char* what)
{
    if (s.texture || s.failed)
        return s.texture;
    // d3dx9_33.dll ships with the game (the water loads its textures the same way).
    HMODULE d3dx = GetModuleHandleW(L"d3dx9_33.dll");
    if (!d3dx)
        d3dx = LoadLibraryW(L"d3dx9_33.dll");
    using TexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DTexture9**);
    auto texFromMem = d3dx ? reinterpret_cast<TexFn>(GetProcAddress(d3dx, "D3DXCreateTextureFromFileInMemory")) : nullptr;
    const ULONG before = DeviceRefs(m_dev);
    if (!texFromMem || FAILED(texFromMem(m_dev, s.dds.data(), static_cast<UINT>(s.dds.size()), &s.texture)))
    {
        s.failed = true;
        s.texture = nullptr;
        Log::Write("%s: %ls can't be loaded; %ls goes without", what, s.normal.c_str(), s.detail.c_str());
        return nullptr;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    s.dds.clear();
    s.dds.shrink_to_fit();
    Log::Write("%s: %ls in use for %ls", what, s.normal.c_str(), s.detail.c_str());
    return s.texture;
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

    // Blend sheets: each terrain's <Path>, with its <Grid> (the same layout, shown with the grid on).
    auto tag = [](const std::string& block, const char* name) {
        const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
        const size_t a = block.find(open);
        const size_t b = a == std::string::npos ? std::string::npos : block.find(close, a);
        if (b == std::string::npos)
            return std::string();
        std::string v = block.substr(a + open.size(), b - a - open.size());
        v.erase(0, v.find_first_not_of(" \t\r\n"));
        v.erase(v.find_last_not_of(" \t\r\n") + 1);
        std::replace(v.begin(), v.end(), '/', '\\');
        return v;
    };
    size_t sheets = 0;
    for (size_t at = text.find("<TerrainArtInfo>"); at != std::string::npos; at = text.find("<TerrainArtInfo>", at))
    {
        const size_t end = text.find("</TerrainArtInfo>", at);
        if (end == std::string::npos)
            break;
        const std::string block = text.substr(at, end - at);
        at = end;
        const std::string path = tag(block, "Path"), grid = tag(block, "Grid");
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(tolower(c)); });
        if (lower.size() <= 4 || lower.compare(lower.size() - 4, 4, ".dds") != 0)
            continue;
        ++sheets;
        auto s = std::make_unique<Shipped>();
        s->detail = Widen(path);
        s->normal = s->detail.substr(0, s->detail.size() - 4) + L"_normal.dds";
        std::wstring normalFrom;
        if (!ReadAsset(s->normal, s->dds, normalFrom))
            continue;
        s->match.Load(grid.empty() ? s->detail : s->detail + L";" + Widen(grid), "blend normals");
        s->dxt5nm = IsDxt5nm(s->dds);
        Log::Write("blend normals: %ls has a normal map (%ls%s)", s->detail.c_str(), normalFrom.c_str(),
                   s->dxt5nm ? ", DXT5nm" : "");
        m_blendShipped.push_back(std::move(s));
    }
    Log::Write("blend normals: %zu terrain blend sheets, %zu with their own normal map", sheets, m_blendShipped.size());
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
    for (auto& s : m_blendShipped)
        if (s->texture)
        {
            s->texture->Release();
            s->texture = nullptr;
        }
    m_blendShipped.clear();
    m_blendCache.clear();
    m_loaded = false;
    m_internalRefs = 0;
}
