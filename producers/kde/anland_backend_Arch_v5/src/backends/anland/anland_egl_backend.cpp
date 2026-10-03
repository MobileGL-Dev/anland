/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_egl_backend.h"
#include "anland_backend.h"
#include "anland_logging.h"
#include "anland_output.h"

#include "core/graphicsbuffer.h" // DmaBufAttributes
#include "core/drmdevice.h"
#include "core/output.h" // OutputTransform
#include "core/renderdevice.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/eglnativefence.h"
#include "opengl/glutils.h"
#include "utils/filedescriptor.h"

#include <QTimer>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace KWin
{

// How many EGL backends have initialized in this process (see AnlandEglBackend::init).
static int s_initializedBackends = 0;

static uint32_t protocol_format_to_drm(uint32_t fmt)
{
    switch (fmt) {
    case 1:
        return DRM_FORMAT_ABGR8888;
    default:
        return DRM_FORMAT_XRGB8888;
    }
}

AnlandEglLayer::AnlandEglLayer(AnlandOutput *output, AnlandEglBackend *backend)
    : OutputLayer(output, OutputLayerType::Primary)
    , m_backend(backend)
    , m_output(output)
    , m_display(backend->display())
{
    connect(m_output, &BackendOutput::transformChanged, this, &AnlandEglLayer::onOutputTransformChanged);
    if (m_backend->backend()->usesMobileGl()) {
        // The server window changes size under the compositor (the consumer's layout moved: the
        // extra-keys bar shown or hidden, a rotation) whether or not anything here is being drawn, so
        // the size is polled rather than read inside a frame: a frame already begun has its scene laid
        // out for the old output, and drawing it into the new buffer is a ghost. A poll asks MobileGL
        // for what the server published, nothing more.
        m_surfaceSizePoll = new QTimer(this);
        m_surfaceSizePoll->setInterval(250);
        connect(m_surfaceSizePoll, &QTimer::timeout, this, &AnlandEglLayer::followMobileGlSurfaceSize);
        m_surfaceSizePoll->start();
    }
}

void AnlandEglLayer::followMobileGlSurfaceSize()
{
    if (m_bufCount == 0 || !m_fbos[0]) {
        return;
    }
    const EGLDisplay display = m_backend->eglDisplayObject()->handle();
    const EGLSurface surface = m_backend->eglDisplayObject()->defaultSurface();
    EGLint width = 0;
    EGLint height = 0;
    if (surface == EGL_NO_SURFACE || !eglQuerySurface(display, surface, EGL_WIDTH, &width)
        || !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        return;
    }
    const QSize size(width, height);
    if (!size.isValid() || size == m_output->modeSize()) {
        return;
    }
    qCInfo(KWIN_ANLAND) << "MobileGL surface is now" << size << "- resizing the output";
    // Between frames, so the next one is laid out for the new output and drawn into the whole buffer.
    m_output->resize(size);
    m_fbos[0] = std::make_unique<GLFramebuffer>(0, size);
    m_damageJournal.clear();
    addDeviceRepaint(Region::infinite());
}

Region AnlandEglLayer::mobileGlRepaint()
{
    EglDisplay *eglDisplay = m_backend->eglDisplayObject();
    if (!eglDisplay->supportsBufferAge()) {
        return Region::infinite();
    }
    // Asked before anything is drawn: the server answers for the buffer this frame's draws land in
    // (on Espryt the driver's, on Magma the swapchain image's), 0 when it does not know.
    EGLint age = 0;
    if (!eglQuerySurface(eglDisplay->handle(), eglDisplay->defaultSurface(), EGL_BUFFER_AGE_EXT, &age)) {
        return Region::infinite();
    }
    return m_damageJournal.accumulate(age, Region::infinite());
}

Region AnlandEglLayer::adjustRenderRegion(const Region &region)
{
    if (!m_backend->backend()->usesMobileGl() || m_bufCount == 0) {
        return region;
    }
    EglDisplay *eglDisplay = m_backend->eglDisplayObject();
    static const bool partialUpdate = eglDisplay->hasExtension(QByteArrayLiteral("EGL_KHR_partial_update"));
    if (!partialUpdate || !eglDisplay->supportsBufferAge()) {
        return region;
    }
    // EGL_KHR_partial_update: the buffer keeps its content only outside the region declared here,
    // so the region must be what this frame paints - every pixel of it. Drivers honour a damage
    // region by its bounding box (pixels inside it that are not drawn come out undefined), so the
    // frame paints, and declares, one rectangle: the bounds of what it was going to paint.
    const QSize size = m_fbos[0] ? m_fbos[0]->size() : m_output->modeSize();
    const Rect whole(0, 0, size.width(), size.height());
    Region paint = region & whole;
    if (!paint.isEmpty()) {
        paint = Region(paint.boundingRect());
    }
    QList<EGLint> rects;
    if (paint != Region(whole)) {
        // KWin's device rectangles run from the top-left; EGL's from the bottom-left.
        for (const Rect &rect : paint.rects()) {
            rects << rect.x() << size.height() - rect.y() - rect.height() << rect.width() << rect.height();
        }
    }
    if (paint.isEmpty()) {
        // EGL cannot declare "nothing" (no rectangles is the whole surface): declare one pixel and
        // have it painted.
        paint = Region(Rect(0, 0, 1, 1));
        rects = {0, size.height() - 1, 1, 1};
    }
    eglSetDamageRegionKHR(eglDisplay->handle(), eglDisplay->defaultSurface(), rects.isEmpty() ? nullptr : rects.data(),
                          rects.size() / 4);
    return paint;
}

bool AnlandEglLayer::mobileGlSwap(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion)
{
    EglDisplay *eglDisplay = m_backend->eglDisplayObject();
    const EGLDisplay display = eglDisplay->handle();
    const EGLSurface surface = eglDisplay->defaultSurface();
    const QSize size = m_fbos[0] ? m_fbos[0]->size() : m_output->modeSize();
    // Everything the frame painted, not only what changed: a driver may write back no more of the
    // buffer than the swap damage, and the repairs buffer age asked for are paint too.
    const Region damage = (renderedDeviceRegion | damagedDeviceRegion) & Rect(0, 0, size.width(), size.height());
    static const bool withDamage = eglDisplay->hasExtension(QByteArrayLiteral("EGL_KHR_swap_buffers_with_damage"));
    bool swapped;
    // An empty damage has no EGL spelling (no rectangles means the whole surface): a plain swap.
    if (withDamage && !damage.isEmpty() && damage != Region(Rect(0, 0, size.width(), size.height()))) {
        // KWin's device rectangles run from the top-left; EGL's from the bottom-left.
        QList<EGLint> rects;
        const auto damageRects = damage.rects();
        rects.reserve(damageRects.size() * 4);
        for (const Rect &rect : damageRects) {
            rects << rect.x() << size.height() - rect.y() - rect.height() << rect.width() << rect.height();
        }
        swapped = eglSwapBuffersWithDamageKHR(display, surface, rects.data(), rects.size() / 4);
    } else {
        swapped = eglSwapBuffers(display, surface);
    }
    if (swapped) {
        m_damageJournal.add(damagedDeviceRegion);
    } else {
        // Which buffer holds what is no longer known.
        m_damageJournal.clear();
    }
    return swapped;
}

AnlandEglLayer::~AnlandEglLayer()
{
    releaseBuffers();
}

void AnlandEglLayer::releaseBuffers()
{
    if (auto *context = m_backend->openglContext()) {
        context->makeCurrent();
    }

    for (int i = 0; i < MAX_BUFS; i++) {
        m_fbos[i].reset();
        m_textures[i].reset();
        m_accumDamage[i] = Region();
}
    m_damageJournal.clear();
    m_bufCount = 0;
}

bool AnlandEglLayer::importBuffers(int count)
{
    if (m_backend->backend()->usesMobileGl()) {
        buf_info info{};
        if (count != 1 || get_dmabuf_info_at(m_display, 0, &info) < 0
            || info.format != ANLAND_FORMAT_MOBILEGL_SURFACE) {
            qCWarning(KWIN_ANLAND) << "consumer did not advertise a MobileGL Surface";
            releaseBuffers();
            return false;
        }
        releaseBuffers();
        const QSize size(info.width, info.height);
        if (!size.isValid()) {
            return false;
        }
        if (size != m_output->modeSize()) {
            m_output->resize(size);
        }
        m_fbos[0] = std::make_unique<GLFramebuffer>(0, size);
        m_accumDamage[0] = Region::infinite();
        m_bufCount = 1;
        return true;
    }
    if (count <= 0 || count > MAX_BUFS) {
        qCWarning(KWIN_ANLAND) << "invalid dmabuf count" << count;
        releaseBuffers();
        return false;
    }

    auto *context = m_backend->openglContext();
    if (!context || !context->makeCurrent()) {
        qCWarning(KWIN_ANLAND) << "cannot make the EGL context current while importing dmabufs";
        releaseBuffers();
        return false;
    }

    releaseBuffers();

    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);

    for (int i = 0; i < count; i++) {
        const int fd = get_dmabuf_fd_at(m_display, i);
        buf_info info;
        if (fd < 0 || get_dmabuf_info_at(m_display, i, &info) < 0) {
            qCWarning(KWIN_ANLAND) << "failed to get dmabuf info for buffer" << i;
            releaseBuffers();
            return false;
        }

        if (i == 0) {
            const QSize bufSize(info.width, info.height);
            if (bufSize != m_output->modeSize() && bufSize.isValid()) {
                qCInfo(KWIN_ANLAND) << "dmabuf size changed, resizing output to" << bufSize;
                m_output->resize(bufSize);
            }
        }
        const QSize actual(info.width, info.height);

        DmaBufAttributes attrs;
        attrs.planeCount = 1;
        attrs.width = actual.width();
        attrs.height = actual.height();
        attrs.format = protocol_format_to_drm(info.format);
        attrs.modifier = info.modifier;
        const int importedFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (importedFd < 0) {
            qCWarning(KWIN_ANLAND) << "failed to duplicate dmabuf" << i;
            releaseBuffers();
            return false;
        }
        attrs.fd[0] = FileDescriptor(importedFd);
        attrs.offset[0] = static_cast<int>(info.offset);
        attrs.pitch[0] = static_cast<int>(info.stride);

        std::shared_ptr<GLTexture> texture = m_backend->importDmaBufAsTexture(attrs);
        if (!texture) {
            qCWarning(KWIN_ANLAND) << "failed to import dmabuf" << i << "as texture";
            releaseBuffers();
            return false;
        }

        texture->setContentTransform(contentTransform);
        auto fbo = std::make_unique<GLFramebuffer>(texture.get());
        if (!fbo->valid()) {
            qCWarning(KWIN_ANLAND) << "framebuffer for dmabuf" << i << "is not complete";
            releaseBuffers();
            return false;
        }

        qCDebug(KWIN_ANLAND) << "imported buffer" << i << "fd" << fd << actual
                             << "fmt" << Qt::hex << attrs.format << "mod" << attrs.modifier;

        m_textures[i] = std::move(texture);
        m_fbos[i] = std::move(fbo);
        m_accumDamage[i] = Region::infinite();
    }

    m_bufCount = count;
    return true;
}
void AnlandEglLayer::onOutputTransformChanged()
{
    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);
    for (int i = 0; i < m_bufCount; i++) {
        if (m_textures[i]) {
            m_textures[i]->setContentTransform(contentTransform);
        }
        m_accumDamage[i] = Region::infinite();
    }
    m_damageJournal.clear();
    addDeviceRepaint(Region::infinite());
}

