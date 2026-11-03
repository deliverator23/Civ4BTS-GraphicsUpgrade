// Config loading, logging and small helpers.
#include "common.h"
#include "assets.h"

#include <share.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <vector>

Config g_config;
HMODULE g_self = nullptr;
const char* volatile g_lastCall = "none";
volatile DWORD g_lastCallTick = 0;

// Background thread: if the game makes no Direct3D call for 5 s, log the last one it made (once per stall). A hang
// inside or just after a d3d call then shows up in the log, where first-call logging alone stays silent.
static DWORD WINAPI WatchdogThread(LPVOID)
{
    const char* reported = nullptr;
    DWORD reportedTick = 0;
    for (;;)
    {
        Sleep(1000);
        DWORD tick = g_lastCallTick;
        const char* call = g_lastCall;
        if (!tick || GetTickCount() - tick < 5000)
            continue;
        if (call == reported && tick == reportedTick)
            continue;
        reported = call;
        reportedTick = tick;
        Log::Write("watchdog: no Direct3D calls for %lu s; last call was %s", (GetTickCount() - tick) / 1000, call);
    }
}

void StartWatchdog()
{
    HANDLE thread = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

static std::wstring ModuleDir(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// A running mod's GraphicsUpgrade.ini (empty: none). Every setting is looked up there first: a setting present in it
// (even empty) wins over the game folder's.
static std::wstring g_modIni;

// The value of a setting in one INI file, or false if the file doesn't have it.
static bool IniRaw(const std::wstring& ini, const wchar_t* section, const wchar_t* key, std::wstring& out)
{
    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(section, key, L"\x01", buf, 4096, ini.c_str());
    if (wcscmp(buf, L"\x01") == 0)
        return false;
    out = buf;
    return true;
}

static std::wstring IniString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    std::wstring value;
    if ((!g_modIni.empty() && IniRaw(g_modIni, section, key, value)) || IniRaw(ini, section, key, value))
        return value;
    return def;
}

static int IniInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def)
{
    std::wstring s = IniString(ini, section, key, L"");
    return s.empty() ? def : static_cast<int>(wcstol(s.c_str(), nullptr, 0));  // base 0 accepts 0x7A
}

// Problems found while reading the INI; written to the log once it's open (Log::Init).
static std::vector<std::string> g_settingsWarnings;

// A setting that 1.0's INIs wrote under another name or section. Each file is read in turn (the mod's first, as
// IniString): its new key if set, else its old one, else an empty new key still counts as set (no value). The old key
// is read only while the file sets none of `blockers` (the other new keys that replaced it). Returns the value, and
// sets `old` when it came from the old key.
static std::wstring IniRenamed(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                               const wchar_t* oldSection, const wchar_t* oldKey, bool& old,
                               std::initializer_list<const wchar_t*> blockers = {})
{
    old = false;
    const std::wstring* const files[] = {&g_modIni, &ini};
    for (const std::wstring* file : files)
    {
        if (file->empty())
            continue;
        std::wstring value, oldValue;
        const bool have = IniRaw(*file, section, key, value);
        if (!value.empty())
            return value;
        bool blocked = false;
        for (const wchar_t* b : blockers)
            blocked = blocked || (IniRaw(*file, section, b, oldValue) && !oldValue.empty());
        if (!blocked && IniRaw(*file, oldSection, oldKey, oldValue) && !oldValue.empty())
        {
            old = true;
            return oldValue;
        }
        if (have)
            return value;
    }
    return L"";
}

// Every setting line of a section, key and value trimmed, in file order. (GetPrivateProfileString gives only a key's
// first line.)
static std::vector<std::pair<std::wstring, std::wstring>> IniSection(const std::wstring& ini, const wchar_t* section)
{
    auto trim = [](const std::wstring& t) {
        size_t a = t.find_first_not_of(L" \t"), b = t.find_last_not_of(L" \t");
        return a == std::wstring::npos ? std::wstring() : t.substr(a, b - a + 1);
    };
    std::vector<wchar_t> buf(65536);
    const DWORD n = GetPrivateProfileSectionW(section, buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    std::vector<std::pair<std::wstring, std::wstring>> lines;
    for (const wchar_t* line = buf.data(); line < buf.data() + n && *line; line += wcslen(line) + 1)
    {
        const std::wstring text = line;
        const size_t eq = text.find(L'=');
        if (eq != std::wstring::npos && text[0] != L';' && text[0] != L'#')
            lines.emplace_back(trim(text.substr(0, eq)), trim(text.substr(eq + 1)));
    }
    return lines;
}

// Settings that are lists of entries separated by ';' (TextureList): every line of the key counts, so a second
// SurfaceTextures= line adds to the first instead of being ignored.
static bool IsListSetting(const std::wstring& key)
{
    for (const wchar_t* name : {L"SurfaceTextures", L"NoShadowTextures", L"BlobShadowTexture"})
        if (_wcsicmp(key.c_str(), name) == 0)
            return true;
    return false;
}

// A list setting: the entries of every line of the key, joined with ';', from the mod's INI if it has the key at all
// (as IniString), else the game folder's.
static std::wstring IniList(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    const std::wstring* const files[] = {&g_modIni, &ini};
    for (const std::wstring* file : files)
    {
        if (file->empty())
            continue;
        std::wstring entries;
        bool found = false;
        for (const auto& line : IniSection(*file, section))
            if (_wcsicmp(line.first.c_str(), key) == 0)
            {
                found = true;
                if (!line.second.empty())
                    entries += (entries.empty() ? L"" : L"; ") + line.second;
            }
        if (found)
            return entries;
    }
    return def;
}

// A setting written more than once in one file: say so in the log. Of an ordinary setting only the first line
// counts; of a list setting, all of them.
static void WarnDuplicates(const std::wstring& file)
{
    std::vector<wchar_t> names(16384);
    const DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), file.c_str());
    for (const wchar_t* section = names.data(); section < names.data() + n && *section; section += wcslen(section) + 1)
    {
        std::map<std::wstring, std::pair<std::wstring, int>> seen;  // lower-case key -> as written, how many lines
        for (const auto& line : IniSection(file, section))
        {
            std::wstring lower = line.first;
            for (wchar_t& c : lower)
                c = static_cast<wchar_t>(towlower(c));
            auto& entry = seen[lower];
            if (entry.second++ == 0)
                entry.first = line.first;
        }
        for (const auto& kv : seen)
        {
            if (kv.second.second < 2)
                continue;
            char msg[600];
            snprintf(msg, sizeof(msg), "settings: [%ls] %ls is set on %d lines in %ls; %s", section,
                     kv.second.first.c_str(), kv.second.second, file.c_str(),
                     IsListSetting(kv.second.first) ? "the entries of all of them are used"
                                                    : "only the first counts, so remove the others");
            g_settingsWarnings.push_back(msg);
        }
    }
}

