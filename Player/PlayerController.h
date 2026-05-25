#pragma once

#include <QObject>
#include <QTimer>

#include <memory>
#include <thread>

#include "framequeue.h"

class Renderer;
class Decoder;
class VideoDisplay;

class PlayerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(double currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileNameChanged)

public:
    explicit PlayerController(QObject *parent = nullptr);
    ~PlayerController() override;

    double currentTime() const { return m_currentTime; }
    double duration() const { return m_duration; }
    bool isPlaying() const { return m_isPlaying; }
    bool hasVideo() const { return m_hasVideo; }
    QString fileName() const { return m_fileName; }

    void setVideoDisplay(VideoDisplay *vd);
    void shutdown();

    Q_INVOKABLE void loadFile(const QString &filePath);
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void seek(double position);
    Q_INVOKABLE void skipForward();
    Q_INVOKABLE void skipBackward();

signals:
    void currentTimeChanged();
    void durationChanged();
    void isPlayingChanged();
    void hasVideoChanged();
    void fileNameChanged();

private:
    void poll();
    void stopPipeline();
    void startPipeline(const QString &filePath);
    void onPlaybackFinished();

    // Pipeline (heap, owned)
    std::unique_ptr<BoundedQueue<AVFramePtr>> m_frameQueue;
    std::unique_ptr<Decoder> m_decoder;
    std::unique_ptr<Renderer> m_renderer;
    std::thread m_decodeThread;
    std::thread m_renderThread;

    VideoDisplay *m_videoDisplay = nullptr;
    QTimer m_pollTimer;

    double m_currentTime = 0.0;
    double m_duration = 0.0;
    bool m_isPlaying = false;
    bool m_hasVideo = false;
    bool m_playbackFinished = false;
    bool m_seeking = false;
    QString m_fileName;
    QString m_currentFile;
};