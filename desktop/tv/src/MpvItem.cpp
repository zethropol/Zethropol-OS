#include "MpvItem.h"
#include "StreamSecurity.h"

#include <QByteArray>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <mpv/render_gl.h>

#include <algorithm>
#include <array>
#include <clocale>

namespace zethropol::tv {

namespace {

constexpr int kMaxAutoRetries = 3;
constexpr std::array<int, kMaxAutoRetries> kRetryDelaysMs = {3000, 6000, 10000};
constexpr const char *kDefaultStreamUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

QString sanitizeUserAgent(const QString &raw)
{
    QString out;
    out.reserve(qMin(raw.size(), 240));
    for (const QChar ch : raw) {
        const ushort code = ch.unicode();
        if (code < 0x20 || code == 0x7F) {
            continue;
        }
        out.append(ch);
        if (out.size() >= 240) {
            break;
        }
    }
    out = out.trimmed();
    return out.isEmpty() ? QString::fromLatin1(kDefaultStreamUserAgent) : out;
}

QString formatAudioDriverLabel(const QString &ao)
{
    const QString lower = ao.trimmed().toLower();
    if (lower == QLatin1String("pipewire")) {
        return QStringLiteral("PipeWire");
    }
    if (lower == QLatin1String("pulse")) {
        return QStringLiteral("PulseAudio");
    }
    if (lower == QLatin1String("alsa")) {
        return QStringLiteral("ALSA");
    }
    if (lower == QLatin1String("null")) {
        return QStringLiteral("Sessiz Çıkış");
    }
    return ao.trimmed();
}

void *getOpenGlProcAddress(void * /*ctx*/, const char *name)
{
    QOpenGLContext *glCtx = QOpenGLContext::currentContext();
    if (!glCtx || !name) {
        return nullptr;
    }
    return reinterpret_cast<void *>(glCtx->getProcAddress(QByteArray(name)));
}

void onMpvRenderUpdateCallback(void *ctx)
{
    auto *item = static_cast<MpvItem *>(ctx);
    if (!item) {
        return;
    }
    QMetaObject::invokeMethod(item, &MpvItem::onMpvRenderUpdate, Qt::QueuedConnection);
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(MpvItem *item)
        : m_item(item)
        , m_handle(item ? item->handleHolder() : nullptr)
    {}

    ~MpvRenderer() override
    {
        if (m_renderCtx) {
            mpv_render_context_set_update_callback(m_renderCtx, nullptr, nullptr);
            mpv_render_context_free(m_renderCtx);
            m_renderCtx = nullptr;
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (!m_renderCtx && m_handle && m_handle->mpv && QOpenGLContext::currentContext()) {
            mpv_opengl_init_params glInitParams{
                getOpenGlProcAddress,
                nullptr,
            };
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInitParams},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            if (mpv_render_context_create(&m_renderCtx, m_handle->mpv, params) >= 0) {
                mpv_render_context_set_update_callback(m_renderCtx, onMpvRenderUpdateCallback, m_item);
            } else {
                m_renderCtx = nullptr;
            }
        }
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override
    {
        QQuickOpenGLUtils::resetOpenGLState();
        QOpenGLFramebufferObject *fbo = framebufferObject();
        if (!m_renderCtx || !fbo) {
            return;
        }

        const QSize sz = fbo->size();
        mpv_opengl_fbo mpvFbo{
            static_cast<int>(fbo->handle()),
            sz.width(),
            sz.height(),
            0,
        };
        int flipY = MpvItem::kRenderFlipY;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_renderCtx, params);
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    MpvItem *m_item = nullptr;
    std::shared_ptr<MpvHandleHolder> m_handle;
    mpv_render_context *m_renderCtx = nullptr;
};

} // namespace

MpvHandleHolder::~MpvHandleHolder()
{
    if (mpv) {
        mpv_set_wakeup_callback(mpv, nullptr, nullptr);
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
    }
}

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_handle(std::make_shared<MpvHandleHolder>())
{
    setMirrorVertically(kMirrorVertically);
    setTextureFollowsItemSize(true);

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &MpvItem::performScheduledRetry);

