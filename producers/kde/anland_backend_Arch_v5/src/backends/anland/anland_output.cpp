/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_output.h"
#include "anland_backend.h"
#include "anland_egl_backend.h"
#include "anland_logging.h"

#include "core/outputconfiguration.h"
#include "core/renderbackend.h" // OutputFrame
#include "core/renderloop.h"

#include <chrono>

namespace KWin
{

AnlandOutput::AnlandOutput(AnlandBackend *parent, const QString &name)
    : BackendOutput()
    , m_backend(parent)
    , m_renderLoop(std::make_unique<RenderLoop>(this))
{
    setInformation(Information{
        .name = name,
        .manufacturer = QStringLiteral("anland"),
        .model = QStringLiteral("anland"),
        .internal = true,
    });
}

AnlandOutput::~AnlandOutput()
{
}

RenderLoop *AnlandOutput::renderLoop() const
{
    return m_renderLoop.get();
}

bool AnlandOutput::testPresentation(const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(frame)
    return true;
}

bool AnlandOutput::present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(layersToUpdate)
    // The scene has already been rendered into the daemon's dmabuf by the layer
    // (AnlandEglLayer::doEndFrame). Hand it to the consumer now.
    m_frame = frame;

    const bool handedToConsumer = m_backend->notifyFramePresented();
    if (handedToConsumer) {
        // The consumer will present the buffer and then signal buffer-ready;
        // defer frame completion until then (see onConsumerReady()).
        m_awaitingPresent = true;
    } else {
        // Nothing was handed to the consumer this frame, so no buffer-ready will
        // arrive for it — complete it now so the RenderLoop never stalls.
        completeFrame();
    }
    return true;
}

void AnlandOutput::init(const QSize &pixelSize, int refresh, qreal scale)
{
    // refresh is in mHz, like RenderLoop/OutputMode expect.
    if (refresh <= 0) {
        refresh = 120000;
    }
    m_renderLoop->setRefreshRate(refresh);

    auto mode = std::make_shared<OutputMode>(OutputModeline(pixelSize, refresh, OutputModeline::Flag::Preferred));

    setState(State{
        .position = QPoint(0, 0),
        .scale = scale,
        .modes = {mode},
        .currentMode = mode,
    });
}

void AnlandOutput::updateEnabled(bool enabled)
{
    State next = m_state;
    next.enabled = enabled;
    setState(next);
}

void AnlandOutput::setRefreshRate(int refresh)
{
    if (refresh <= 0 || refresh == m_renderLoop->refreshRate()) {
        return;
    }
    m_renderLoop->setRefreshRate(refresh);

    auto mode = std::make_shared<OutputMode>(OutputModeline(modeSize(), refresh, OutputModeline::Flag::Preferred));
    State next = m_state;
    next.modes = {mode};
    next.currentMode = mode;
    setState(next);
}

void AnlandOutput::completeFrame()
{
    if (!m_frame) {
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    m_frame->presented(now, PresentationMode::VSync);
    m_frame.reset();
}

void AnlandOutput::onConsumerReady()
{
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        completeFrame();
    }
    m_renderLoop->scheduleRepaint();
}

void AnlandOutput::resize(const QSize &newSize)
{
    if (newSize == modeSize() || !newSize.isValid()) {
        return;
    }

    qCInfo(KWIN_ANLAND) << "resizing output to" << newSize;

    const int refresh = m_renderLoop->refreshRate();
    auto mode = std::make_shared<OutputMode>(OutputModeline(newSize, refresh, OutputModeline::Flag::Preferred));
    State next = m_state;
    next.modes = {mode};
    next.currentMode = mode;
    setState(next);

    m_backend->notifyOutputsChanged();

    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        m_frame.reset();
    }
}

void AnlandOutput::setEglLayer(std::unique_ptr<AnlandEglLayer> &&layer)
{
    m_layer = std::move(layer);
}

AnlandEglLayer *AnlandOutput::eglLayer() const
{
    return m_layer.get();
}

void AnlandOutput::failPendingFrame()
{
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        m_frame.reset();
    }
}

void AnlandOutput::stopRendering()
{
    failPendingFrame();

    if (!m_renderingInhibited) {
        m_renderLoop->inhibit();
        m_renderingInhibited = true;
    }
}

void AnlandOutput::resumeRendering()
{
    if (m_renderingInhibited) {
        m_renderLoop->uninhibit();
        m_renderingInhibited = false;
    }
}

void AnlandOutput::applyChanges(const OutputConfiguration &config)
{
    BackendOutput::applyChanges(config);
    if (const auto props = config.constChangeSet(this); props && props->dpmsMode) {
        const bool alreadyThere = m_state.dpmsMode == *props->dpmsMode;
        setPowerSaving(*props->dpmsMode != DpmsMode::On);
        // The workspace settles its own DPMS state on this signal; an output that was already
        // powered down (KWin started with nobody looking) must still give it.
        if (alreadyThere) {
            Q_EMIT dpmsModeChanged();
        }
    }
}

void AnlandOutput::setPowerSaving(bool saving)
{
    const DpmsMode mode = saving ? DpmsMode::Off : DpmsMode::On;
    if (m_state.dpmsMode != mode) {
        State next = m_state;
        next.dpmsMode = mode;
        setState(next);
        qCInfo(KWIN_ANLAND) << (saving ? "viewer gone: output powered down" : "viewer back: output powered up");
    }
    if (saving) {
        stopRendering();
    } else {
        resumeRendering();
        m_renderLoop->scheduleRepaint();
    }
}

} // namespace KWin

#include "moc_anland_output.cpp"