std::optional<OutputLayerBeginFrameInfo> AnlandEglLayer::doBeginFrame()
{
    auto *context = m_backend->openglContext();
    if (!context || !context->makeCurrent()) {
        return std::nullopt;
    }

    if (m_bufCount == 0) {
        return std::nullopt;
    }

    m_currentIndex = get_selected_idx(m_display);
    if (m_currentIndex < 0 || m_currentIndex >= m_bufCount) {
        m_currentIndex = 0;
    }

    if (m_backend->backend()->usesMobileGl() && m_fbos[0]->size() != m_output->modeSize()) {
        // The frame's target is always the output's mode, which the scene is laid out for: a
        // viewport of another size draws the scene shifted, or leaves rows of the buffer unwritten.
        m_fbos[0] = std::make_unique<GLFramebuffer>(0, m_output->modeSize());
        m_damageJournal.clear();
    }

    return OutputLayerBeginFrameInfo{
        .renderTarget = m_backend->backend()->usesMobileGl()
            // No FlipY here: eglSwapBuffers into the server-owned Android window is
            // already top-down; the flip is only correct for the dmabuf path,
            // where the consumer's blit applies its own transform.
            ? RenderTarget(m_fbos[0].get(), m_output->transform())
            : RenderTarget(m_fbos[m_currentIndex].get()),
        // Android's BufferQueue (Espryt) or the swapchain (Magma) rotates the server window's
        // buffers; the buffer age MobileGL reports says which of the journal's frames this one
        // has missed.
        .repaint = m_backend->backend()->usesMobileGl() ? mobileGlRepaint() : m_accumDamage[m_currentIndex],
    };
}

