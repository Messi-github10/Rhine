#include "PlayerController.hpp"
#include "renderer.hpp"
#include "VulkanRenderer.hpp"
#include "VulkanContext.hpp"
#include "IDecoder.hpp"
#include "CpuDecoder.hpp"
#include "VulkanDecoder.hpp"
#include "videodisplay.hpp"

#include <QFileInfo>
#include <QUrl>
#include <QQuickWindow>
#include <QScreen>
#include <QDebug>
#include <algorithm>
#include <cmath>

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
{
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &PlayerController::poll);
    m_pollTimer.start();
}

PlayerController::~PlayerController() {
    shutdown();
}

void PlayerController::setVideoDisplay(VideoDisplay *vd) {
    m_videoDisplay = vd;
}

void PlayerController::shutdown() {
    stopPipeline();
}

void PlayerController::stopPipeline() {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    // Check if threads are actually running
    bool decRunning = m_decodeThread.joinable();
    bool rendRunning = m_renderThread.joinable();
    qDebug() << "[stopPipeline] decodeRunning=" << decRunning
             << "renderRunning=" << rendRunning;

    // 1. Shutdown the queue first — unblocks any thread stuck in push()/pop()
    if (m_frameQueue) m_frameQueue->shutdown();
    auto t1 = clock::now();

    // 2. Signal decoder/renderer to stop (wakes paused renderer)
    if (m_decoder) m_decoder->stop();
    if (m_renderer) m_renderer->stop();
    auto t2 = clock::now();

    // 3. Join threads
    if (m_decodeThread.joinable()) m_decodeThread.join();
    auto t3 = clock::now();

    if (m_renderThread.joinable()) m_renderThread.join();
    auto t4 = clock::now();

    // 4. Destroy pipeline objects
    m_renderer.reset();
    m_decoder.reset();
    m_frameQueue.reset();
    auto t5 = clock::now();

    // 5. Destroy VkSurfaceKHR (after render thread has joined)
    if (m_vulkanSurface != VK_NULL_HANDLE) {
        auto *window = m_videoDisplay ? m_videoDisplay->window() : nullptr;
        if (window) {
            // Disconnect resize signals
            disconnect(m_resizeWConn);
            disconnect(m_resizeHConn);
        }
        vkDestroySurfaceKHR(VulkanContext::instance().vkInstance(),
                            m_vulkanSurface, nullptr);
        m_vulkanSurface = VK_NULL_HANDLE;
    }
    auto t6 = clock::now();

    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    qDebug() << "[stopPipeline] shutdown:" << ms(t1-t0) << "ms"
             << "| stop:" << ms(t2-t1) << "ms"
             << "| joinDec:" << ms(t3-t2) << "ms"
             << "| joinRend:" << ms(t4-t3) << "ms"
             << "| destroy:" << ms(t5-t4) << "ms"
             << "| surface:" << ms(t6-t5) << "ms"
             << "| total:" << ms(t6-t0) << "ms";
}

