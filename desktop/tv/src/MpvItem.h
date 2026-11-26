#pragma once

#include <QQuickFramebufferObject>
#include <QString>
#include <QTimer>

#include <mpv/client.h>

#include <memory>
#include <mutex>

namespace zethropol::tv {

struct MpvHandleHolder
{
    mpv_handle *mpv = nullptr;
    std::mutex mutex;

    ~MpvHandleHolder();
};

class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT

    Q_PROPERTY(QString currentUrl READ currentUrl NOTIFY currentUrlChanged)
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(QString aspectRatio READ aspectRatio WRITE setAspectRatio NOTIFY aspectRatioChanged)
    Q_PROPERTY(bool retrying READ retrying NOTIFY retryingChanged)
    Q_PROPERTY(int retryAttempt READ retryAttempt NOTIFY retryAttemptChanged)
    Q_PROPERTY(bool backendAvailable READ backendAvailable CONSTANT)
    Q_PROPERTY(bool hasAudioTrack READ hasAudioTrack NOTIFY audioStatusChanged)
    Q_PROPERTY(bool audioOutputError READ audioOutputError NOTIFY audioStatusChanged)
    Q_PROPERTY(QString currentAudioOutput READ currentAudioOutput NOTIFY audioStatusChanged)
    Q_PROPERTY(QString audioStatus READ audioStatus NOTIFY audioStatusChanged)

public:
    static constexpr const char *kDefaultAudioOutput = "pipewire,pulse,alsa";
    static constexpr const char *kAudioClientName = "Zethropol TV";
    static constexpr int kDefaultVolume = 80;

    // Canonical upright OpenGL FBO orientation for Qt Quick + libmpv:
    // libmpv with MPV_RENDER_PARAM_FLIP_Y = 0 already renders top-to-bottom for Qt Quick's
    // texture coordinate space; QQuickFramebufferObject::mirrorVertically must stay false
    // so the frame is not inverted a second time (net flip parity = 0).
    static constexpr bool kMirrorVertically = false;
    static constexpr int kRenderFlipY = 0;
    static constexpr bool isOrientationUpright() noexcept
    {
        return kMirrorVertically == (kRenderFlipY != 0);
    }
    static_assert(kMirrorVertically == (kRenderFlipY != 0),
                  "MpvItem FBO mirrorVertically and MPV_RENDER_PARAM_FLIP_Y must yield upright video");

    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;

    Q_INVOKABLE bool playUrl(const QString &url);
    Q_INVOKABLE bool playUrlWithHeaders(const QString &url,
                                        const QString &referer = QString(),
                                        const QString &userAgent = QString());
    Q_INVOKABLE void stop();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void cycleAspectRatio();
    Q_INVOKABLE void setResolvingState(const QString &statusText = QString());
    Q_INVOKABLE void setExternalError(const QString &message);

    QString currentUrl() const { return m_currentUrl; }
    QString playbackState() const { return m_playbackState; }
    QString errorMessage() const { return m_errorMessage; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    bool paused() const { return m_paused; }
    QString aspectRatio() const { return m_aspectRatio; }
    bool retrying() const { return m_retrying; }
    int retryAttempt() const { return m_retryAttempt; }
    bool backendAvailable() const { return m_handle && m_handle->mpv != nullptr; }
    bool hasAudioTrack() const { return m_hasAudioTrack; }
    bool audioOutputError() const { return m_audioOutputError; }
    QString currentAudioOutput() const { return m_currentAudioOutput; }
    QString audioStatus() const { return m_audioStatus; }

    // Live libmpv property inspection helpers (used by diagnostics & unit tests)
    QString mpvPropertyString(const QString &name) const;
    QString configuredAudioOutput() const;
    QString configuredAudioClientName() const;
    QString configuredAudioTrackSelection() const;
    double mpvVolume() const;
    bool isMpvMuted() const;

    void setVolume(int vol);
    void setMuted(bool muted);
    void setAspectRatio(const QString &ratio);

    std::shared_ptr<MpvHandleHolder> handleHolder() const { return m_handle; }

signals:
    void currentUrlChanged();
    void playbackStateChanged();
    void errorMessageChanged();
    void volumeChanged();
    void mutedChanged();
    void pausedChanged();
    void aspectRatioChanged();
    void retryingChanged();
    void retryAttemptChanged();
    void audioStatusChanged();

public slots:
    void onMpvRenderUpdate();

private slots:
    void onMpvEvents();
    void performScheduledRetry();

private:
    static void mpvWakeupCallback(void *ctx);

    void initializeMpv();
    void setPlaybackState(const QString &state);
    void setErrorMessage(const QString &msg);
    void setRetrying(bool retrying, int attempt);
    void scheduleRetry(const QString &reason);
    void cancelRetry();
    void applyAspectRatioToMpv(const QString &ratio);
    void applyAudioSettingsToMpv(bool synchronous, bool includeAid);
    void applyHttpHeadersToMpv(const QString &referer, const QString &userAgent);
    void refreshAudioDiagnostics();
    void updateAudioDiagnostics(bool hasTrack,
                                const QString &currentAo,
                                bool aoError,
                                const QString &statusText);
    void sendLoadFile(const QString &url);

    std::shared_ptr<MpvHandleHolder> m_handle;
    QString m_currentUrl;
    QString m_currentReferer;
    QString m_currentUserAgent;
    QString m_lastNetworkError;
    QString m_playbackState = QStringLiteral("idle");
    QString m_errorMessage;
    int m_volume = kDefaultVolume;
    bool m_muted = false;
    bool m_paused = false;
    QString m_aspectRatio = QStringLiteral("auto");
    bool m_retrying = false;
    int m_retryAttempt = 0;
    bool m_stoppingIntentionally = false;
    bool m_hasAudioTrack = false;
    bool m_audioOutputError = false;
    QString m_currentAudioOutput;
    QString m_audioStatus = QStringLiteral("Ses hazır (PipeWire/PulseAudio/ALSA)");
    QString m_observedCurrentAo;
    QString m_observedAudioDevice;
    QString m_observedAid = QStringLiteral("auto");
    qint64 m_observedAudioTrackId = 0;
    qint64 m_observedSampleRate = 0;
    QString m_lastAudioError;
    QTimer m_retryTimer;
};

} // namespace zethropol::tv
