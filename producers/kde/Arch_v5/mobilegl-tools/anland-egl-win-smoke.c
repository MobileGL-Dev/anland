/* Window-surface variant of anland-egl-smoke: exercises the exact KWin path
 * (WINDOW_BIT config, desktop GL core context, server-owned window surface,
 * makeCurrent + clear + eglSwapBuffers). Requires MOBILEGL_IPC_SURFACE=server
 * and the embedded server's display to be attached. */
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
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    check(eglChooseConfig(display, configAttrs, &config, 1, &count) && count >= 1, "choose config");
    const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    check(context != EGL_NO_CONTEXT, "create context");
    EGLSurface surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    check(surface != EGL_NO_SURFACE, "create window surface");
    check(eglMakeCurrent(display, surface, surface, context), "make current");
    PFNGLGETSTRINGPROC getString = (PFNGLGETSTRINGPROC)eglGetProcAddress("glGetString");
    PFNGLCLEARCOLORPROC clearColor = (PFNGLCLEARCOLORPROC)eglGetProcAddress("glClearColor");
    PFNGLCLEARPROC clear = (PFNGLCLEARPROC)eglGetProcAddress("glClear");
    PFNGLGETERRORPROC getError = (PFNGLGETERRORPROC)eglGetProcAddress("glGetError");
    if (!getString || !clearColor || !clear || !getError) return 2;
    printf("GL renderer: %s\nGL version: %s\n", getString(GL_RENDERER), getString(GL_VERSION));
    for (int i = 0; i < 60; i++) {
        clearColor(0.1f * (i % 3), 0.8f, 0.2f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        check(eglSwapBuffers(display, surface), "swap buffers");
    }
    printf("60 frames swapped, GL error=0x%x\n", getError());
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release current");
    check(eglDestroySurface(display, surface), "destroy surface");
    check(eglDestroyContext(display, context), "destroy context");
    check(eglTerminate(display), "terminate");
    return 0;
}