    initializeMpv();
}

MpvItem::~MpvItem()
{
    cancelRetry();
    if (m_handle && m_handle->mpv) {
        mpv_set_wakeup_callback(m_handle->mpv, nullptr, nullptr);
        const char *stopCmd[] = {"stop", nullptr};
        mpv_command_async(m_handle->mpv, 0, stopCmd);
    }
}

void MpvItem::initializeMpv()
{
    // libmpv strictly requires C numeric locale before creation/init.
    std::setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        setErrorMessage(QStringLiteral("libmpv oynatıcı örneği başlatılamadı."));
        setPlaybackState(QStringLiteral("error"));
        return;
    }

    // Mandatory security hardening: block all scripts, hooks, external resolvers, and unsafe playlists.
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto-safe");
    mpv_set_option_string(mpv, "load-scripts", "no");
    mpv_set_option_string(mpv, "load-unsafe-playlists", "no");
    mpv_set_option_string(mpv, "ytdl", "no");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "input-default-bindings", "no");
    mpv_set_option_string(mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "tls-verify", "yes");
    mpv_set_option_string(mpv, "network-timeout", "15");
    mpv_set_option_string(mpv, "user-agent", kDefaultStreamUserAgent);
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "cache-secs", "10");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "16MiB");
    mpv_set_option_string(mpv, "idle", "yes");
    mpv_set_option_string(mpv, "keep-open", "no");

    // Explicit Linux desktop audio output chain (PipeWire -> PulseAudio -> ALSA)
    // and KDE Plasma / PipeWire mixer stream identification.
    mpv_set_option_string(mpv, "ao", kDefaultAudioOutput);
    mpv_set_option_string(mpv, "audio-client-name", kAudioClientName);
    mpv_set_option_string(mpv, "audio-device", "auto");
    mpv_set_option_string(mpv, "aid", "auto");
    mpv_set_option_string(mpv, "alang", "tr,tur,en");
    mpv_set_option_string(mpv, "mute", "no");
    mpv_set_option_string(mpv, "audio-channels", "auto-safe");
    mpv_set_option_string(mpv, "audio-fallback-to-null", "no");

    const double vol = static_cast<double>(m_volume);
    mpv_set_option(mpv, "volume", MPV_FORMAT_DOUBLE, const_cast<double *>(&vol));

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        setErrorMessage(QStringLiteral("libmpv başlatma başarısız oldu."));
        setPlaybackState(QStringLiteral("error"));
        return;
    }

    m_handle->mpv = mpv;
    mpv_request_log_messages(mpv, "warn");
    applyAudioSettingsToMpv(true, true);

    mpv_observe_property(mpv, 1, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 2, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 3, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 5, "current-ao", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 6, "audio-device", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 7, "aid", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 8, "current-tracks/audio/id", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 9, "audio-params/samplerate", MPV_FORMAT_INT64);

    mpv_set_wakeup_callback(mpv, &MpvItem::mpvWakeupCallback, this);
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    return new MpvRenderer(const_cast<MpvItem *>(this));
}

bool MpvItem::playUrl(const QString &url)
{
    return playUrlWithHeaders(url, QString(), QString());
}

bool MpvItem::playUrlWithHeaders(const QString &url,
                                 const QString &referer,
                                 const QString &userAgent)
{
    cancelRetry();

    QString rejectReason;
    if (!StreamSecurity::isAllowedMediaUrl(url, &rejectReason)) {
        m_stoppingIntentionally = true;
        if (m_handle && m_handle->mpv) {
            const char *stopCmd[] = {"stop", nullptr};
            mpv_command_async(m_handle->mpv, 0, stopCmd);
        }
        setErrorMessage(QStringLiteral("Güvenlik engeli: %1").arg(rejectReason));
        setPlaybackState(QStringLiteral("error"));
        return false;
    }

    const QString cleaned = url.trimmed();
    m_currentReferer = StreamSecurity::isAllowedMediaUrl(referer) ? referer.trimmed() : QString();
    m_currentUserAgent = sanitizeUserAgent(userAgent);
    m_lastNetworkError.clear();

    if (m_currentUrl != cleaned) {
        m_currentUrl = cleaned;
        emit currentUrlChanged();
    }

    setErrorMessage(QString());
    setPlaybackState(QStringLiteral("loading"));

    if (!m_handle || !m_handle->mpv) {
        setErrorMessage(QStringLiteral("Video motoru hazır değil."));
        setPlaybackState(QStringLiteral("error"));
        return false;
    }

    applyHttpHeadersToMpv(m_currentReferer, m_currentUserAgent);
    sendLoadFile(cleaned);
    return true;
}

