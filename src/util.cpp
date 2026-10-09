// Config loading, logging and small helpers.
#include "common.h"

#include <share.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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
    g_config.traceKey = IniKey(ini, L"trace", L"Key", VK_F11);
    std::wstring mods = IniString(ini, L"general", L"KeyModifiers", L"ctrl+alt+shift");
    for (auto& c : mods)
        c = static_cast<wchar_t>(towlower(c));
    g_config.keyCtrl = mods.find(L"ctrl") != std::wstring::npos;
    g_config.keyShift = mods.find(L"shift") != std::wstring::npos;
    g_config.keyAlt = mods.find(L"alt") != std::wstring::npos;
    g_config.traceFrames = IniInt(ini, L"trace", L"Frames", 1);
    g_config.autoTraceFrame = IniInt(ini, L"trace", L"AutoFrame", 0);
    g_config.hashTextures = IniInt(ini, L"trace", L"HashTextures", 1) != 0;
    g_config.clearPureDevice = IniInt(ini, L"device", L"ClearPureDevice", 1) != 0;
    g_config.ghostEnabled = IniInt(ini, L"ghost", L"Enabled", 0) != 0;
    g_config.ghostKey = IniKey(ini, L"ghost", L"ToggleKey", 'G');
    std::wstring offset = IniString(ini, L"ghost", L"Offset", L"90,0,0");
    swscanf_s(offset.c_str(), L"%f,%f,%f", &g_config.ghostOffset[0], &g_config.ghostOffset[1], &g_config.ghostOffset[2]);
    g_config.reflectionEnabled = IniInt(ini, L"reflection", L"Enabled", 1) != 0;
    g_config.reflectionSize = static_cast<UINT>(IniInt(ini, L"reflection", L"Size", 512));
    g_config.waterHeight = static_cast<float>(_wtof(IniString(ini, L"reflection", L"WaterHeight", L"49.5").c_str()));
    g_config.reflectionKey = IniKey(ini, L"reflection", L"ToggleKey", 'R');
    g_config.overlayEnabled = IniInt(ini, L"reflection", L"Overlay", 0) != 0;
    g_config.overlayKey = IniKey(ini, L"reflection", L"OverlayToggleKey", 'O');
    g_config.waterEnabled = IniInt(ini, L"water", L"Enabled", 1) != 0;
    g_config.waterKey = IniKey(ini, L"water", L"ToggleKey", 'W');
    std::wstring colDir = IniString(ini, L"water", L"ColonizationDir", L"");
    if (!colDir.empty())
        g_config.colonizationDir = colDir;
    g_config.underwaterDepth = static_cast<float>(_wtof(IniString(ini, L"water", L"UnderwaterDepth", L"25").c_str()));
    g_config.underwaterVisibility =
        static_cast<float>(_wtof(IniString(ini, L"water", L"UnderwaterVisibility", L"0.7").c_str()));
    g_config.underwaterBands = static_cast<UINT>((std::max)(1, (std::min)(16, IniInt(ini, L"water", L"UnderwaterBands", 8))));
    std::wstring wc = IniString(ini, L"water", L"WaterConstants", L"");
    if (!wc.empty())
        swscanf_s(wc.c_str(), L"%f,%f,%f,%f", &g_config.waterConstants[0], &g_config.waterConstants[1],
                  &g_config.waterConstants[2], &g_config.waterConstants[3]);
    g_config.shadowsEnabled = IniInt(ini, L"shadows", L"Enabled", 1) != 0;
    g_config.shadowsKey = IniKey(ini, L"shadows", L"ToggleKey", 'S');
    g_config.shadowSize = static_cast<UINT>(IniInt(ini, L"shadows", L"Size", 2048));
    g_config.shadowMaxRadius = static_cast<float>(_wtof(IniString(ini, L"shadows", L"MaxRadius", L"2000").c_str()));
    g_config.shadowSoftness = static_cast<float>(_wtof(IniString(ini, L"shadows", L"Softness", L"0.5").c_str()));
    g_config.shadowDarkness = static_cast<float>(_wtof(IniString(ini, L"shadows", L"Darkness", L"0.45").c_str()));
    g_config.hideBlobShadows = IniInt(ini, L"shadows", L"HideBlobShadows", 1) != 0;
    g_config.blobShadowTexture =
        IniString(ini, L"shadows", L"BlobShadowTexture", L"Art\\Units\\01_UnitShadows\\unit_shadows.dds");
    g_config.noShadowTextures = IniString(ini, L"shadows", L"NoShadowTextures", L"");
    g_config.buildingShadows = IniInt(ini, L"shadows", L"BuildingShadows", 1) != 0;
    g_config.buildingDarkness =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"BuildingDarkness", L"0.68").c_str()));
    g_config.hillShadows = IniInt(ini, L"shadows", L"HillShadows", 1) != 0;
    g_config.hillShadowSoftness =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"HillShadowSoftness", L"8").c_str()));
    g_config.waterShadow =
        static_cast<float>(_wtof(IniString(ini, L"shadows", L"WaterShadow", L"0.6").c_str()));
    g_config.shadowOverlay = IniInt(ini, L"shadows", L"Overlay", 0) != 0;
    g_config.shadowOverlayKey = IniKey(ini, L"shadows", L"OverlayToggleKey", 0);
    std::wstring sunDir = IniString(ini, L"shadows", L"SunDirection", L"");
    if (!sunDir.empty())
        swscanf_s(sunDir.c_str(), L"%f,%f,%f", &g_config.sunDirection[0], &g_config.sunDirection[1],
                  &g_config.sunDirection[2]);
    // [lighting]: colours as R,G,B in 0..255. SunColour/AmbientColour apply to every group unless a group's own key
    // is set.
    auto rgb = [&](const wchar_t* key, float* out) {
        std::wstring v = IniString(ini, L"lighting", key, L"");
        int c[3];
        if (v.empty() || swscanf_s(v.c_str(), L"%d,%d,%d", &c[0], &c[1], &c[2]) != 3)
            return false;
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<float>((std::max)(0, (std::min)(255, c[i]))) / 255.0f;
        return true;
    };
    g_config.lightingEnabled = IniInt(ini, L"lighting", L"Enabled", 1) != 0;
    g_config.lightingKey = IniKey(ini, L"lighting", L"ToggleKey", 'L');
    g_config.lightCycleKey = IniKey(ini, L"lighting", L"LightCycleToggleKey", 'P');
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
        g_config.lightSunSet[g] = g_config.lightSunOwn[g] = rgb(sunKeys[g], g_config.lightSun[g]);
        if (!g_config.lightSunSet[g] && haveSun)
            memcpy(g_config.lightSun[g], sunAll, sizeof(sunAll)), g_config.lightSunSet[g] = true;
        if (!ambientKeys[g])
            continue;
        g_config.lightAmbientSet[g] = g_config.lightAmbientOwn[g] = rgb(ambientKeys[g], g_config.lightAmbient[g]);
        if (!g_config.lightAmbientSet[g] && haveAmbient)
            memcpy(g_config.lightAmbient[g], ambientAll, sizeof(ambientAll)), g_config.lightAmbientSet[g] = true;
    }
    rgb(L"BakedTerrainSun", g_config.bakedTerrainSun);
    // Light colour cycle: LightCycleSteps (sun) and LightCycleAmbientSteps = step,R,G,B;step,R,G,B;... (steps from 0
    // to LightChangeStepCount, colours 0..255), blended in a loop over LightChangeCycleTime seconds.
    g_config.lightCycleOn = IniInt(ini, L"lighting", L"LightCycleOn", 1) != 0;
    g_config.lightCycleTime =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightChangeCycleTime", L"600").c_str()));
    g_config.lightCycleStepCount =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightChangeStepCount", L"24").c_str()));
    g_config.lightCycleStart =
        static_cast<float>(_wtof(IniString(ini, L"lighting", L"LightCycleStartStep", L"12").c_str()));
    if (g_config.lightCycleStart < 0 || g_config.lightCycleStart > g_config.lightCycleStepCount)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "settings: [lighting] LightCycleStartStep=%g isn't within 0..%g; starting at 0",
                 g_config.lightCycleStart, g_config.lightCycleStepCount);
        g_settingsWarnings.push_back(msg);
        g_config.lightCycleStart = 0;
    }
    g_config.lightCycle = CycleKeys(ini, L"LightCycleSteps", g_config.lightCycleStepCount);
    g_config.lightCycleAmbient = CycleKeys(ini, L"LightCycleAmbientSteps", g_config.lightCycleStepCount);
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
    if (!bakedDir.empty())
        swscanf_s(bakedDir.c_str(), L"%f,%f,%f", &g_config.bakedTerrainDirection[0], &g_config.bakedTerrainDirection[1],
                  &g_config.bakedTerrainDirection[2]);
    rgb(L"BakedTerrainAmbient", g_config.bakedTerrainAmbient);
}

std::wstring ProxyDir()
{
    return ModuleDir(g_self);
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
    // Shared, so the log can be read while the game runs and a second game process (e.g. a relaunch while the first
    // is still exiting) can append to it too. Every line carries the process id.
    s_file = _wfsopen((s_gameDir + L"\\session.log").c_str(), L"ab", _SH_DENYNO);
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
