#pragma once

// small helpers shared by the Win32*.cpp files. windows only

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace clipper::win32 {

// we keep utf-8 everywhere, windows wants utf-16
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// "KeePassXC" for C:\...\KeePassXC.exe, "" if the process can't be opened (elevated, gone)
std::string processName(DWORD pid);
std::string windowProcessName(HWND window);

std::wstring exePath();

// the AppBus window: owns the popup hotkey (Win32GlobalShortcut.cpp talks to it)
HWND appBusWindow();
constexpr UINT kMsgSetHotKey = WM_APP + 0x40;  // wParam = MOD_* flags, lParam = vk. 0 = unregister. returns 1 / 0

}  // namespace clipper::win32