void MpvItem::applyHttpHeadersToMpv(const QString &referer, const QString &userAgent)
{
    if (!m_handle || !m_handle->mpv) {
        return;
    }
    const QByteArray refUtf8 = (StreamSecurity::isAllowedMediaUrl(referer) ? referer.trimmed() : QString()).toUtf8();
    const char *refRaw = refUtf8.constData();
    mpv_set_property_async(m_handle->mpv, 0, "referrer", MPV_FORMAT_STRING, &refRaw);

    const QByteArray uaUtf8 = sanitizeUserAgent(userAgent).toUtf8();
    const char *uaRaw = uaUtf8.constData();
    mpv_set_property_async(m_handle->mpv, 0, "user-agent", MPV_FORMAT_STRING, &uaRaw);
}

void MpvItem::sendLoadFile(const QString &url)
{
    if (!m_handle || !m_handle->mpv) {
        return;
    }
    m_stoppingIntentionally = false;
    m_lastAudioError.clear();
    m_lastNetworkError.clear();
    m_observedCurrentAo.clear();
    m_observedAudioTrackId = 0;
    m_observedSampleRate = 0;
    updateAudioDiagnostics(false, QString(), false, QStringLiteral("Ses akışı bekleniyor..."));

    int unpause = 0;
    mpv_set_property_async(m_handle->mpv, 0, "pause", MPV_FORMAT_FLAG, &unpause);
    // Guarantee active unmuted audio level and automatic audio track selection on every channel load.
    applyAudioSettingsToMpv(false, true);

    const QByteArray utf8Url = url.toUtf8();
    const char *cmd[] = {"loadfile", utf8Url.constData(), "replace", nullptr};
    const int rc = mpv_command_async(m_handle->mpv, 0, cmd);
    if (rc < 0) {
        scheduleRetry(QStringLiteral("Yayın şu anda alınamıyor (mpv kodu: %1).").arg(rc));
    }
}

void MpvItem::stop()
{
    cancelRetry();
    m_stoppingIntentionally = true;
    m_lastAudioError.clear();
    m_observedCurrentAo.clear();
    m_observedAudioTrackId = 0;
    m_observedSampleRate = 0;
    if (m_handle && m_handle->mpv) {
        const char *cmd[] = {"stop", nullptr};
        mpv_command_async(m_handle->mpv, 0, cmd);
    }
    if (m_paused) {
        m_paused = false;
        emit pausedChanged();
    }
    setErrorMessage(QString());
    setPlaybackState(QStringLiteral("idle"));
    updateAudioDiagnostics(false, QString(), false, QStringLiteral("Ses hazır (PipeWire/PulseAudio/ALSA)"));
}

void MpvItem::setResolvingState(const QString &statusText)
{
    cancelRetry();
    m_stoppingIntentionally = true;
    m_lastAudioError.clear();
    m_observedCurrentAo.clear();
    m_observedAudioTrackId = 0;
    m_observedSampleRate = 0;
    if (m_handle && m_handle->mpv) {
        const char *cmd[] = {"stop", nullptr};
        mpv_command_async(m_handle->mpv, 0, cmd);
    }
    if (m_paused) {
        m_paused = false;
        emit pausedChanged();
    }
    setErrorMessage(statusText);
    setPlaybackState(QStringLiteral("resolving"));
    updateAudioDiagnostics(false, QString(), false, QStringLiteral("Ses akışı bekleniyor..."));
}

