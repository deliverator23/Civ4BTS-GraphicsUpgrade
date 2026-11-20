// Keeps GraphicsUpgrade's hotkeys from the game: a message hook on the game window's thread blanks the key's messages
// when exactly the hotkey modifiers are held, so Ctrl+Shift+1 doesn't also reach BtS as Ctrl+1 or Shift+1 (unit
// groups) and Ctrl+Shift+/ as / (next worker). BtS reads its keys from window messages, as do mods' Python key
// handlers. The device's own GetAsyncKeyState polling still sees the keys.
#pragma once

#include "common.h"

namespace HotkeyFilter
{
// Starts dropping these keys (0 entries are ignored) on the thread that owns window (the calling thread if it has
// none). Calling it again replaces the keys and the thread. No keys: stops.
void Start(HWND window, const std::vector<UINT>& keys);
void Stop();
}  // namespace HotkeyFilter