// A hotkey as written in the INI: a letter or digit (W, 5), a key name (F11, PrintScreen, Pause, Space, Tab,
// Insert, Delete, Home, End, PageUp, PageDown, Numpad0-Numpad9), a virtual-key code (0x57 or 87), or none / empty / 0
// for no hotkey. Returns the virtual-key code (0 = none); unrecognised text logs a warning and counts as none.
static UINT ParseKey(const std::wstring& raw, const wchar_t* section, const wchar_t* key)
{
    std::wstring s;
    for (wchar_t c : raw)
        if (!iswspace(c))
            s += static_cast<wchar_t>(towupper(c));
    if (s.empty() || s == L"NONE" || s == L"0")
        return 0;
    if (s.size() == 1 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'0' && s[0] <= L'9')))
        return s[0];  // letters and digits are their own virtual-key codes
    if (s.size() == 1 && iswpunct(s[0]))
    {
        SHORT vk = VkKeyScanW(s[0]);  // the key that types it on this keyboard (/ is VK_OEM_2 on a US one)
        if (vk != -1)
            return static_cast<UINT>(vk & 0xFF);
    }
    if (s.size() >= 2 && s[0] == L'F' && iswdigit(s[1]))
    {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    if (s.compare(0, 6, L"NUMPAD") == 0 && s.size() == 7 && iswdigit(s[6]))
        return VK_NUMPAD0 + (s[6] - L'0');
    static const struct
    {
        const wchar_t* name;
        UINT vk;
    } kNames[] = {{L"PRINTSCREEN", VK_SNAPSHOT}, {L"PAUSE", VK_PAUSE}, {L"SPACE", VK_SPACE},   {L"TAB", VK_TAB},
                  {L"INSERT", VK_INSERT},        {L"DELETE", VK_DELETE}, {L"HOME", VK_HOME}, {L"END", VK_END},
                  {L"PAGEUP", VK_PRIOR},         {L"PAGEDOWN", VK_NEXT}, {L"SCROLLLOCK", VK_SCROLL}};
    for (const auto& k : kNames)
        if (s == k.name)
            return k.vk;
    wchar_t* end = nullptr;
    unsigned long code = wcstoul(s.c_str(), &end, 0);  // 0x57 or 87
    if (end && *end == 0 && code > 0 && code < 256)
        return static_cast<UINT>(code);
    char msg[256];
    snprintf(msg, sizeof(msg), "settings: [%ls] %ls=%ls isn't a key this understands; no hotkey", section, key,
             raw.c_str());
    g_settingsWarnings.push_back(msg);  // the log isn't open yet while settings are read
    return 0;
}

std::string KeyName(UINT vk)
{
    if (vk == 0)
        return "none";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    switch (vk)
    {
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_PAUSE: return "Pause";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_SCROLL: return "ScrollLock";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static UINT IniKey(const std::wstring& ini, const wchar_t* section, const wchar_t* key, UINT def)
{
    std::wstring value;
    if ((g_modIni.empty() || !IniRaw(g_modIni, section, key, value)) && !IniRaw(ini, section, key, value))
        return def;  // not in either INI: the default key (an empty value means no hotkey)
    return ParseKey(value, section, key);
}

// A light cycle's keyframes, sorted by step: colours step,R,G,B;... (0..255), directions step,x,y,z;... (the way the
// light travels; z below 0, so it shines down) or sun angles step,azimuth,elevation;... (degrees; elevation above 0).
// Bad entries are warned about and skipped.
enum CycleKind
{
    kCycleColour,
    kCycleDirection,
    kCycleAngles,
};
static std::vector<Config::LightCycleKey> CycleKeys(const std::wstring& ini, const wchar_t* key, float stepCount,
                                                    CycleKind kind = kCycleColour)
{
    const bool direction = kind == kCycleDirection, angles = kind == kCycleAngles;
    std::vector<Config::LightCycleKey> keys;
    std::wstring steps = IniString(ini, L"lighting", key, L"");
    for (size_t start = 0; start < steps.size();)
    {
        size_t end = steps.find(L';', start);
        std::wstring item = steps.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        start = end == std::wstring::npos ? steps.size() : end + 1;
        if (item.find_first_not_of(L" \t") == std::wstring::npos)
            continue;
        float step, v[3] = {};
        bool ok = (angles ? swscanf_s(item.c_str(), L"%f,%f,%f", &step, &v[0], &v[1]) == 3
                          : swscanf_s(item.c_str(), L"%f,%f,%f,%f", &step, &v[0], &v[1], &v[2]) == 4) &&
                  step >= 0 && step <= stepCount;
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (ok && direction)
            ok = v[2] < -0.05f * len;  // shining down
        if (ok && angles)
            ok = v[1] > 0 && v[1] <= 90;  // above the horizon
        if (!ok)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "settings: [lighting] %ls entry \"%ls\" isn't %s within 0..%g; skipped", key,
                     item.c_str(),
                     direction ? "step,x,y,z (z below 0)"
                     : angles  ? "step,azimuth,elevation (elevation 0..90)"
                               : "step,R,G,B",
                     stepCount);
            g_settingsWarnings.push_back(msg);
            continue;
        }
        Config::LightCycleKey k;
        k.step = step;
        for (int i = 0; i < 3; ++i)
            k.value[i] = angles      ? v[i]
                         : direction ? v[i] / len
                                     : (std::max)(0.0f, (std::min)(255.0f, v[i])) / 255.0f;
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end(),
              [](const Config::LightCycleKey& a, const Config::LightCycleKey& b) { return a.step < b.step; });
    return keys;
}