void MpvItem::setExternalError(const QString &message)
{
    cancelRetry();
    m_stoppingIntentionally = true;
    m_lastAudioError.clear();
    m_observedCurrentAo.clear();
    m_observedAudioTrackId = 0;
    m_observedSampleRate = 0;
    if (m_handle && m_handle->mpv) {
        const char *cmd[] = {"stop", nullptr};
        mpv_command_async(m_handle->mpv, 0, cmd);
    }
    if (m_paused) {
        m_paused = false;
        emit pausedChanged();
    }
    setErrorMessage(message.trimmed().isEmpty()
                        ? QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı")
                        : message.trimmed());
    setPlaybackState(QStringLiteral("error"));
    updateAudioDiagnostics(false, QString(), false, QStringLiteral("Ses hazır (PipeWire/PulseAudio/ALSA)"));
}

void MpvItem::togglePause()
{
    if (!m_handle || !m_handle->mpv || m_currentUrl.isEmpty()) {
        return;
    }
    if (m_playbackState == QStringLiteral("error") || m_playbackState == QStringLiteral("idle")) {
        retry();
        return;
    }
    int nextPause = m_paused ? 0 : 1;
    mpv_set_property_async(m_handle->mpv, 0, "pause", MPV_FORMAT_FLAG, &nextPause);
}

void MpvItem::retry()
{
    if (m_currentUrl.isEmpty()) {
        return;
    }
    cancelRetry();
    if (!StreamSecurity::isAllowedMediaUrl(m_currentUrl)) {
        return;
    }
    setErrorMessage(QString());
    setPlaybackState(QStringLiteral("loading"));
    applyHttpHeadersToMpv(m_currentReferer, m_currentUserAgent);
    sendLoadFile(m_currentUrl);
}

QString MpvItem::mpvPropertyString(const QString &name) const
{
    if (!m_handle || !m_handle->mpv || name.isEmpty()) {
        return QString();
    }
    const QByteArray key = name.toUtf8();
    char *raw = mpv_get_property_string(m_handle->mpv, key.constData());
    if (!raw) {
        return QString();
    }
    const QString result = QString::fromUtf8(raw);
    mpv_free(raw);
    return result;
}

QString MpvItem::configuredAudioOutput() const
{
    return mpvPropertyString(QStringLiteral("ao"));
}

QString MpvItem::configuredAudioClientName() const
{
    return mpvPropertyString(QStringLiteral("audio-client-name"));
}

QString MpvItem::configuredAudioTrackSelection() const
{
    return mpvPropertyString(QStringLiteral("aid"));
}

double MpvItem::mpvVolume() const
{
    if (!m_handle || !m_handle->mpv) {
        return 0.0;
    }
    double val = 0.0;
    if (mpv_get_property(m_handle->mpv, "volume", MPV_FORMAT_DOUBLE, &val) < 0) {
        return 0.0;
    }
    return val;
}

bool MpvItem::isMpvMuted() const
{
    if (!m_handle || !m_handle->mpv) {
        return false;
    }
    int flag = 0;
    if (mpv_get_property(m_handle->mpv, "mute", MPV_FORMAT_FLAG, &flag) < 0) {
        return false;
    }
    return flag != 0;
}

