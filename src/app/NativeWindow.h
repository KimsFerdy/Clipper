#pragma once

#include <cstdint>

struct GLFWwindow;
struct _XDisplay;

namespace clipper {

// the native bits behind glfw. own file because <X11/Xlib.h> / <windows.h> #define None, Status, Bool, near...
void initNativeThreads();                         // linux: XInitThreads, before anything else talks to X
std::uintptr_t nativeWindow(GLFWwindow* window);  // X11 Window / HWND

#ifdef _WIN32
void attachParentConsole();                           // so --help prints in the terminal it was run from
void setDarkTitleBar(GLFWwindow* window, bool dark);  // DWMWA_USE_IMMERSIVE_DARK_MODE
#else
_XDisplay* x11Display();
#endif

}  // namespace clipper