// [lighting] LightCycleImage: the file named (a full path; or a name looked for in the running mod's GraphicsUpgrade
// folder, then under Assets as the game finds art, the mod's first, then the game folder's GraphicsUpgrade folder and
// the game folder itself) as cycle keyframes, one per column (step = column), sun from the top row, ambient from the
// second (none if it has one row). gameDir: the folder with d3d9.dll. False (and a warning) if it can't be used.
static bool CycleImage(std::wstring name, const std::wstring& gameDir, std::vector<Config::LightCycleKey>& sun,
                       std::vector<Config::LightCycleKey>& ambient)
{
    name.erase(0, name.find_first_not_of(L" \t\""));
    name.erase(name.find_last_not_of(L" \t\"") + 1);
    if (name.empty())
        return false;
    std::replace(name.begin(), name.end(), L'/', L'\\');
    std::vector<uint8_t> bytes;
    std::wstring from;
    const bool full = name.size() > 2 && (name[1] == L':' || (name[0] == L'\\' && name[1] == L'\\'));
    if (full)
        from = ReadFileBytes(name, bytes) ? name : L"";
    else if (!g_config.modGraphicsDir.empty() && ReadFileBytes(g_config.modGraphicsDir + L"\\" + name, bytes))
        from = g_config.modGraphicsDir + L"\\" + name;
    else if (!ReadAsset(name, bytes, from))
    {
        for (const std::wstring& dir : {gameDir + L"\\GraphicsUpgrade", gameDir})
            if (ReadFileBytes(dir + L"\\" + name, bytes))
            {
                from = dir + L"\\" + name;
                break;
            }
    }
    char msg[700];
    UINT w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (from.empty() || !DecodeImage(bytes, w, h, rgba))
    {
        snprintf(msg, sizeof(msg), "settings: [lighting] LightCycleImage=%ls %s; LightCycleSteps and "
                 "LightCycleAmbientSteps used instead", name.c_str(), from.empty() ? "not found" : "can't be read as an image");
        g_settingsWarnings.push_back(msg);
        return false;
    }
    sun.clear();
    ambient.clear();
    for (UINT x = 0; x < w; ++x)
    {
        Config::LightCycleKey k;
        k.step = static_cast<float>(x);
        for (UINT row = 0; row < (std::min)(h, 2u); ++row)
        {
            const uint8_t* p = &rgba[(static_cast<size_t>(row) * w + x) * 4];
            for (int c = 0; c < 3; ++c)
                k.value[c] = p[c] / 255.0f;
            (row == 0 ? sun : ambient).push_back(k);
        }
    }
    snprintf(msg, sizeof(msg), "settings: [lighting] LightCycleImage: %ls, %ux%u: %u steps, sun from the top row%s",
             from.c_str(), w, h, w, h > 1 ? ", ambient from the second" : " (no second row: the ambient doesn't cycle)");
    g_settingsWarnings.push_back(msg);
    return true;
}

// The running mod's folder: BtS runs a mod with "mod= Mods\<name>" (or mod=\Mods\<name>, quoted or not) on its
// command line, or loads the one in CivilizationIV.ini's [CONFIG] Mod (0 = none) at startup. Empty: no mod.
static std::wstring ModFolder(const std::wstring& gameDir)
{
    auto resolve = [&](std::wstring value) -> std::wstring {
        value.erase(0, value.find_first_not_of(L" \t\"\\/"));
        value.erase(value.find_last_not_of(L" \t\"\\/") + 1);
        if (value.empty() || value == L"0")
            return L"";
        std::wstring path = value.find(L':') != std::wstring::npos ? value : gameDir + L"\\" + value;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            path = gameDir + L"\\Mods\\" + value;  // "mod= Name" without Mods\ in front
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) ? path : L"";
    };
    std::wstring cmd = GetCommandLineW(), lower = cmd;
    for (auto& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    size_t at = lower.find(L"mod=");
    if (at != std::wstring::npos && at > 0 && cmd[at - 1] == L'"')
    {
        // The whole argument quoted: "mod=Mods\name with spaces"
        size_t end = cmd.find(L'"', at + 4);
        return resolve(cmd.substr(at + 4, end == std::wstring::npos ? std::wstring::npos : end - at - 4));
    }
    if (at != std::wstring::npos)
    {
        size_t start = cmd.find_first_not_of(L" \t", at + 4);
        if (start != std::wstring::npos)
        {
            size_t end = cmd[start] == L'"' ? cmd.find(L'"', start + 1) : cmd.find_first_of(L" \t", start);
            return resolve(cmd.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start + (cmd[start] == L'"')));
        }
    }
    // Autoloaded (BtS only): BtS's own settings (My Documents\My Games\Beyond the Sword\CivilizationIV.ini).
    wchar_t exe[MAX_PATH] = {}, docs[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeName = exe;
    for (auto& c : exeName)
        c = static_cast<wchar_t>(towlower(c));
    if (exeName.find(L"civ4beyondsword") != std::wstring::npos &&
        SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
    {
        std::wstring civIni = std::wstring(docs) + L"\\My Games\\Beyond the Sword\\CivilizationIV.ini";
        wchar_t buf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"CONFIG", L"Mod", L"0", buf, MAX_PATH, civIni.c_str());
        return resolve(buf);
    }
    return L"";
}