void MpvItem::applyAudioSettingsToMpv(bool synchronous, bool includeAid)
{
    if (!m_handle || !m_handle->mpv) {
        return;
    }
    const char *aidAuto = "auto";
    int muteFlag = m_muted ? 1 : 0;
    double vol = static_cast<double>(m_volume);

    if (synchronous) {
        if (includeAid) {
            mpv_set_property_string(m_handle->mpv, "aid", aidAuto);
        }
        mpv_set_property(m_handle->mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
        mpv_set_property(m_handle->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    } else {
        if (includeAid) {
            mpv_set_property_async(m_handle->mpv, 0, "aid", MPV_FORMAT_STRING, &aidAuto);
        }
        mpv_set_property_async(m_handle->mpv, 0, "mute", MPV_FORMAT_FLAG, &muteFlag);
        mpv_set_property_async(m_handle->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &vol);
    }
}

void MpvItem::setVolume(int vol)
{
    const int clamped = std::clamp(vol, 0, 100);
    const bool changed = (m_volume != clamped);
    m_volume = clamped;
    if (m_handle && m_handle->mpv) {
        double v = static_cast<double>(m_volume);
        mpv_set_property_async(m_handle->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &v);
    }
    if (changed) {
        emit volumeChanged();
    }
    refreshAudioDiagnostics();
}

void MpvItem::setMuted(bool muted)
{
    const bool changed = (m_muted != muted);
    m_muted = muted;
    if (!m_muted && m_volume == 0) {
        m_volume = kDefaultVolume;
        emit volumeChanged();
    }
    applyAudioSettingsToMpv(false, false);
    if (changed) {
        emit mutedChanged();
    }
    refreshAudioDiagnostics();
}

void MpvItem::setAspectRatio(const QString &ratio)
{
    const QString normalized = ratio.trimmed().toLower();
    if (normalized != QStringLiteral("auto") && normalized != QStringLiteral("16:9")
        && normalized != QStringLiteral("4:3") && normalized != QStringLiteral("21:9")
        && normalized != QStringLiteral("fill")) {
        return;
    }
    if (m_aspectRatio == normalized) {
        return;
    }
    m_aspectRatio = normalized;
    applyAspectRatioToMpv(m_aspectRatio);
    emit aspectRatioChanged();
}

void MpvItem::cycleAspectRatio()
{
    static const QStringList kOrder = {
        QStringLiteral("auto"),
        QStringLiteral("16:9"),
        QStringLiteral("4:3"),
        QStringLiteral("21:9"),
        QStringLiteral("fill"),
    };
    const int idx = kOrder.indexOf(m_aspectRatio);
    setAspectRatio(kOrder[(idx + 1) % kOrder.size()]);
}

void MpvItem::applyAspectRatioToMpv(const QString &ratio)
{
    if (!m_handle || !m_handle->mpv) {
        return;
    }
    double panscan = 0.0;
    QByteArray overrideVal = "-1";
    if (ratio == QStringLiteral("fill")) {
        panscan = 1.0;
    } else if (ratio != QStringLiteral("auto")) {
        overrideVal = ratio.toUtf8();
    }
    mpv_set_property_async(m_handle->mpv, 0, "panscan", MPV_FORMAT_DOUBLE, &panscan);
    const char *rawStr = overrideVal.constData();
    mpv_set_property_async(m_handle->mpv, 0, "video-aspect-override", MPV_FORMAT_STRING, &rawStr);
}

void MpvItem::mpvWakeupCallback(void *ctx)
{
    auto *self = static_cast<MpvItem *>(ctx);
    if (!self) {
        return;
    }
    QMetaObject::invokeMethod(self, &MpvItem::onMpvEvents, Qt::QueuedConnection);
}

void MpvItem::onMpvRenderUpdate()
{
    update();
}

void MpvItem::onMpvEvents()
{
    if (!m_handle || !m_handle->mpv) {
        return;
    }

    while (true) {
        mpv_event *event = mpv_wait_event(m_handle->mpv, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }

        switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            setErrorMessage(QString());
            setPlaybackState(QStringLiteral("loading"));
            refreshAudioDiagnostics();
            break;

        case MPV_EVENT_FILE_LOADED:
        case MPV_EVENT_AUDIO_RECONFIG:
            refreshAudioDiagnostics();
            break;

        case MPV_EVENT_PLAYBACK_RESTART:
            cancelRetry();
            setErrorMessage(QString());
            setPlaybackState(m_paused ? QStringLiteral("paused") : QStringLiteral("playing"));
            if (!m_stoppingIntentionally) {
                // Re-apply active unmuted volume once the PipeWire/PulseAudio stream opens.
                applyAudioSettingsToMpv(false, false);
            }
            refreshAudioDiagnostics();
            break;

        case MPV_EVENT_LOG_MESSAGE: {
            const auto *logMsg = static_cast<const mpv_event_log_message *>(event->data);
            if (!logMsg || !logMsg->prefix || !logMsg->text || logMsg->log_level > MPV_LOG_LEVEL_WARN) {
                break;
            }
            const QString prefix = QString::fromUtf8(logMsg->prefix);
            const QString text = StreamSecurity::sanitizeDisplayText(QString::fromUtf8(logMsg->text), 140);
            if (text.isEmpty()) {
                break;
            }
            const bool isAudioSubsystem = prefix == QLatin1String("ao")
                                          || prefix.startsWith(QStringLiteral("ao/"))
                                          || prefix == QLatin1String("ad");
            if (isAudioSubsystem) {
                m_lastAudioError = text;
                refreshAudioDiagnostics();
            } else if (prefix == QLatin1String("ffmpeg") || prefix == QLatin1String("lavf")
                       || prefix == QLatin1String("stream") || prefix == QLatin1String("hls")) {
                const QString lowerText = text.toLower();
                if (lowerText.contains(QStringLiteral("403 forbidden"))
                    || lowerText.contains(QStringLiteral("http error 403"))) {
                    m_lastNetworkError = QStringLiteral("Yayıncı akışı erişime kapalı / korumalı (HTTP 403).");
                } else if (lowerText.contains(QStringLiteral("404 not found"))
                           || lowerText.contains(QStringLiteral("http error 404"))) {
                    m_lastNetworkError = QStringLiteral("Yayın kaynağı bulunamadı (HTTP 404).");
                } else if (lowerText.contains(QStringLiteral("451"))) {
                    m_lastNetworkError = QStringLiteral("Yayın coğrafi veya yasal erişim kısıtlı (HTTP 451).");
                } else if (lowerText.contains(QStringLiteral("401 unauthorized"))
                           || lowerText.contains(QStringLiteral("http error 401"))) {
                    m_lastNetworkError = QStringLiteral("Yayın oturum belirteci reddedildi (HTTP 401).");
                }
            }
            break;
        }

        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *prop = static_cast<const mpv_event_property *>(event->data);
            if (!prop || !prop->name) {
                break;
            }
            const QByteArray propName(prop->name);
            if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                const bool flag = (*static_cast<const int *>(prop->data)) != 0;
                if (propName == "pause") {
                    if (m_paused != flag) {
                        m_paused = flag;
                        emit pausedChanged();
                    }
                    if (m_playbackState == QStringLiteral("playing") && m_paused) {
                        setPlaybackState(QStringLiteral("paused"));
                    } else if (m_playbackState == QStringLiteral("paused") && !m_paused) {
                        setPlaybackState(QStringLiteral("playing"));
                    }
                } else if (propName == "paused-for-cache") {
                    if (flag && !m_stoppingIntentionally && m_playbackState != QStringLiteral("error")) {
                        setPlaybackState(QStringLiteral("buffering"));
                    } else if (!flag && m_playbackState == QStringLiteral("buffering")) {
                        setPlaybackState(m_paused ? QStringLiteral("paused") : QStringLiteral("playing"));
                    }
                }
            } else if (prop->format == MPV_FORMAT_STRING) {
                const QString val = (prop->data && *static_cast<char *const *>(prop->data))
                    ? QString::fromUtf8(*static_cast<char *const *>(prop->data))
                    : QString();
                if (propName == "current-ao") {
                    m_observedCurrentAo = val.trimmed();
                    if (!m_observedCurrentAo.isEmpty() && m_observedCurrentAo != QLatin1String("null")) {
                        m_lastAudioError.clear();
                    }
                    refreshAudioDiagnostics();
                } else if (propName == "audio-device") {
                    m_observedAudioDevice = val.trimmed();
                    refreshAudioDiagnostics();
                } else if (propName == "aid") {
                    m_observedAid = val.trimmed();
                    refreshAudioDiagnostics();
                }
            } else if (prop->format == MPV_FORMAT_INT64) {
                const qint64 num = prop->data ? static_cast<qint64>(*static_cast<const int64_t *>(prop->data)) : 0;
                if (propName == "current-tracks/audio/id") {
                    m_observedAudioTrackId = num;
                    refreshAudioDiagnostics();
                } else if (propName == "audio-params/samplerate") {
                    m_observedSampleRate = num;
                    refreshAudioDiagnostics();
                }
            } else if (prop->format == MPV_FORMAT_NONE) {
                if (propName == "current-ao") {
                    m_observedCurrentAo.clear();
                    refreshAudioDiagnostics();
                } else if (propName == "current-tracks/audio/id") {
                    m_observedAudioTrackId = 0;
                    refreshAudioDiagnostics();
                } else if (propName == "audio-params/samplerate") {
                    m_observedSampleRate = 0;
                    refreshAudioDiagnostics();
                }
            }
            break;
        }

        case MPV_EVENT_END_FILE: {
            const auto *eof = static_cast<const mpv_event_end_file *>(event->data);
            if (!eof) {
                break;
            }
            if (m_stoppingIntentionally || eof->reason == MPV_END_FILE_REASON_STOP
                || eof->reason == MPV_END_FILE_REASON_REDIRECT) {
                break;
            }
            if (eof->reason == MPV_END_FILE_REASON_ERROR) {
                if (!m_lastNetworkError.isEmpty()) {
                    scheduleRetry(m_lastNetworkError);
                } else {
                    const QString detail = QString::fromUtf8(mpv_error_string(eof->error));
                    scheduleRetry(QStringLiteral("Yayın şu anda alınamıyor (%1).").arg(detail));
                }
            } else if (eof->reason == MPV_END_FILE_REASON_EOF && !m_currentUrl.isEmpty()) {
                scheduleRetry(m_lastNetworkError.isEmpty()
                                  ? QStringLiteral("Canlı yayın akışı kesildi.")
                                  : m_lastNetworkError);
            }
            break;
        }

        default:
            break;
        }
    }
}

