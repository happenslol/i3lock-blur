#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <err.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "blur.h"

extern Display *display;
static bool initialized = false;

static void swap(int *a, int *b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

#if DEBUG_GL
void printShaderInfoLog(GLuint obj) {
    int infologLength = 0;
    int charsWritten = 0;
    char *infoLog;

    glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &infologLength);

    if (infologLength > 0) {
        infoLog = (char *)malloc(infologLength);
        glGetShaderInfoLog(obj, infologLength, &charsWritten, infoLog);
        printf("shader_infolog: %s\n", infoLog);
        free(infoLog);
    }
}

void printProgramInfoLog(GLuint obj) {
    int infologLength = 0;
    int charsWritten = 0;
    char *infoLog;

    glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &infologLength);

    if (infologLength > 0) {
        infoLog = (char *)malloc(infologLength);
        glGetProgramInfoLog(obj, infologLength, &charsWritten, infoLog);
        printf("program_infolog: %s\n", infoLog);
        free(infoLog);
    }
}
#endif

static const char *VERT_SHADER = R"(
varying vec2 v_Coordinates;

void main(void) {
    gl_Position = ftransform();
    v_Coordinates = vec2(gl_MultiTexCoord0);
}
)";

static const char *FRAG_SHADER = R"(
#version 120

varying vec2 v_Coordinates;
uniform sampler2D u_Texture0;

void main() {
    gl_FragColor = texture2D(u_Texture0, v_Coordinates) * vec4(0.9, 0.9, 0.9, 1.0);
}
)";

GLXFBConfig *configs = NULL;
GLXContext ctx;

Pixmap pixmap_buffers[2];
GLXPixmap glx_buffers[2];

XVisualInfo *vis;

GLuint shader;
GLuint vertex_shader;
GLuint fragment_shader;

static PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT_f = NULL;
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT_f = NULL;

const int pixmap_config[] = {
    GLX_BIND_TO_TEXTURE_RGBA_EXT,
    True,
    GLX_DRAWABLE_TYPE,
    GLX_PIXMAP_BIT,
    GLX_BIND_TO_TEXTURE_TARGETS_EXT,
    GLX_TEXTURE_2D_BIT_EXT,
    GLX_DOUBLEBUFFER,
    False,
    GLX_Y_INVERTED_EXT,
    GLX_DONT_CARE,
    None
};

const int pixmap_attribs[] = {
    GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
    GLX_TEXTURE_FORMAT_EXT,
    GLX_TEXTURE_FORMAT_RGB_EXT, None
};

void glx_init(int screen, int w, int h) {
    if (initialized) return;

    int config_count;
    configs = glXChooseFBConfig(display, screen, pixmap_config, &config_count);
    vis = glXGetVisualFromFBConfig(display, configs[0]);
    ctx = glXCreateContext(display, vis, NULL, True);

    glXBindTexImageEXT_f = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress(
        (GLubyte *)"glXBindTexImageEXT"
    );
    if (glXBindTexImageEXT_f == NULL)
        errx(EXIT_FAILURE, "Failed to load extension glXBindTexImageEXT.\n");

    glXReleaseTexImageEXT_f = (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress(
        (GLubyte *)"glXReleaseTexImageEXT"
    );
    if (glXReleaseTexImageEXT_f == NULL)
        errx(EXIT_FAILURE, "Failed to load extension glXReleaseTexImageEXT.\n");

    // Initialize our buffers
    for (uint8_t i = 0; i < 2; ++i) {
        pixmap_buffers[i] = XCreatePixmap(
            display,
            RootWindow(display, vis->screen),
            w, h,
            vis->depth
        );

        glx_buffers[i] = glXCreatePixmap(
            display,
            configs[0],
            pixmap_buffers[i],
            pixmap_attribs
        );
    }

    glXMakeCurrent(display, glx_buffers[0], ctx);

    vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &VERT_SHADER, NULL);
    glCompileShader(vertex_shader);

#if DEBUG_GL
    int vertex_compile_status;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &vertex_compile_status);
    printf("vertex shader status: %d\n", vertex_compile_status);
    printShaderInfoLog(vertex_shader);
