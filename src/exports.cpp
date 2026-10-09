// DLL entry, loading of the real d3d9.dll, and the d3d9.dll exports.
//
// The games load d3d9.dll by name at runtime (LoadLibrary + GetProcAddress("Direct3DCreate9")), so this DLL is
// picked up from the game folder. Direct3DCreate9 is wrapped; everything else is passed through unchanged.
#include "common.h"
#include "proxy_d3d9.h"

#include <intrin.h>  // _ReturnAddress

#include <cstdio>
#include <cstring>
#include <mutex>

static HMODULE g_real = nullptr;
static std::once_flag g_initOnce;

// Exports with no instrumentation are forwarded through these slots (resolved in Init).
extern "C" {
FARPROC g_fwd_D3DPERF_BeginEvent, g_fwd_D3DPERF_EndEvent, g_fwd_D3DPERF_GetStatus, g_fwd_D3DPERF_QueryRepeatFrame,
    g_fwd_D3DPERF_SetMarker, g_fwd_D3DPERF_SetOptions, g_fwd_D3DPERF_SetRegion, g_fwd_Direct3DCreate9Ex,
    g_fwd_Direct3DShaderValidatorCreate9, g_fwd_PSGPError, g_fwd_PSGPSampleTexture, g_fwd_DebugSetLevel,
    g_fwd_DebugSetMute, g_fwd_Direct3D9EnableMaximizedWindowedModeShim;
}

static void Init()
{
    LoadConfig();
    Log::Init();
    StartWatchdog();
    if (!g_config.chainDll.empty())
    {
        g_real = LoadLibraryW(g_config.chainDll.c_str());
        Log::Write("chain dll %ls: %s", g_config.chainDll.c_str(), g_real ? "loaded" : "FAILED, using system d3d9");
    }
    if (!g_real)
    {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);  // SysWOW64 for a 32-bit process
        g_real = LoadLibraryW((std::wstring(sys) + L"\\d3d9.dll").c_str());
        Log::Write("system d3d9: %ls\\d3d9.dll %s", sys, g_real ? "loaded" : "FAILED");
    }
#define RESOLVE(name) g_fwd_##name = g_real ? GetProcAddress(g_real, #name) : nullptr
    RESOLVE(D3DPERF_BeginEvent);
    RESOLVE(D3DPERF_EndEvent);
    RESOLVE(D3DPERF_GetStatus);
    RESOLVE(D3DPERF_QueryRepeatFrame);
    RESOLVE(D3DPERF_SetMarker);
    RESOLVE(D3DPERF_SetOptions);
    RESOLVE(D3DPERF_SetRegion);
    RESOLVE(Direct3DCreate9Ex);
    RESOLVE(Direct3DShaderValidatorCreate9);
    RESOLVE(PSGPError);
    RESOLVE(PSGPSampleTexture);
    RESOLVE(DebugSetLevel);
    RESOLVE(DebugSetMute);
    RESOLVE(Direct3D9EnableMaximizedWindowedModeShim);
#undef RESOLVE
}

extern "C" void __cdecl EnsureInit()
{
    std::call_once(g_initOnce, Init);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);  // no LoadLibrary here: loader lock
        // Pin ourselves so we're never unloaded. Colonization loads d3d9.dll, runs a capability check and frees it
        // again; the system d3d9.dll we loaded stays resident, and the renderer's later LoadLibrary("d3d9.dll")
        // would then get that one by base name and bypass the proxy.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    }
    return TRUE;
}

// "<module>+0x<offset>" for a code address, e.g. "Civ4BeyondSword.exe+0x12345".
static std::string CodeLocation(void* address)
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module))
        GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    char buf[MAX_PATH + 32];
    snprintf(buf, sizeof(buf), "%s+0x%x", name,
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    return buf;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    EnsureInit();
    static LONG calls = 0;
    LONG n = InterlockedIncrement(&calls);
    std::string caller = CodeLocation(_ReturnAddress());
    Log::Write("Direct3DCreate9(%u) call %ld from %s, thread %lu", sdkVersion, n, caller.c_str(), GetCurrentThreadId());
    using Fn = IDirect3D9*(WINAPI*)(UINT);
    Fn real = g_real ? reinterpret_cast<Fn>(GetProcAddress(g_real, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d3d = real ? real(sdkVersion) : nullptr;
    Log::Write("Direct3DCreate9(%u) -> %s", sdkVersion, d3d ? "wrapped" : "NULL");
    return d3d ? new ProxyD3D9(d3d) : nullptr;
}

// Pass-through exports with no wrapping. Naked jumps keep any calling convention and argument list intact
// (the D3DPERF_* and debug entry points are rarely used by these games, and some are undocumented).
#define FORWARD(name)                                                                                              \
    extern "C" __declspec(naked) void Fwd_##name()                                                                 \
    {                                                                                                              \
        __asm call EnsureInit                                                                                      \
        __asm jmp dword ptr[g_fwd_##name]                                                                          \
    }

FORWARD(D3DPERF_BeginEvent)
FORWARD(D3DPERF_EndEvent)
FORWARD(D3DPERF_GetStatus)
FORWARD(D3DPERF_QueryRepeatFrame)
FORWARD(D3DPERF_SetMarker)
FORWARD(D3DPERF_SetOptions)
FORWARD(D3DPERF_SetRegion)
FORWARD(Direct3DCreate9Ex)  // not wrapped: neither game uses D3D9Ex
FORWARD(Direct3DShaderValidatorCreate9)
FORWARD(PSGPError)
FORWARD(PSGPSampleTexture)
FORWARD(DebugSetLevel)
FORWARD(DebugSetMute)
FORWARD(Direct3D9EnableMaximizedWindowedModeShim)