bool AnlandEglLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    Q_UNUSED(frame)
    if (m_bufCount == 0 || !m_backend->openglContext()) {
        return false;
    }
    if (m_backend->backend()->usesMobileGl()) {
        const bool swapped = mobileGlSwap(renderedDeviceRegion, damagedDeviceRegion);
        const EGLint error = swapped ? EGL_SUCCESS : eglGetError();
        set_render_fence(m_display, -1);
        if (error == EGL_CONTEXT_LOST) {
            // MobileGL lost this compositor's session (a device loss on the server). The next
            // frame's reset check (EglBackend::checkGraphicsReset) restarts compositing on
            // contexts of a fresh session, so that frame has to come even on an idle desktop.
            qCWarning(KWIN_ANLAND) << "MobileGL swap failed: the session is lost; compositing restarts on a fresh one";
            addDeviceRepaint(Region::infinite());
        } else if (!swapped) {
            qCWarning(KWIN_ANLAND) << "MobileGL swap failed" << Qt::hex << error;
        }
        return swapped;
    }
    glFlush();
    for (int i = 0; i < m_bufCount; i++) {
        m_accumDamage[i] = m_accumDamage[i] + damagedDeviceRegion;
    }
    if (m_currentIndex < m_bufCount) {
        m_accumDamage[m_currentIndex] = Region();
    }

    for (int i = 0; i < m_bufCount; i++) {
        if (!m_accumDamage[i].isEmpty()) {
            addDeviceRepaint(Region::infinite());
            break;
        }
    }

    EGLNativeFence fence{m_backend->eglDisplayObject()};
    set_render_fence(m_display, fence.takeFileDescriptor().take());
    return true;
}