#endif

    fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &FRAG_SHADER, NULL);
    glCompileShader(fragment_shader);

#if DEBUG_GL
    int fragment_compile_status;
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fragment_compile_status);
    printf("fragment shader status: %d\n", fragment_compile_status);
    printShaderInfoLog(fragment_shader);
#endif

    shader = glCreateProgram();
    glAttachShader(shader, vertex_shader);
    glAttachShader(shader, fragment_shader);
    glLinkProgram(shader);

#if DEBUG_GL
    int program_link_status;
    glGetShaderiv(fragment_shader, GL_LINK_STATUS, &program_link_status);
    printf("program link status: %d\n", program_link_status);
    printProgramInfoLog(shader);
#endif

    initialized = true;
}

static void glx_free_pixmaps(void) {
    if (!initialized) return;

    for (uint8_t i = 0; i < 2; ++i) {
        XFreePixmap(display, pixmap_buffers[i]);
        glXDestroyPixmap(display, glx_buffers[i]);
    }
}

void glx_resize(int w, int h) {
    if (!initialized) return;

    /* free old pixmaps */
    glx_free_pixmaps();

    /* create new pixmaps */
    for (uint8_t i = 0; i < 2; ++i) {
        pixmap_buffers[i] = XCreatePixmap(
            display,
            RootWindow(display, vis->screen),
            w, h,
            vis->depth
        );

        glx_buffers[i] = glXCreatePixmap(
            display,
            configs[0],
            pixmap_buffers[i],
            pixmap_attribs
        );
    }

    glXMakeCurrent(display, glx_buffers[0], ctx);
}

void glx_deinit(void) {
    if (!initialized) return;

    glx_free_pixmaps();

    glDetachShader(shader, vertex_shader);
    glDetachShader(shader, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    glDeleteProgram(shader);
    glXDestroyContext(display, ctx);

    XFree(vis);
    XFree(configs);

    initialized = false;
}

void post_process_pixmap(int screen, Pixmap pixmap, int width, int height) {
    if (!initialized) return;

    bool source = 0;
    bool target = 1;
    int iterations = 1;

    // destroy the existing pixmap and create a new one in its place
    glXDestroyPixmap(display, glx_buffers[source]);
    glx_buffers[source] = glXCreatePixmap(display, configs[0], pixmap, pixmap_attribs);

    for (uint8_t i = 0; i < iterations; ++i) {
        // Start out with the given source and target,
        // swap buffers for all subsequent iterations
        if (i > 0) swap(&source, &target);

        glXMakeCurrent(display, glx_buffers[target], ctx);
        glEnable(GL_TEXTURE_2D);
        glXBindTexImageEXT_f(display, glx_buffers[source], GLX_FRONT_EXT, NULL);

        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

        glViewport(0, 0, (GLsizei)width, (GLsizei)height);
        glClearColor(0.0, 0.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glUseProgram(shader);

        // TODO: Set uniforms here

        glBegin(GL_QUADS);
            glTexCoord2f(0.0, 0.0);
            glVertex2f(-1.0, 1.0);

            glTexCoord2f(1.0, 0.0);
            glVertex2f(1.0, 1.0);

            glTexCoord2f(1.0, 1.0);
            glVertex2f(1.0, -1.0);

            glTexCoord2f(0.0, 1.0);
            glVertex2f(-1.0, -1.0);
        glEnd();

        glFlush();
    }

    GC gc = XCreateGC(display, pixmap, 0, NULL);
    XCopyArea(display, pixmap_buffers[target], pixmap, gc, 0, 0, width, height, 0, 0);
    XFreeGC(display, gc);
}
