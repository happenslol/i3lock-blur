#include <X11/Xlib.h>
#include <cairo.h>
#include <xcb/xcb.h>

void post_process_pixmap(int screen, Pixmap pixmap, int width, int height);
void glx_init(int screen, int w, int h);
void glx_deinit(void);
void glx_resize(int w, int h);