DrmDevice *AnlandEglLayer::scanoutDevice() const
{
    // The MobileGL output is an EGL window surface on the server's Android
    // Surface: no client buffer can be put on it directly, so clients get no
    // scanout tranche towards the (identity-only) DRM device.
    if (m_backend->backend()->usesMobileGl()) {
        return nullptr;
    }
    return m_backend->drmDevice();
}

FormatModifierMap AnlandEglLayer::supportedDrmFormats() const
{
    return m_backend->supportedFormats();
}

std::shared_ptr<GLTexture> AnlandEglLayer::texture() const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_bufCount) {
        return nullptr;
    }
    return m_textures[m_currentIndex];
}

AnlandEglBackend::AnlandEglBackend(AnlandBackend *b)
    : m_backend(b)
{
}

AnlandEglBackend::~AnlandEglBackend()
{
    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        static_cast<AnlandOutput *>(output)->setEglLayer(nullptr);
    }
    cleanup();
}

display_ctx *AnlandEglBackend::display() const
{
    return m_backend->display();
}

DrmDevice *AnlandEglBackend::drmDevice() const
{
    return m_backend->drmDevice();
}

bool AnlandEglBackend::initializeEgl()
{
    if (!m_backend->usesMobileGl() && !initClientExtensions()) {
        return false;
    }
    if (!m_backend->renderDevice()) {
        qCWarning(KWIN_ANLAND) << "backend has no render device";
        return false;
    }
    setRenderDevice(m_backend->renderDevice());
    return true;
}
bool AnlandEglBackend::init()
{
    if (!initializeEgl()) {
        qCWarning(KWIN_ANLAND) << "Could not initialize egl";
        return false;
    }
    if (!createContext()) {
        qCWarning(KWIN_ANLAND) << "Could not initialize rendering context";
        return false;
    }

    // A backend after the first is compositing restarted, which KWin does when its context reports
    // a reset: under MobileGL, this compositor's session was lost and the context just created is
    // on a fresh one (its window surface follows at the first make-current). The images imported
    // from clients' buffers were the lost session's, so they are dropped and each buffer is
    // imported again when it is next drawn; the clients' images live in their own sessions.
    if (s_initializedBackends++ > 0 && m_backend->usesMobileGl()) {
        qCInfo(KWIN_ANLAND) << "compositing restarted on a fresh MobileGL session; client buffers are imported again";
        eglDisplayObject()->dropImportedImages();
    }

    initWayland();

    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        addOutput(output);
    }

    connect(m_backend, &AnlandBackend::outputAdded, this, &AnlandEglBackend::addOutput);
    return true;
}

void AnlandEglBackend::addOutput(BackendOutput *output)
{
    openglContext()->makeCurrent();
    auto *anlandOutput = static_cast<AnlandOutput *>(output);
    anlandOutput->setEglLayer(std::make_unique<AnlandEglLayer>(anlandOutput, this));

    // The consumer may have connected before the GL backend created its
    // output layer. Import the already-held dmabufs here as well as on the
    // reconnect path, otherwise the first connection would never render.
    if (m_backend->isConsumerConnected()) {
        m_backend->importBuffers(anlandOutput->eglLayer());
    }
}

QList<OutputLayer *> AnlandEglBackend::compatibleOutputLayers(BackendOutput *output)
{
    return {static_cast<AnlandOutput *>(output)->eglLayer()};
}

} // namespace KWin

#include "moc_anland_egl_backend.cpp"
