#include "app/NativeWindow.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

namespace clipper {

void initXlibThreads() { XInitThreads(); }
_XDisplay* x11Display() { return glfwGetX11Display(); }
unsigned long x11Window(GLFWwindow* window) { return glfwGetX11Window(window); }

}  // namespace clipper
