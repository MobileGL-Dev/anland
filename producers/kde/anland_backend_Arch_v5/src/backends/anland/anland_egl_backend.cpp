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

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace KWin
{

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

    return OutputLayerBeginFrameInfo{
        .renderTarget = m_backend->backend()->usesMobileGl()
            ? RenderTarget(m_fbos[0].get(), m_output->transform().combine(OutputTransform::FlipY))
            : RenderTarget(m_fbos[m_currentIndex].get()),
        // Android's BufferQueue rotates its own buffers; until buffer age is
        // exposed by the split client every swap needs a complete repaint.
        .repaint = m_backend->backend()->usesMobileGl() ? Region::infinite() : m_accumDamage[m_currentIndex],
    };
}

bool AnlandEglLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    Q_UNUSED(renderedDeviceRegion)
    Q_UNUSED(frame)
    if (m_bufCount == 0 || !m_backend->openglContext()) {
        return false;
    }
    if (m_backend->backend()->usesMobileGl()) {
        const bool swapped = eglSwapBuffers(m_backend->eglDisplayObject()->handle(), m_backend->eglDisplayObject()->defaultSurface());
        set_render_fence(m_display, -1);
        if (!swapped) {
            qCWarning(KWIN_ANLAND) << "MobileGL swap failed" << Qt::hex << eglGetError();
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
