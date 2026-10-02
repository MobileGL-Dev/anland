#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>

static void check(EGLBoolean ok, const char *operation) {
    if (!ok) { fprintf(stderr, "%s failed: EGL 0x%x\n", operation, eglGetError()); exit(1); }
}
int main(void) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(eglInitialize(display, NULL, NULL), "initialize");
    check(eglBindAPI(EGL_OPENGL_API), "bind OpenGL");
    EGLConfig config;
    EGLint count;
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    check(eglChooseConfig(display, configAttrs, &config, 1, &count) && count >= 1, "choose config");
    const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    check(context != EGL_NO_CONTEXT, "create context");
    const EGLint surfaceAttrs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
    check(surface != EGL_NO_SURFACE, "create pbuffer");
    check(eglMakeCurrent(display, surface, surface, context), "make current");
    PFNGLGETSTRINGPROC getString = (PFNGLGETSTRINGPROC)eglGetProcAddress("glGetString");
    PFNGLCLEARCOLORPROC clearColor = (PFNGLCLEARCOLORPROC)eglGetProcAddress("glClearColor");
    PFNGLCLEARPROC clear = (PFNGLCLEARPROC)eglGetProcAddress("glClear");
    PFNGLREADPIXELSPROC readPixels = (PFNGLREADPIXELSPROC)eglGetProcAddress("glReadPixels");
    PFNGLGETERRORPROC getError = (PFNGLGETERRORPROC)eglGetProcAddress("glGetError");
    if (!getString || !clearColor || !clear || !readPixels || !getError) return 2;
    printf("EGL: %s\nGL vendor: %s\nGL renderer: %s\nGL version: %s\n",
        eglQueryString(display, EGL_VENDOR), getString(GL_VENDOR), getString(GL_RENDERER), getString(GL_VERSION));
    clearColor(0.25f, 0.5f, 0.75f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] = {0};
    readPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = getError();
    printf("pixel RGBA=%u,%u,%u,%u GL error=0x%x\n", pixel[0], pixel[1], pixel[2], pixel[3], error);
    int result = error != GL_NO_ERROR || abs((int)pixel[0] - 64) > 1 ||
        abs((int)pixel[1] - 128) > 1 || abs((int)pixel[2] - 191) > 1 || pixel[3] != 255;
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release current");
    check(eglDestroySurface(display, surface), "destroy surface");
    check(eglDestroyContext(display, context), "destroy context");
    check(eglTerminate(display), "terminate");
    return result;
}