void LoadConfig()
{
    std::wstring dir = ModuleDir(g_self);
    std::wstring ini = dir + L"\\GraphicsUpgrade.ini";
    // A running mod's GraphicsUpgrade folder: its INI overrides this one, setting by setting.
    g_config.modDir = ModFolder(dir);
    g_config.modGraphicsDir.clear();
    g_modIni.clear();
    if (!g_config.modDir.empty())
    {
        std::wstring modGu = g_config.modDir + L"\\GraphicsUpgrade";
        DWORD attrs = GetFileAttributesW(modGu.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            g_config.modGraphicsDir = modGu;
        std::wstring modIni = modGu + L"\\GraphicsUpgrade.ini";
        if (GetFileAttributesW(modIni.c_str()) != INVALID_FILE_ATTRIBUTES)
            g_modIni = modIni;
        char msg[600];
        snprintf(msg, sizeof(msg), "settings: mod %ls: %ls", g_config.modDir.c_str(),
                 g_modIni.empty() ? (g_config.modGraphicsDir.empty() ? L"no GraphicsUpgrade folder; the game folder's settings"
                                                                      : L"GraphicsUpgrade folder, no GraphicsUpgrade.ini; the game folder's settings")
                                  : L"its GraphicsUpgrade\\GraphicsUpgrade.ini overrides the game folder's, setting by setting");
        g_settingsWarnings.push_back(msg);
    }
    WarnDuplicates(ini);
    if (!g_modIni.empty())
        WarnDuplicates(g_modIni);
    g_config.outputDir = IniString(ini, L"general", L"OutputDir", L"");
    if (g_config.outputDir.empty())
    {
        // Never default into the game folder.
        wchar_t local[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        g_config.outputDir = std::wstring(local) + L"\\GraphicsUpgrade";
    }
    g_config.chainDll = IniString(ini, L"general", L"ChainDll", L"");
    g_config.games = IniString(ini, L"general", L"Games", L"bts");
    g_config.dumpShaders = IniInt(ini, L"debug", L"DumpShaders", 0) != 0;
    g_config.tracesEnabled = IniInt(ini, L"debug", L"Traces", 0) != 0;
    g_config.terrainDebug = (std::max)(0, (std::min)(4, IniInt(ini, L"debug", L"TerrainDebug", 0)));
    g_config.terrainDebugKey = IniKey(ini, L"debug", L"TerrainDebugKey", '0');
    g_config.waterTime = static_cast<float>(_wtof(IniString(ini, L"debug", L"WaterTime", L"-1").c_str()));
    g_config.terrainVertsPerPlot = IniInt(ini, L"debug", L"TerrainVertsPerPlot", 17);
    g_config.tileSetsPatch = IniInt(ini, L"terrain tiles", L"TilePicker", 0) != 0;
    g_config.hillField = IniInt(ini, L"terrain tiles", L"HillStamps", 0) != 0;
    for (int s = 0; s < 8; ++s)  // JigsawPieces1..8
    {
        const std::wstring n = L"JigsawPieces" + std::to_wstring(s + 1);
        Config::JigsawSet& j = g_config.jigsaw[s];
        j = Config::JigsawSet();
        j.terrains = IniString(ini, L"terrain tiles", (n + L"Terrains").c_str(), L"");
        j.pieces = IniString(ini, L"terrain tiles", (n + L"Folder").c_str(), L"");
        std::wstring base = IniString(ini, L"terrain tiles", (n + L"Floor").c_str(), L"keep");
        for (auto& c : base)
            c = static_cast<wchar_t>(towlower(c));
        j.base = base == L"water" ? 1 : base == L"land" ? 2 : 0;
        if (!j.terrains.empty() && base != L"keep" && base != L"water" && base != L"land")
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "settings: [terrain tiles] %lsFloor=%ls should be keep, water or land; keep used",
                     n.c_str(), base.c_str());
            g_settingsWarnings.push_back(msg);
        }
        const std::wstring height = IniString(ini, L"terrain tiles", (n + L"Height").c_str(), L"");
        j.hasHeight = !height.empty();
        j.height = j.hasHeight ? static_cast<float>(_wtof(height.c_str())) : 1.0f;
    }
    for (int s = 0; s < 8; ++s)  // PlotStamps1..8
    {
        const std::wstring n = L"PlotStamps" + std::to_wstring(s + 1);
        Config::PlotStampSet& p = g_config.plotStamps[s];
        p = Config::PlotStampSet();
        p.terrains = IniString(ini, L"terrain tiles", (n + L"Terrains").c_str(), L"");
        p.folder = IniString(ini, L"terrain tiles", (n + L"Folder").c_str(), L"");
        p.features = IniString(ini, L"terrain tiles", (n + L"Features").c_str(), L"");
        p.hideModels = IniString(ini, L"terrain tiles", (n + L"HideModels").c_str(), L"");
        const std::wstring height = IniString(ini, L"terrain tiles", (n + L"Height").c_str(), L"");
        p.hasHeight = !height.empty();
        p.height = p.hasHeight ? static_cast<float>(_wtof(height.c_str())) : 1.0f;
    }
    g_config.tileSets = (std::max)(1, (std::min)(8, IniInt(ini, L"terrain tiles", L"TileGroups", 1)));
    for (int s = 0; s < 8; ++s)  // the INI numbers tile groups from 1: TileGroup1 is set 0
        g_config.tileSetTerrains[s] =
            IniString(ini, L"terrain tiles", (L"TileGroup" + std::to_wstring(s + 1)).c_str(), L"");
    auto fourInts = [&](const wchar_t* key, int* out) {
        int v[4];
        if (swscanf_s(IniString(ini, L"terrain tiles", key, L"").c_str(), L"%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4)
            for (int k = 0; k < 4; ++k)
                out[k] = (std::max)(1, v[k]);
    };
    fourInts(L"CoastAtlasCells", g_config.coastAtlasCells);
    fourInts(L"CoastWaveModels", g_config.coastWaveModels);
    for (int k = 0; k < 8; ++k)
        g_config.landBlends[k] =
            IniString(ini, L"terrain tiles", (L"StandardBlendOnLand" + std::to_wstring(k + 1)).c_str(), L"");
    g_config.landBlendTerrains = IniString(ini, L"terrain tiles", L"StandardBlendOnLand", L"");
    g_config.traceKey = IniKey(ini, L"trace", L"Key", '9');
    std::wstring mods = IniString(ini, L"general", L"KeyModifiers", L"ctrl+shift");
    for (auto& c : mods)
        c = static_cast<wchar_t>(towlower(c));
    g_config.keyCtrl = mods.find(L"ctrl") != std::wstring::npos;
    g_config.keyShift = mods.find(L"shift") != std::wstring::npos;
    g_config.keyAlt = mods.find(L"alt") != std::wstring::npos;
    g_config.compareKey = IniKey(ini, L"general", L"CompareKey", VK_OEM_2);
    g_config.traceFrames = IniInt(ini, L"trace", L"Frames", 1);
    g_config.autoTraceFrame = IniInt(ini, L"trace", L"AutoFrame", 0);
    g_config.hashTextures = IniInt(ini, L"trace", L"HashTextures", 1) != 0;
    g_config.clearPureDevice = IniInt(ini, L"device", L"ClearPureDevice", 1) != 0;
    g_config.ghostEnabled = IniInt(ini, L"ghost", L"Enabled", 0) != 0;
    g_config.ghostKey = IniKey(ini, L"ghost", L"ToggleKey", '8');
    std::wstring offset = IniString(ini, L"ghost", L"Offset", L"90,0,0");
    swscanf_s(offset.c_str(), L"%f,%f,%f", &g_config.ghostOffset[0], &g_config.ghostOffset[1], &g_config.ghostOffset[2]);
    g_config.reflectionEnabled = IniInt(ini, L"reflection", L"Enabled", 1) != 0;
    g_config.reflectionSize = static_cast<UINT>(IniInt(ini, L"reflection", L"Size", 512));
    g_config.waterHeight = static_cast<float>(_wtof(IniString(ini, L"reflection", L"WaterHeight", L"49.5").c_str()));
    g_config.reflectionKey = IniKey(ini, L"reflection", L"ToggleKey", 0);
    g_config.overlayEnabled = IniInt(ini, L"reflection", L"Overlay", 0) != 0;
    g_config.overlayKey = IniKey(ini, L"reflection", L"OverlayToggleKey", 0);
    g_config.waterEnabled = IniInt(ini, L"water", L"Enabled", 1) != 0;
    g_config.waterKey = IniKey(ini, L"water", L"ToggleKey", '1');
    g_config.waterNormalKey = IniKey(ini, L"water", L"NormalMapToggleKey", '7');
    g_config.waterNormalMap = IniInt(ini, L"water", L"NormalMap", 0);
    std::wstring colDir = IniString(ini, L"water", L"ColonizationDir", L"");
    if (!colDir.empty())
        g_config.colonizationDir = colDir;
    g_config.waterSurfaceTextures =
        IniList(ini, L"water", L"SurfaceTextures", L"Art\\Terrain\\Features\\Oasis\\oasis_water.dds");
    g_config.underwaterDepth = static_cast<float>(_wtof(IniString(ini, L"water", L"UnderwaterDepth", L"25").c_str()));
    g_config.underwaterVisibility =
        static_cast<float>(_wtof(IniString(ini, L"water", L"UnderwaterVisibility", L"0.7").c_str()));
    g_config.underwaterBands = static_cast<UINT>((std::max)(1, (std::min)(16, IniInt(ini, L"water", L"UnderwaterBands", 8))));
    g_config.glintFollowsSun = (std::max)(0, (std::min)(2, IniInt(ini, L"water", L"GlintFollowsSun", 1)));
    auto strength = [&](const wchar_t* key) {
        return (std::max)(0.0f, (std::min)(4.0f, static_cast<float>(_wtof(IniString(ini, L"water", key, L"1").c_str()))));
    };
    g_config.caustics = strength(L"Caustics");
    g_config.waveShading = (std::max)(0, (std::min)(255, IniInt(ini, L"water", L"WaveShading", 64)));
    g_config.foamLight = (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(_wtof(
                                                           IniString(ini, L"water", L"FoamLight", L"0.4").c_str()))));
    g_config.riverGlint = (std::max)(0, (std::min)(255, IniInt(ini, L"water", L"RiverGlint", 128)));
    g_config.riverShimmer = (std::max)(0.0f, (std::min)(3.0f, static_cast<float>(_wtof(
                                                              IniString(ini, L"water", L"RiverShimmer", L"3").c_str()))));
    std::wstring civ5Dir = IniString(ini, L"water", L"Civ5Dir", L"");
    if (!civ5Dir.empty())
        g_config.civ5Dir = civ5Dir;
    // The water shader's f4WaterConstants: x = ReflectionEvenness, y = ReflectionRipple, z = RefractionRipple (w unused).
    // 1.0's INIs had them as one setting, WaterConstants=x,y,z,w, read when none of the three is set.
    const wchar_t* waterKeys[3] = {L"ReflectionEvenness", L"ReflectionRipple", L"RefractionRipple"};
    bool oldWaterConstants = false;
    for (int i = 0; i < 3; ++i)
    {
        bool old;
        std::wstring v = IniRenamed(ini, L"water", waterKeys[i], L"water", L"WaterConstants", old,
                                    {waterKeys[0], waterKeys[1], waterKeys[2]});
        float c[4];
        if (old && swscanf_s(v.c_str(), L"%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) >= i + 1)
            g_config.waterConstants[i] = (std::max)(0.0f, c[i]), oldWaterConstants = true;
        else if (!old && !v.empty())
            g_config.waterConstants[i] = (std::max)(0.0f, static_cast<float>(_wtof(v.c_str())));
    }
    if (oldWaterConstants)
        g_settingsWarnings.push_back("settings: [water] WaterConstants is the old name; use ReflectionEvenness, "
                                     "ReflectionRipple and RefractionRipple");
    g_config.shadowsEnabled = IniInt(ini, L"shadows", L"Enabled", 1) != 0;
    g_config.shadowsKey = IniKey(ini, L"shadows", L"ToggleKey", '2');
    g_config.shadowSize = static_cast<UINT>(IniInt(ini, L"shadows", L"Size", 2048));
    g_config.shadowMaxRadius = static_cast<float>(_wtof(IniString(ini, L"shadows", L"MaxRadius", L"2000").c_str()));
    g_config.shadowSoftness = static_cast<float>(_wtof(IniString(ini, L"shadows", L"Softness", L"0.5").c_str()));
    g_config.shadowDarkness = static_cast<float>(_wtof(IniString(ini, L"shadows", L"Darkness", L"0.45").c_str()));
    g_config.hideBlobShadows = IniInt(ini, L"shadows", L"HideBlobShadows", 1) != 0;
    g_config.blobShadowTexture =
        IniList(ini, L"shadows", L"BlobShadowTexture", L"Art\\Units\\01_UnitShadows\\unit_shadows.dds");
    g_config.noShadowTextures = IniList(ini, L"shadows", L"NoShadowTextures", L"");
    g_config.buildingShadows = IniInt(ini, L"shadows", L"BuildingShadows", 1) != 0;
    g_config.buildingDarkness =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"BuildingDarkness", L"0.68").c_str()));
    g_config.hillShadows = IniInt(ini, L"shadows", L"HillShadows", 1) != 0;
    g_config.hillShadowSoftness =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"HillShadowSoftness", L"8").c_str()));
    g_config.hillShadowLength =
        (std::max)(0.1f, static_cast<float>(_wtof(IniString(ini, L"shadows", L"HillShadowLength", L"1.64").c_str())));
    g_config.waterShadow =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"WaterShadow", L"0.6").c_str()));
    g_config.shadowOverlay = IniInt(ini, L"shadows", L"Overlay", 0) != 0;
    g_config.shadowOverlayKey = IniKey(ini, L"shadows", L"OverlayToggleKey", 0);
    std::wstring sunDir = IniString(ini, L"shadows", L"SunDirection", L"");
    if (!sunDir.empty())
        swscanf_s(sunDir.c_str(), L"%f,%f,%f", &g_config.sunDirection[0], &g_config.sunDirection[1],
                  &g_config.sunDirection[2]);
    // [lighting]: colours as R,G,B in 0..255. SunColour/AmbientColour apply to every group unless the group's own key
    // in [lighting groups] is set.
    auto parseRgb = [](const std::wstring& v, float* out) {
        int c[3];
        if (v.empty() || swscanf_s(v.c_str(), L"%d,%d,%d", &c[0], &c[1], &c[2]) != 3)
            return false;
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<float>((std::max)(0, (std::min)(255, c[i]))) / 255.0f;
        return true;
    };
    auto rgb = [&](const wchar_t* key, float* out) { return parseRgb(IniString(ini, L"lighting", key, L""), out); };
    // A group's colour; 1.0's INIs had these in [lighting].
    auto groupRgb = [&](const wchar_t* key, float* out) {
        bool old;
        std::wstring v = IniRenamed(ini, L"lighting groups", key, L"lighting", key, old);
        if (old)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "settings: [lighting] %ls has moved; set it in [lighting groups]", key);
            g_settingsWarnings.push_back(msg);
        }
        return parseRgb(v, out);
    };
    g_config.anisotropy = static_cast<UINT>((std::max)(1, (std::min)(16, IniInt(ini, L"textures", L"Anisotropy", 8))));
    g_config.anisotropyKey = IniKey(ini, L"textures", L"ToggleKey", '5');
    g_config.detailNormals = IniInt(ini, L"textures", L"DetailNormals", 1) != 0;
    g_config.detailNormalsKey = IniKey(ini, L"textures", L"DetailNormalsToggleKey", '6');
    g_config.detailNormalStrength = (std::max)(
        0.0f, static_cast<float>(_wtof(IniString(ini, L"textures", L"DetailNormalStrength", L"1").c_str())));
    g_config.blendTileClamp = IniInt(ini, L"textures", L"BlendTileClamp", 1) != 0;
    g_config.blendNormalStrength = (std::max)(
        0.0f, static_cast<float>(_wtof(IniString(ini, L"textures", L"BlendNormalStrength", L"1").c_str())));
    g_config.detailBumpiness =
        (std::max)(0.0f, static_cast<float>(_wtof(IniString(ini, L"textures", L"DetailBumpiness", L"1.5").c_str())));
    g_config.lightingEnabled = IniInt(ini, L"lighting", L"Enabled", 1) != 0;
    g_config.lightingKey = IniKey(ini, L"lighting", L"ToggleKey", '3');
    g_config.lightCycleKey = IniKey(ini, L"lighting", L"LightCycleToggleKey", '4');
    g_config.matchShadowDirection = IniInt(ini, L"lighting", L"MatchShadowDirection", 1) != 0;
    std::wstring lightDir = IniString(ini, L"lighting", L"LightDirection", L"");
    g_config.haveLightDirection =
        !lightDir.empty() && swscanf_s(lightDir.c_str(), L"%f,%f,%f", &g_config.lightDirection[0],
                                       &g_config.lightDirection[1], &g_config.lightDirection[2]) == 3;
    float sunAll[3], ambientAll[3];
    bool haveSun = rgb(L"SunColour", sunAll), haveAmbient = rgb(L"AmbientColour", ambientAll);
    const wchar_t* sunKeys[kLightGroups] = {L"UnitSun", L"MechSun",     L"TerrainSun", L"RiverSun",
                                            L"TreeSun", L"BuildingSun", L"WaterSun"};
    const wchar_t* ambientKeys[kLightGroups] = {L"UnitAmbient", L"MechAmbient",     L"TerrainAmbient", L"RiverAmbient",
                                                L"TreeAmbient", L"BuildingAmbient", nullptr};  // water: no ambient
    for (int g = 0; g < kLightGroups; ++g)
    {
        g_config.lightSunSet[g] = g_config.lightSunOwn[g] = groupRgb(sunKeys[g], g_config.lightSun[g]);
        if (!g_config.lightSunSet[g] && haveSun)
            memcpy(g_config.lightSun[g], sunAll, sizeof(sunAll)), g_config.lightSunSet[g] = true;
        if (!ambientKeys[g])
            continue;
        g_config.lightAmbientSet[g] = g_config.lightAmbientOwn[g] =
            groupRgb(ambientKeys[g], g_config.lightAmbient[g]);
        if (!g_config.lightAmbientSet[g] && haveAmbient)
            memcpy(g_config.lightAmbient[g], ambientAll, sizeof(ambientAll)), g_config.lightAmbientSet[g] = true;
    }
    g_config.bakedTerrainSunSet = rgb(L"BakedTerrainSun", g_config.bakedTerrainSun);
    // Light colour cycle: LightCycleSteps (sun) and LightCycleAmbientSteps = step,R,G,B;step,R,G,B;... (steps from 0
    // to LightChangeStepCount, colours 0..255), blended in a loop over LightChangeCycleTime seconds.
    g_config.lightCycleOn = IniInt(ini, L"lighting", L"LightCycleOn", 1) != 0;
    g_config.lightCycleTime =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightChangeCycleTime", L"600").c_str()));
    g_config.lightCycleStepCount =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightChangeStepCount", L"24").c_str()));
    g_config.lightCycleStart =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightCycleStartStep", L"12").c_str()));
    // LightCycleImage: a small picture, a column per step (its width is the step count), the top row the sun's colour
    // and the second the ambient's. It replaces LightCycleSteps and LightCycleAmbientSteps.
    std::vector<Config::LightCycleKey> imageSun, imageAmbient;
    const bool image = CycleImage(IniString(ini, L"lighting", L"LightCycleImage", L""), dir, imageSun, imageAmbient);
    if (image)
        g_config.lightCycleStepCount = static_cast<float>(imageSun.size());
    if (g_config.lightCycleStart < 0 || g_config.lightCycleStart > g_config.lightCycleStepCount)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "settings: [lighting] LightCycleStartStep=%g isn't within 0..%g; starting at 0",
                 g_config.lightCycleStart, g_config.lightCycleStepCount);
        g_settingsWarnings.push_back(msg);
        g_config.lightCycleStart = 0;
    }
    g_config.lightCycle = image ? imageSun : CycleKeys(ini, L"LightCycleSteps", g_config.lightCycleStepCount);
    g_config.lightCycleAmbient =
        image ? imageAmbient : CycleKeys(ini, L"LightCycleAmbientSteps", g_config.lightCycleStepCount);
    g_config.lightCycleDirection = CycleKeys(ini, L"LightCycleDirections", g_config.lightCycleStepCount, kCycleDirection);
    g_config.lightCycleAngles = CycleKeys(ini, L"LightCycleSunAngles", g_config.lightCycleStepCount, kCycleAngles);
    // The simple sun path: SunMiddayFrom = a compass point (N, NNE, NE ... NNW) or a bearing in degrees.
    std::wstring from = IniString(ini, L"lighting", L"SunMiddayFrom", L"");
    from.erase(0, from.find_first_not_of(L" \t"));
    from.erase(from.find_last_not_of(L" \t") + 1);
    g_config.sunPath = false;
    if (!from.empty())
    {
        static const wchar_t* const kPoints[] = {L"N",  L"NNE", L"NE", L"ENE", L"E",  L"ESE", L"SE", L"SSE",
                                                 L"S",  L"SSW", L"SW", L"WSW", L"W",  L"WNW", L"NW", L"NNW"};
        std::wstring upper;
        for (wchar_t c : from)
            upper += static_cast<wchar_t>(towupper(c));
        for (int i = 0; i < 16 && !g_config.sunPath; ++i)
            if (upper == kPoints[i])
                g_config.sunMiddayFrom = 22.5f * i, g_config.sunPath = true;
        wchar_t* end = nullptr;
        double bearing = wcstod(from.c_str(), &end);
        if (!g_config.sunPath && end && *end == 0)
            g_config.sunMiddayFrom = static_cast<float>(std::fmod(std::fmod(bearing, 360.0) + 360.0, 360.0)),
            g_config.sunPath = true;
        if (!g_config.sunPath)
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "settings: [lighting] SunMiddayFrom=%ls isn't a compass point (N, NNE, NE ... "
                     "NNW) or a bearing in degrees; no sun path", from.c_str());
            g_settingsWarnings.push_back(msg);
        }
    }
    auto height = [&](const wchar_t* key, float def) {
        float h = static_cast<float>(_wtof(IniString(ini, L"lighting", key, std::to_wstring(def).c_str()).c_str()));
        if (h > 0 && h <= 90)
            return h;
        char msg[160];
        snprintf(msg, sizeof(msg), "settings: [lighting] %ls=%g isn't 0..90 degrees above the horizon; using %g", key, h,
                 def);
        g_settingsWarnings.push_back(msg);
        return def;
    };
    g_config.sunMiddayHeight = height(L"SunMiddayHeight", 45);
    g_config.sunriseHeight = height(L"SunriseHeight", 30);
    g_config.moonHeight = height(L"MoonHeight", 50);
    g_config.shadowsFollowSun = IniInt(ini, L"lighting", L"ShadowsFollowSun", 1) != 0;
    g_config.moonGlintSet = rgb(L"MoonGlint", g_config.moonGlint);
    // LitEffectTextures: TextureList entries, or "*" for every blended, unlit effect.
    g_config.litEffectTextures = IniString(ini, L"lighting", L"LitEffectTextures", L"");
    std::wstring allEffects = g_config.litEffectTextures;
    allEffects.erase(0, allEffects.find_first_not_of(L" \t"));
    allEffects.erase(allEffects.find_last_not_of(L" \t") + 1);
    g_config.litEffectsAll = allEffects == L"*";
    if (g_config.litEffectsAll)
        g_config.litEffectTextures.clear();
    const float range =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"BuildingLightRange", L"1").c_str()));
    g_config.buildingLightRange = (std::min)(2.0f, (std::max)(1.0f, range));
    if (range < 1 || range > 2)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "settings: [lighting] BuildingLightRange=%g isn't 1..2; using %g", range,
                 g_config.buildingLightRange);
        g_settingsWarnings.push_back(msg);
    }
    if (g_config.lightCycleOn && ((g_config.lightCycle.empty() && g_config.lightCycleAmbient.empty() &&
                                   g_config.lightCycleDirection.empty() && g_config.lightCycleAngles.empty() &&
                                   !g_config.sunPath) ||
                                  g_config.lightCycleTime <= 0 || g_config.lightCycleStepCount <= 0))
    {
        g_settingsWarnings.push_back("settings: [lighting] LightCycleOn=1 needs LightCycleSteps, "
                                     "LightCycleAmbientSteps, LightCycleDirections or LightCycleSunAngles, a LightChangeCycleTime and a "
                                     "LightChangeStepCount above 0; no cycle");
        g_config.lightCycleOn = false;
    }
    std::wstring bakedDir = IniString(ini, L"lighting", L"BakedTerrainDirection", L"");
    g_config.bakedTerrainDirectionSet =
        !bakedDir.empty() && swscanf_s(bakedDir.c_str(), L"%f,%f,%f", &g_config.bakedTerrainDirection[0],
                                       &g_config.bakedTerrainDirection[1], &g_config.bakedTerrainDirection[2]) == 3;
    g_config.bakedTerrainAmbientSet = rgb(L"BakedTerrainAmbient", g_config.bakedTerrainAmbient);
}