void MpvItem::scheduleRetry(const QString &reason)
{
    // Terminal HTTP auth/geo/not-found errors should fail fast so fallback chains
    // or clean error messages surface immediately instead of stalling for 19 seconds.
    const bool terminalHttpError = reason.contains(QStringLiteral("HTTP 403"))
                                   || reason.contains(QStringLiteral("HTTP 404"))
                                   || reason.contains(QStringLiteral("HTTP 451"))
                                   || reason.contains(QStringLiteral("HTTP 401"));
    if (terminalHttpError || m_currentUrl.isEmpty() || !StreamSecurity::isAllowedMediaUrl(m_currentUrl)) {
        setRetrying(false, 0);
        setErrorMessage(reason);
        setPlaybackState(QStringLiteral("error"));
        return;
    }

    setPlaybackState(QStringLiteral("error"));

    if (m_retryAttempt >= kMaxAutoRetries) {
        setRetrying(false, m_retryAttempt);
        setErrorMessage(QStringLiteral("%1 Otomatik yeniden deneme sınırına ulaşıldı.").arg(reason));
        return;
    }

    const int delayMs = kRetryDelaysMs[static_cast<size_t>(m_retryAttempt)];
    setRetrying(true, m_retryAttempt + 1);
    setErrorMessage(QStringLiteral("Yayın şu anda alınamıyor / Yeniden deneniyor (%1/%2)...")
                        .arg(m_retryAttempt)
                        .arg(kMaxAutoRetries));
    m_retryTimer.start(delayMs);
}

