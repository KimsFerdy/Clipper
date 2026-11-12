#include "app/NativeWindow.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dwmapi.h>
#include <windows.h>

#include <cstdio>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

namespace clipper {

#ifdef _WIN32

void initNativeThreads() {}

std::uintptr_t nativeWindow(GLFWwindow* window) { return reinterpret_cast<std::uintptr_t>(glfwGetWin32Window(window)); }

void attachParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

void setDarkTitleBar(GLFWwindow* window, bool dark) {
    const BOOL on = dark;
    DwmSetWindowAttribute(glfwGetWin32Window(window), 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof on);
}

#else

void initNativeThreads() { XInitThreads(); }
_XDisplay* x11Display() { return glfwGetX11Display(); }
std::uintptr_t nativeWindow(GLFWwindow* window) { return glfwGetX11Window(window); }

#endif

}  // namespace clipper
