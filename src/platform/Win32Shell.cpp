#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include "platform/Shell.h"

#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>

namespace clipper {

bool showInFileManager(const std::string& path) {
    const std::wstring wide = win32::widen(path);
    // explorer window on the parent folder with the file selected
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(wide.c_str())) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        if (SUCCEEDED(co)) CoUninitialize();
        ILFree(item);
        if (SUCCEEDED(hr)) return true;
    }
    // gone: open whatever is left of the folder
    const std::wstring parent = std::filesystem::path(wide).parent_path().wstring();
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

}  // namespace clipper