void PlayerController::startPipeline(const QString &filePath) {
    // Frame queue
    m_frameQueue = std::make_unique<BoundedQueue<AVFramePtr>>(30);

    // Decoder — prefer Vulkan hardware decode, fallback to CPU software
    // Note: Vulkan decode produces GPU frames; CPU renderer cannot consume them.
    // On Wayland (no swapchain), Vulkan decode is only used if VulkanRenderer can
    // also be used. On XCB, full GPU pipeline (decode + render).
    bool useVulkan = VulkanContext::instance().isValid();
    bool isXcb = (QGuiApplication::platformName() == "xcb");

    DecoderConfig decCfg;
    decCfg.filePath = filePath.toStdString();
    decCfg.frameQueue = m_frameQueue.get();

    bool decoderOpened = false;

    if (useVulkan && isXcb) {
        auto vulkanDec = std::make_unique<VulkanDecoder>(decCfg);
        if (vulkanDec->open()) {
            m_decoder = std::move(vulkanDec);
            decoderOpened = true;
        } else {
            qInfo() << "[PlayerController] Vulkan hardware decode unavailable,"
                    << "falling back to CPU software decode";
            useVulkan = false;
        }
    }

    if (!decoderOpened) {
        m_decoder = std::make_unique<CpuDecoder>(decCfg);
        if (!m_decoder->open()) {
            qCritical() << "[PlayerController] Failed to open:" << filePath;
            m_decoder.reset();
            m_frameQueue.reset();
            return;
        }
    }

    // Renderer (Vulkan swapchain on X11, CPU on Wayland/fallback)
    // Wayland: Qt OpenGL and Vulkan swapchain cannot share the same wl_surface.
    // Use QT_QPA_PLATFORM=xcb for full GPU pipeline.
    bool isXcb = (QGuiApplication::platformName() == "xcb");
    QQuickWindow *window = m_videoDisplay ? m_videoDisplay->window() : nullptr;

    if (useVulkan && window && isXcb) {
        // Create VkSurfaceKHR from Qt window
        m_vulkanSurface = VulkanContext::instance().createSurface(window);
        if (!m_vulkanSurface) {
            qWarning() << "[PlayerController] Failed to create VkSurfaceKHR, falling back to CPU";
            useVulkan = false;
        } else {
            // Connect resize signals
            m_resizeWConn = connect(window, &QQuickWindow::widthChanged,
                                    this, [this](int w) {
                if (m_renderer) m_renderer->resize(w, 0);
            });
            m_resizeHConn = connect(window, &QQuickWindow::heightChanged,
                                    this, [this](int h) {
                if (m_renderer) m_renderer->resize(0, h);
            });
        }
    } else if (useVulkan && !isXcb) {
        qInfo() << "[PlayerController] Wayland detected — Vulkan swapchain disabled.";
        qInfo() << "  Run with QT_QPA_PLATFORM=xcb for full GPU pipeline.";
        useVulkan = false;
    }

    if (useVulkan) {
        VulkanRenderer::Config rendCfg;
        rendCfg.frameQueue = m_frameQueue.get();
        rendCfg.srcWidth = m_decoder->width();
        rendCfg.srcHeight = m_decoder->height();
        rendCfg.timeBase = m_decoder->timeBase();
        rendCfg.surface = m_vulkanSurface;
        rendCfg.windowWidth = window->width();
        rendCfg.windowHeight = window->height();

        m_renderer = std::make_unique<VulkanRenderer>(rendCfg);
        qDebug() << "[PlayerController] Using VulkanRenderer (swapchain)";
    } else {
        RendererConfig rendCfg;
        rendCfg.frameQueue = m_frameQueue.get();
        rendCfg.srcWidth = m_decoder->width();
        rendCfg.srcHeight = m_decoder->height();
        rendCfg.srcFormat = m_decoder->pixelFormat();
        rendCfg.timeBase = m_decoder->timeBase();
        rendCfg.frameRate = m_decoder->frameRate();

        m_renderer = std::make_unique<Renderer>(rendCfg);
        qDebug() << "[PlayerController] Using CPU Renderer";
    }
    m_renderer->setDuration(m_decoder->duration());

    // Resize window to match video dimensions
    if (window) {
        // Scale to fit 80% of screen
        int sw = window->screen()->availableSize().width();
        int sh = window->screen()->availableSize().height();
        int vw = m_decoder->width();
        int vh = m_decoder->height();

        double scaleW = sw * 0.8 / vw;
        double scaleH = sh * 0.8 / vh;
        double scale = std::min(scaleW, scaleH);

        window->resize(int(vw * scale), int(vh * scale));
    }

    // Start decode thread
    IDecoder *decPtr = m_decoder.get();
    m_decodeThread = std::thread([decPtr]() {
        decPtr->run();
    });

    // Start render thread
    IRenderer *rendPtr = m_renderer.get();
    VideoDisplay *vd = m_videoDisplay;
    PlayerController *ctrl = this;

    m_renderThread = std::thread([rendPtr, vd, ctrl, useVulkan]() {
        rendPtr->run(
            // onFrame: CPU path sends QImage to VideoDisplay, Vulkan is no-op
            [vd, useVulkan](VideoFrame frame) {
                if (useVulkan || !vd) return;
                QImage img(frame.data.data(), frame.width, frame.height,
                          frame.stride, QImage::Format_RGB32);
                QMetaObject::invokeMethod(vd, [vd, img = img.copy()]() {
                    vd->setFrame(img);
                }, Qt::QueuedConnection);
            },
            // onDone: notify controller when playback ends
            [ctrl]() {
                QMetaObject::invokeMethod(ctrl, [ctrl]() {
                    ctrl->onPlaybackFinished();
                }, Qt::QueuedConnection);
            }
        );
    });
}

