// Reading the games' art (loose files and FPK archives), and sets of textures named in the settings.
#pragma once
#include "common.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out);

// The files in a Civ4 FPK archive. Names are lower case with backslashes.
struct FpkEntry
{
    std::string name;
    uint32_t size = 0, offset = 0;
};
std::vector<FpkEntry> FpkEntries(const std::wstring& fpk);
bool ReadFpkEntry(const std::wstring& fpk, const FpkEntry& entry, std::vector<uint8_t>& out);
bool ReadFromFpk(const std::wstring& fpk, const std::string& name, std::vector<uint8_t>& out);

// Textures named in the settings, recognised in game by their content. Each entry (separated by ';') is a .dds file
// or a folder of them, as a path under Assets (looked up in the running mod, Beyond the Sword, Warlords and
// Civilization IV, loose or in their FPK archives) or a full path, or a texture hash from a frame trace. Every mip
// level of a file counts, so lower texture detail settings still match.
class TextureList
{
public:
    void Load(const std::wstring& entries, const char* setting);  // logs what each entry found
    bool Contains(IDirect3DBaseTexture9* texture);              // cached per texture
    void Forget(const void* texture) { m_seen.erase(texture); }
    void Clear() { m_seen.clear(); }

private:
    bool AddDds(const std::vector<uint8_t>& dds);

    std::unordered_set<std::string> m_hashes;
    std::unordered_set<uint32_t> m_sizes;  // width << 16 | height of each listed level: only those are hashed
    bool m_anySize = false;                // a bare hash was listed: any size may match
    std::unordered_map<const void*, bool> m_seen;
};