void LoadBakedTerrainLight()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    if (g_config.bakedTerrainSunSet && g_config.bakedTerrainAmbientSet && g_config.bakedTerrainDirectionSet)
    {
        Log::Write("lighting: baked terrain light from the settings (BakedTerrainSun, BakedTerrainAmbient, "
                   "BakedTerrainDirection)");
        return;
    }
    const wchar_t* const kSunLight = L"Art\\Terrain\\Lights\\SunLight.nif";
    std::vector<uint8_t> nif;
    std::wstring from;
    float sun[3], ambient[3], direction[3];
    if (!ReadAsset(kSunLight, nif, from))
        Log::Write("lighting: %ls not found; baked terrain light as BtS's", kSunLight);
    else if (!ReadNifLights(nif, sun, ambient, direction))
        Log::Write("lighting: %ls (%ls) isn't a light rig GraphicsUpgrade can read; baked terrain light as BtS's",
                   kSunLight, from.c_str());
    else
    {
        if (!g_config.bakedTerrainSunSet)
            memcpy(g_config.bakedTerrainSun, sun, sizeof(sun));
        if (!g_config.bakedTerrainAmbientSet)
            memcpy(g_config.bakedTerrainAmbient, ambient, sizeof(ambient));
        if (!g_config.bakedTerrainDirectionSet)
            memcpy(g_config.bakedTerrainDirection, direction, sizeof(direction));
        Log::Write("lighting: %ls (%ls): sun %.0f,%.0f,%.0f, ambient %.0f,%.0f,%.0f, direction %.3f,%.3f,%.3f", kSunLight,
                   from.c_str(), sun[0] * 255, sun[1] * 255, sun[2] * 255, ambient[0] * 255, ambient[1] * 255,
                   ambient[2] * 255, direction[0], direction[1], direction[2]);
    }
    const float* s = g_config.bakedTerrainSun;
    const float* a = g_config.bakedTerrainAmbient;
    const float* d = g_config.bakedTerrainDirection;
    Log::Write("lighting: baked terrain light: sun %.0f,%.0f,%.0f%s, ambient %.0f,%.0f,%.0f%s, direction %.3f,%.3f,%.3f%s",
               s[0] * 255, s[1] * 255, s[2] * 255, g_config.bakedTerrainSunSet ? " (settings)" : "", a[0] * 255,
               a[1] * 255, a[2] * 255, g_config.bakedTerrainAmbientSet ? " (settings)" : "", d[0], d[1], d[2],
               g_config.bakedTerrainDirectionSet ? " (settings)" : "");
}