void PlayerController::loadFile(const QString &filePath) {
    // Resolve local path (QML passes QUrl as string)
    QUrl url(filePath);
    QString localPath = url.isLocalFile() ? url.toLocalFile() : filePath;

    // 1. Stop current pipeline
    stopPipeline();

    // 2. Start new pipeline with resolved path
    startPipeline(localPath);

    // 3. Update QML properties
    if (m_decoder) {
        m_hasVideo = true;
        m_duration = m_decoder->duration();
        m_fileName = QFileInfo(localPath).fileName();
        m_currentTime = 0.0;
        m_isPlaying = false;
        m_playbackFinished = false;
        m_currentFile = localPath;
    } else {
        m_hasVideo = false;
        m_duration = 0.0;
        m_fileName.clear();
        m_currentFile.clear();
    }

    emit hasVideoChanged();
    emit durationChanged();
    emit fileNameChanged();
    emit currentTimeChanged();
    emit isPlayingChanged();
}

void PlayerController::onPlaybackFinished() {
    m_playbackFinished = true;
    m_currentTime = m_duration;
    m_isPlaying = false;
    emit currentTimeChanged();
    emit isPlayingChanged();
}

void PlayerController::poll() {
    if (!m_renderer) {
        if (m_isPlaying) {
            m_isPlaying = false;
            emit isPlayingChanged();
        }
        return;
    }

    // Don't overwrite finished state set by onPlaybackFinished()
    if (m_playbackFinished) return;

    double t = m_renderer->currentTime();
    bool playing = m_renderer->isPlaying();

    // During seek, protect the target time from being overwritten by stale
    // renderer values. Release once the renderer catches up.
    if (m_seeking) {
        if (std::abs(t - m_currentTime) < 0.5) {
            m_seeking = false;
        } else {
            // Still waiting for renderer to catch up — only track isPlaying
            if (playing != m_isPlaying) {
                m_isPlaying = playing;
                emit isPlayingChanged();
            }
            return;
        }
    }

    if (t != m_currentTime) {
        m_currentTime = t;
        emit currentTimeChanged();
    }
    if (playing != m_isPlaying) {
        m_isPlaying = playing;
        emit isPlayingChanged();
    }
}

void PlayerController::togglePlayPause() {
    if (!m_renderer) return;
    if (m_renderer->isPlaying()) {
        m_renderer->pause();
    } else if (m_playbackFinished) {
        // Replay: reload the same file from beginning
        if (!m_currentFile.isEmpty()) {
            loadFile(m_currentFile);
            if (m_renderer) m_renderer->play();
        }
    } else {
        m_renderer->play();
    }
}

void PlayerController::seek(double position) {
    if (!m_decoder || !m_renderer) return;
    position = std::clamp(position, 0.0, m_duration);

    // Update progress bar immediately and block poll() from overwriting it
    m_seeking = true;
    m_currentTime = position;
    emit currentTimeChanged();

    m_decoder->seek(position, nullptr);
}

void PlayerController::skipForward() {
    if (!m_renderer) return;
    double pos = m_renderer->currentTime() + 5.0;
    if (pos > m_duration) pos = m_duration;
    seek(pos);
}

void PlayerController::skipBackward() {
    if (!m_renderer) return;
    double pos = m_renderer->currentTime() - 5.0;
    if (pos < 0.0) pos = 0.0;
    seek(pos);
}