void MpvItem::performScheduledRetry()
{
    if (m_currentUrl.isEmpty() || !StreamSecurity::isAllowedMediaUrl(m_currentUrl)) {
        cancelRetry();
        return;
    }
    setPlaybackState(QStringLiteral("loading"));
    sendLoadFile(m_currentUrl);
}

void MpvItem::cancelRetry()
{
    m_retryTimer.stop();
    setRetrying(false, 0);
}

void MpvItem::setPlaybackState(const QString &state)
{
    if (m_playbackState != state) {
        m_playbackState = state;
        emit playbackStateChanged();
    }
}

void MpvItem::setErrorMessage(const QString &msg)
{
    if (m_errorMessage != msg) {
        m_errorMessage = msg;
        emit errorMessageChanged();
    }
}

void MpvItem::setRetrying(bool retrying, int attempt)
{
    if (m_retrying != retrying) {
        m_retrying = retrying;
        emit retryingChanged();
    }
    if (m_retryAttempt != attempt) {
        m_retryAttempt = attempt;
        emit retryAttemptChanged();
    }
}

void MpvItem::refreshAudioDiagnostics()
{
    const bool hasTrack = (m_observedAudioTrackId > 0)
                          || (m_observedSampleRate > 0)
                          || (!m_observedAid.isEmpty()
                              && m_observedAid != QLatin1String("no")
                              && m_observedAid != QLatin1String("auto"));
    const QString ao = m_observedCurrentAo;

    if (m_playbackState == QLatin1String("idle")) {
        updateAudioDiagnostics(false, ao, false, QStringLiteral("Ses hazır (PipeWire/PulseAudio/ALSA)"));
        return;
    }
    if (m_playbackState == QLatin1String("resolving") || m_playbackState == QLatin1String("loading")) {
        updateAudioDiagnostics(hasTrack, ao, false, QStringLiteral("Ses akışı bekleniyor..."));
        return;
    }

    if (hasTrack && !ao.isEmpty() && ao != QLatin1String("null")) {
        const QString driver = formatAudioDriverLabel(ao);
        QString rateSuffix;
        if (m_observedSampleRate > 0) {
            const double khz = static_cast<double>(m_observedSampleRate) / 1000.0;
            const int decimals = (m_observedSampleRate % 1000 == 0) ? 0 : 1;
            rateSuffix = QStringLiteral(" • %1 kHz").arg(khz, 0, 'f', decimals);
        }
        const QString status = m_muted
            ? QStringLiteral("Sessiz (%1)").arg(driver)
            : QStringLiteral("Ses aktif (%1%2)").arg(driver, rateSuffix);
        updateAudioDiagnostics(true, ao, false, status);
        return;
    }

    if (hasTrack && (ao.isEmpty() || ao == QLatin1String("null"))) {
        const QString status = m_lastAudioError.isEmpty()
            ? QStringLiteral("Ses çıkışı açılamadı (PipeWire/PulseAudio/ALSA)")
            : QStringLiteral("Ses çıkışı hatası: %1").arg(m_lastAudioError);
        updateAudioDiagnostics(true, ao, true, status);
        return;
    }

    if (m_playbackState == QLatin1String("playing") || m_playbackState == QLatin1String("paused")) {
        const bool aoErr = !m_lastAudioError.isEmpty();
        const QString status = aoErr
            ? QStringLiteral("Ses izi/çıkışı yok: %1").arg(m_lastAudioError)
            : QStringLiteral("Bu yayında ses izi bulunamadı");
        updateAudioDiagnostics(false, ao, aoErr, status);
        return;
    }

    updateAudioDiagnostics(hasTrack, ao, !m_lastAudioError.isEmpty(),
                           m_lastAudioError.isEmpty()
                               ? QStringLiteral("Ses bekleniyor...")
                               : QStringLiteral("Ses uyarısı: %1").arg(m_lastAudioError));
}

void MpvItem::updateAudioDiagnostics(bool hasTrack,
                                     const QString &currentAo,
                                     bool aoError,
                                     const QString &statusText)
{
    if (m_hasAudioTrack == hasTrack && m_currentAudioOutput == currentAo
        && m_audioOutputError == aoError && m_audioStatus == statusText) {
        return;
    }
    m_hasAudioTrack = hasTrack;
    m_currentAudioOutput = currentAo;
    m_audioOutputError = aoError;
    m_audioStatus = statusText;
    emit audioStatusChanged();
}

} // namespace zethropol::tv
