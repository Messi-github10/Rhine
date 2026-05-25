#include "PlayerController.h"
#include "renderer.h"
#include "decoder.h"
#include "videodisplay.h"

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
    // 1. Shutdown the queue first — unblocks any thread stuck in push()/pop()
    if (m_frameQueue) m_frameQueue->shutdown();

    // 2. Signal decoder/renderer to stop (wakes paused renderer)
    if (m_decoder) m_decoder->stop();
    if (m_renderer) m_renderer->stop();

    // 3. Join threads — they'll exit promptly because:
    //    - Queue is shutdown → pop() returns nullopt, push() returns early
    //    - m_stop is set → top of loop exits
    //    - Renderer also woken from pause via m_pauseCV
    if (m_decodeThread.joinable()) m_decodeThread.join();
    if (m_renderThread.joinable()) m_renderThread.join();

    // 4. Destroy pipeline objects
    m_renderer.reset();
    m_decoder.reset();
    m_frameQueue.reset();
}

void PlayerController::startPipeline(const QString &filePath) {
    // Frame queue
    m_frameQueue = std::make_unique<BoundedQueue<AVFramePtr>>(30);

    // Decoder
    DecoderConfig decCfg;
    decCfg.filePath = filePath.toStdString();
    decCfg.frameQueue = m_frameQueue.get();

    m_decoder = std::make_unique<Decoder>(decCfg);
    if (!m_decoder->open()) {
        qCritical() << "[PlayerController] Failed to open:" << filePath;
        m_decoder.reset();
        m_frameQueue.reset();
        return;
    }

    // Renderer
    RendererConfig rendCfg;
    rendCfg.frameQueue = m_frameQueue.get();
    rendCfg.srcWidth = m_decoder->width();
    rendCfg.srcHeight = m_decoder->height();
    rendCfg.srcFormat = m_decoder->pixelFormat();
    rendCfg.timeBase = m_decoder->timeBase();
    rendCfg.frameRate = m_decoder->frameRate();

    m_renderer = std::make_unique<Renderer>(rendCfg);
    m_renderer->setDuration(m_decoder->duration());

    // Resize window to match video dimensions
    if (m_videoDisplay) {
        auto *window = m_videoDisplay->window();
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
    }

    // Start decode thread
    Decoder *decPtr = m_decoder.get();
    m_decodeThread = std::thread([decPtr]() {
        decPtr->run();
    });

    // Start render thread
    Renderer *rendPtr = m_renderer.get();
    VideoDisplay *vd = m_videoDisplay;
    PlayerController *ctrl = this;
    m_renderThread = std::thread([rendPtr, vd, ctrl]() {
        rendPtr->run(
            [vd](VideoFrame frame) {
                if (!vd) return;
                QImage img(frame.data.data(), frame.width, frame.height,
                          frame.stride, QImage::Format_RGB32);
                QMetaObject::invokeMethod(vd, [vd, img = img.copy()]() {
                    vd->setFrame(img);
                }, Qt::QueuedConnection);
            },
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