std::wstring ProxyDir()
{
    return ModuleDir(g_self);
}

namespace
{
// The draw being handled, if any (MainViewScope): its device and answer.
IDirect3DDevice9* g_mainViewDev = nullptr;
bool g_mainView = false;

bool WorkOutMainView(IDirect3DDevice9* dev)
{
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    bool main = rt && rt == bb;
    D3DSURFACE_DESC r = {}, b = {};
    if (!main && rt && bb && SUCCEEDED(rt->GetDesc(&r)) && SUCCEEDED(bb->GetDesc(&b)))
        main = r.Width == b.Width && r.Height == b.Height;
    if (rt)
        rt->Release();
    if (bb)
        bb->Release();
    return main;
}
}

bool TargetIsMainView(IDirect3DDevice9* dev)
{
    return dev == g_mainViewDev ? g_mainView : WorkOutMainView(dev);
}

MainViewScope::MainViewScope(IDirect3DDevice9* dev)
    : m_prevDev(g_mainViewDev), m_prevMain(g_mainView), m_main(WorkOutMainView(dev))
{
    g_mainViewDev = dev;
    g_mainView = m_main;
}

MainViewScope::~MainViewScope()
{
    g_mainViewDev = m_prevDev;
    g_mainView = m_prevMain;
}

bool EffectsAllowed()
{
    // Config::games is a comma-separated list of game ids; "*" allows any game.
    std::wstring list = g_config.games, id = Widen(Log::GameId());
    for (auto& c : list)
        c = static_cast<wchar_t>(towlower(c));
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find(L',', start);
        std::wstring item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        item.erase(0, item.find_first_not_of(L" \t"));
        item.erase(item.find_last_not_of(L" \t") + 1);
        if (item == L"*" || item == id)
            return true;
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return false;
}

