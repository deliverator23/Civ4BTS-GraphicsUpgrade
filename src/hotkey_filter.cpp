#include "hotkey_filter.h"

namespace
{
HHOOK g_hook = nullptr;
bool g_keys[256] = {};  // the hotkeys
bool g_held[256] = {};  // a hotkey whose press was dropped: its repeats and release go too, whatever the modifiers do

bool Down(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

// The same test as the device's polling: exactly the configured modifiers, Alt the left one only (AltGr is Ctrl+Alt).
bool ModifiersMatch()
{
    const bool alt = Down(VK_LMENU) && !Down(VK_RMENU);
    return Down(VK_CONTROL) == g_config.keyCtrl && Down(VK_SHIFT) == g_config.keyShift && alt == g_config.keyAlt;
}

// Called as the thread's GetMessage or PeekMessage hands over a message (wParam PM_REMOVE: taken from the queue;
// PM_NOREMOVE: only looked at). A hotkey's key message becomes WM_NULL, before TranslateMessage could make a WM_CHAR of
// it or the window procedure could see it.
LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    MSG* msg = reinterpret_cast<MSG*>(lParam);
    if (code == HC_ACTION && msg &&
        (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP || msg->message == WM_SYSKEYDOWN ||
         msg->message == WM_SYSKEYUP) &&
        msg->wParam < 256)
    {
        const UINT vk = static_cast<UINT>(msg->wParam);
        const bool release = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
        const bool removed = (wParam & PM_REMOVE) != 0;
        bool drop = false;
        if (g_held[vk])
        {
            drop = true;
            if (release && removed)
                g_held[vk] = false;
        }
        else if (!release && g_keys[vk] && ModifiersMatch())
        {
            drop = true;
            if (removed)
                g_held[vk] = true;
        }
        if (drop)
            msg->message = WM_NULL;
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}
}  // namespace

namespace HotkeyFilter
{
void Start(HWND window, const std::vector<UINT>& keys)
{
    Stop();
    bool any = false;
    for (UINT k : keys)
        if (k > 0 && k < 256)
            g_keys[k] = any = true;
    if (!any)
        return;
    DWORD pid = 0;
    DWORD thread = window ? GetWindowThreadProcessId(window, &pid) : 0;
    if (thread == 0 || pid != GetCurrentProcessId())
        thread = GetCurrentThreadId();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetMsgProc), &self);
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, self, thread);
    if (g_hook)
        Log::Write("hotkeys kept from the game (message hook on thread %lu)", thread);
    else
        Log::Write("hotkeys: message hook failed (error %lu); the game sees them too", GetLastError());
}

void Stop()
{
    if (g_hook)
        UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    for (int i = 0; i < 256; ++i)
        g_keys[i] = g_held[i] = false;
}
}  // namespace HotkeyFilter
