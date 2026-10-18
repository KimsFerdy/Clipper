#pragma once

struct GLFWwindow;
struct _XDisplay;

namespace clipper {

// the x11 bits behind glfw. own file because <X11/Xlib.h> #defines None, Status, Bool...
void initXlibThreads();  // before anything else talks to X
_XDisplay* x11Display();
unsigned long x11Window(GLFWwindow* window);

}  // namespace clipper
