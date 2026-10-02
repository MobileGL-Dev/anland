// Multi-window readback probe: the shape plasmashell has - several render threads, each with its
// own context and its own surface, each reading back its whole frame at swap. Every thread paints
// row-identifiable stripes (scissored clears) into its own pbuffer and reads the full surface back;
// a row that carries another thread's colour, or the wrong stripe, is reported.
// Build: gcc -O1 anland-egl-mt-readback-probe.c -o mt-probe -lEGL -lpthread
#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EGLDisplay g_display;
static EGLConfig g_config;
static int g_iterations = 30;

typedef struct {
    int id, width, height, bad, badFirst;
} Job;

#define LOAD(type, name) type name = (type)eglGetProcAddress(#name)

static void *run(void *arg) {
    Job *job = arg;
    const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, contextAttrs);
    const EGLint surfaceAttrs[] = {EGL_WIDTH, job->width, EGL_HEIGHT, job->height, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(g_display, g_config, surfaceAttrs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(g_display, surface, surface, context)) {
        fprintf(stderr, "thread %d: setup failed 0x%x\n", job->id, eglGetError());
        job->bad = -1;
        return NULL;
    }
    LOAD(PFNGLCLEARCOLORPROC, glClearColor);
    LOAD(PFNGLCLEARPROC, glClear);
    LOAD(PFNGLSCISSORPROC, glScissor);
    LOAD(PFNGLENABLEPROC, glEnable);
    LOAD(PFNGLDISABLEPROC, glDisable);
    LOAD(PFNGLREADPIXELSPROC, glReadPixels);
    unsigned char *pixels = malloc((size_t)job->width * job->height * 4);
    job->badFirst = -1;
    for (int it = 0; it < g_iterations; ++it) {
        // Release and re-acquire every frame, as Qt's render loop does around a frame.
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (!eglMakeCurrent(g_display, surface, surface, context)) { job->bad = -2; break; }
        glEnable(GL_SCISSOR_TEST);
        for (int y = 0; y < job->height; y += 32) {
            glScissor(0, y, job->width, 32);
            // red = thread id, green = stripe index, blue = iteration
            glClearColor(job->id / 255.0f, (y / 32 % 256) / 255.0f, (it % 256) / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        memset(pixels, 0, (size_t)job->width * job->height * 4);
        glReadPixels(0, 0, job->width, job->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (int y = 0; y < job->height; ++y) {
            const unsigned char *p = pixels + ((size_t)y * job->width + job->width / 2) * 4;
            if (p[0] != job->id || p[1] != (y / 32 % 256) || p[2] != (it % 256)) {
                if (job->badFirst < 0) {
                    job->badFirst = y;
                    printf("thread %d it %d: row %d holds id=%u stripe=%u iter=%u (want %d,%d,%d)\n", job->id, it, y,
                           p[0], p[1], p[2], job->id, y / 32 % 256, it % 256);
                }
                ++job->bad;
            }
        }
        eglSwapBuffers(g_display, surface);
    }
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_display, surface);
    eglDestroyContext(g_display, context);
    free(pixels);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) g_iterations = atoi(argv[1]);
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) return 2;
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(g_display, configAttrs, &g_config, 1, &count) || count < 1) return 2;
    Job jobs[3] = {{1, 2560, 1412}, {2, 2560, 200}, {3, 800, 600}};
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i) pthread_create(&threads[i], NULL, run, &jobs[i]);
    int failed = 0;
    for (int i = 0; i < 3; ++i) {
        pthread_join(threads[i], NULL);
        printf("thread %d (%dx%d): %d bad rows over %d frames\n", jobs[i].id, jobs[i].width, jobs[i].height,
               jobs[i].bad, g_iterations);
        failed |= jobs[i].bad != 0;
    }
    eglTerminate(g_display);
    return failed;
}