uint64_t Fnv1a(const void* data, size_t size, uint64_t hash)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(uint64_t value)
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string Ptr(const void* p)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return buf;
}

std::string Guid(REFIID id)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", id.Data1, id.Data2, id.Data3,
             id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    return buf;
}

std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());  // ASCII only (ids and file names we make ourselves)
}

bool EnsureDir(const std::wstring& path)
{
    if (path.empty())
        return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2)
        EnsureDir(path.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

namespace Log
{
static CRITICAL_SECTION s_lock;
static std::wstring s_gameDir;
static std::string s_gameId;
static FILE* s_file = nullptr;

void Init()
{
    InitializeCriticalSection(&s_lock);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name(exe);
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name == L"colonization.exe")
        s_gameId = "col";
    else if (name == L"civ4beyondsword.exe")
        s_gameId = "bts";
    else
        for (size_t i = 0; i + 4 < name.size() || (name.size() <= 4 && i < name.size()); ++i)
            s_gameId += static_cast<char>(name[i] < 128 ? name[i] : '_');  // exe name without ".exe"
    s_gameDir = g_config.outputDir + L"\\" + Widen(s_gameId);
    EnsureDir(s_gameDir);
    // A new log each launch; the last one is kept as session.previous.log. If the old log is still open (a relaunch,
    // e.g. BtS restarting itself to load a mod, while the first process exits) it can't be moved: append to it.
    // Shared, so the log can be read while the game runs. Every line carries the process id.
    const std::wstring path = s_gameDir + L"\\session.log";
    MoveFileExW(path.c_str(), (s_gameDir + L"\\session.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    s_file = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
    Write("---- session start: GraphicsUpgrade " GRAPHICSUPGRADE_VERSION ", %ls (game id %s), output %ls", exe, s_gameId.c_str(),
          s_gameDir.c_str());
    for (const std::string& w : g_settingsWarnings)
        Write("%s", w.c_str());  // found while reading the INI, before the log was open
    g_settingsWarnings.clear();
}

void Write(const char* fmt, ...)
{
    if (!s_file)
        return;
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&s_lock);
    fprintf(s_file, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentProcessId(), msg);
    fflush(s_file);
    LeaveCriticalSection(&s_lock);
}

const std::wstring& GameDir() { return s_gameDir; }
const std::string& GameId() { return s_gameId; }
}
