// HKCU\...\Run\Clipper = "<exe>" --minimized. shows up in Task Manager > Startup apps, where it can
// be turned off too: that's StartupApproved\Run\Clipper with an odd first byte

#include "platform/Autostart.h"
#include "platform/Win32Util.h"

namespace clipper {

namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr const wchar_t* kValue = L"Clipper";

bool disabledInTaskManager() {
    BYTE data[12] = {};
    DWORD size = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS)
        return false;
    return size > 0 && (data[0] & 1);
}

}  // namespace

bool isLaunchAtLoginEnabled() {
    DWORD size = 0;
    const bool present =
        RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
    return present && !disabledInTaskManager();
}

bool setLaunchAtLogin(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS r;
    if (enable) {
        const std::wstring command = L"\"" + win32::exePath() + L"\" --minimized";
        r = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    // turned on here = on, whatever Task Manager said before
    if (r == ERROR_SUCCESS) RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValue);
    return r == ERROR_SUCCESS;
}

}  // namespace clipper
