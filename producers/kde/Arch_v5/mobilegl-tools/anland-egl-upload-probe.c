// Large texture upload probe: uploads a known pattern into a window-sized texture the way a
// compositor uploads a wl_shm buffer (full image, then a damaged sub-rectangle with
// GL_UNPACK_ROW_LENGTH), reads it back through an FBO and reports the first mismatching rows.
// Build: gcc anland-egl-upload-probe.c -o anland-egl-upload-probe -lEGL
#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 2560
#define H 1412

static void check(EGLBoolean ok, const char *operation) {
    if (!ok) { fprintf(stderr, "%s failed: EGL 0x%x\n", operation, eglGetError()); exit(1); }
}

#define LOAD(type, name) type name = (type)eglGetProcAddress(#name); if (!name) { fprintf(stderr, "no %s\n", #name); return 2; }

static unsigned char pattern(int x, int y, int c, int seed) {
    switch (c) {
    case 0: return (unsigned char)(x + seed);
    case 1: return (unsigned char)(y + seed);
    case 2: return (unsigned char)(((x >> 8) | ((y >> 8) << 4)) + seed);
    default: return 255;
    }
}

static int verify(const unsigned char *pixels, int seed, int x0, int y0, int w, int h, const char *label) {
    int bad = 0, firstBad = -1, lastBad = -1;
    for (int y = y0; y < y0 + h; ++y) {
        int rowBad = 0;
        for (int x = x0; x < x0 + w && !rowBad; ++x) {
            for (int c = 0; c < 4; ++c) {
                if (pixels[((size_t)y * W + x) * 4 + c] != pattern(x, y, c, seed)) { rowBad = 1; break; }
            }
        }
        if (rowBad) {
            if (firstBad < 0) {
                const unsigned char *p = pixels + ((size_t)y * W + x0) * 4;
                printf("  %s: first bad row %d: got %u,%u,%u,%u at x=%d want %u,%u,%u,%u\n", label, y,
                       p[0], p[1], p[2], p[3], x0, pattern(x0, y, 0, seed), pattern(x0, y, 1, seed),
                       pattern(x0, y, 2, seed), 255);
                // Which source row does the bad row hold? Read y back out of channel 1.
                printf("  %s: row %d holds green=%u (row %% 256 of its source)\n", label, y, p[1]);
                firstBad = y;
            }
            lastBad = y;
            ++bad;
            // Every 64th bad row (and the first few): decode where its first texel came from.
            if (bad <= 3 || bad % 64 == 0) {
                const unsigned char *p = pixels + ((size_t)y * W + x0) * 4;
                const int srcSeed7 = pattern(x0, 0, 0, 7) == p[0];
                const int seed = srcSeed7 ? 7 : 0;
                const int srcY = ((unsigned char)(p[1] - seed)) | ((((unsigned char)(p[2] - seed)) >> 4) << 8);
                printf("    row %d <- source row %d (seed %d)\n", y, srcY, seed);
            }
        }
    }
    printf("%s: %d bad rows of %d (first %d, last %d)\n", label, bad, h, firstBad, lastBad);
    return bad != 0;
}

int main(int argc, char **argv) {
    const int bgra = argc > 1 && strcmp(argv[1], "bgra") == 0;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(eglInitialize(display, NULL, NULL), "initialize");
    check(eglBindAPI(EGL_OPENGL_API), "bind OpenGL");
    EGLConfig config;
    EGLint count;
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    check(eglChooseConfig(display, configAttrs, &config, 1, &count) && count >= 1, "choose config");
    const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    check(context != EGL_NO_CONTEXT, "create context");
    const EGLint surfaceAttrs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
    check(surface != EGL_NO_SURFACE, "create pbuffer");
    check(eglMakeCurrent(display, surface, surface, context), "make current");

    LOAD(PFNGLGENTEXTURESPROC, glGenTextures)
    LOAD(PFNGLBINDTEXTUREPROC, glBindTexture)
    LOAD(PFNGLTEXIMAGE2DPROC, glTexImage2D)
    LOAD(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)
    LOAD(PFNGLTEXPARAMETERIPROC, glTexParameteri)
    LOAD(PFNGLPIXELSTOREIPROC, glPixelStorei)
    LOAD(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)
    LOAD(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)
    LOAD(PFNGLREADPIXELSPROC, glReadPixels)
    LOAD(PFNGLGETERRORPROC, glGetError)

    const GLenum format = bgra ? GL_BGRA : GL_RGBA;
    unsigned char *image = malloc((size_t)W * H * 4);
    unsigned char *readback = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c) {
                // Stored in the upload's channel order; verification reads RGBA.
                const int src = bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c;
                image[((size_t)y * W + x) * 4 + c] = pattern(x, y, src, 0);
            }

    GLuint texture, fbo;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, format, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, W);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, format, GL_UNSIGNED_BYTE, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    printf("format %s, fbo status 0x%x, error 0x%x\n", bgra ? "BGRA" : "RGBA",
           glCheckFramebufferStatus(GL_FRAMEBUFFER), glGetError());
    memset(readback, 0, (size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    int failed = verify(readback, 0, 0, 0, W, H, "full upload");

    // A damaged sub-rectangle, sourced from inside the full image with UNPACK_ROW_LENGTH and the
    // pointer offset to the rectangle's first texel - the compositor's partial shm upload.
    const int rx = 300, ry = 400, rw = 1500, rh = 700;
    for (int y = ry; y < ry + rh; ++y)
        for (int x = rx; x < rx + rw; ++x)
            for (int c = 0; c < 4; ++c) {
                const int src = bgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c;
                image[((size_t)y * W + x) * 4 + c] = pattern(x, y, src, 7);
            }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, W);
    glTexSubImage2D(GL_TEXTURE_2D, 0, rx, ry, rw, rh, format, GL_UNSIGNED_BYTE, image + ((size_t)ry * W + rx) * 4);
    memset(readback, 0, (size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    failed |= verify(readback, 7, rx, ry, rw, rh, "damaged rect");
    failed |= verify(readback, 0, 0, 0, W, ry, "rows above the rect");
    printf("GL error 0x%x\n", glGetError());
    return failed;
}
