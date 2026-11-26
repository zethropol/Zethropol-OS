#include "MpvController.h"
#include "StreamResolver.h"
#include "StreamSecurity.h"

#include <mpv/client.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <string>

namespace zethropol::tvwin7 {

namespace {

std::wstring utf8ToWideLocal(const std::string &s)
{
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string formatWin32Error(DWORD errCode)
{
    char buf[256] = {};
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, errCode, 0, buf, sizeof(buf) - 1, nullptr);
    if (len == 0) {
        return "Hata kodu: " + std::to_string(errCode);
    }
    return StreamSecurity::trimAscii(std::string(buf, len));
}

bool containsHttpTerminalError(const std::string &line, int &httpCodeOut)
{
    const std::string lower = StreamSecurity::toLowerAscii(line);
    if (lower.find("http error 403") != std::string::npos || lower.find("403 forbidden") != std::string::npos) {
        httpCodeOut = 403;
        return true;
    }
    if (lower.find("http error 404") != std::string::npos || lower.find("404 not found") != std::string::npos) {
        httpCodeOut = 404;
        return true;
    }
    if (lower.find("http error 401") != std::string::npos || lower.find("401 unauthorized") != std::string::npos) {
        httpCodeOut = 401;
        return true;
    }
    if (lower.find("http error 451") != std::string::npos) {
        httpCodeOut = 451;
        return true;
    }
    return false;
}

} // namespace

struct MpvController::Api
{
    unsigned long (*client_api_version)(void) = nullptr;
    mpv_handle *(*create)(void) = nullptr;
    int (*initialize)(mpv_handle *) = nullptr;
    void (*terminate_destroy)(mpv_handle *) = nullptr;
    int (*set_option)(mpv_handle *, const char *, mpv_format, void *) = nullptr;
    int (*set_option_string)(mpv_handle *, const char *, const char *) = nullptr;
    int (*set_property)(mpv_handle *, const char *, mpv_format, void *) = nullptr;
    int (*set_property_string)(mpv_handle *, const char *, const char *) = nullptr;
    int (*command)(mpv_handle *, const char **) = nullptr;
    int (*command_async)(mpv_handle *, uint64_t, const char **) = nullptr;
    int (*command_string)(mpv_handle *, const char *) = nullptr;
    int (*observe_property)(mpv_handle *, uint64_t, const char *, mpv_format) = nullptr;
    mpv_event *(*wait_event)(mpv_handle *, double) = nullptr;
    void (*set_wakeup_callback)(mpv_handle *, void (*)(void *), void *) = nullptr;
    int (*request_log_messages)(mpv_handle *, const char *) = nullptr;
    const char *(*error_string)(int) = nullptr;
};

MpvController::MpvController() = default;

MpvController::~MpvController()
{
    shutdown();
}

void MpvController::onWakeup(void *ctx)
{
    auto *self = static_cast<MpvController *>(ctx);
    if (self && self->m_cfg.notifyHwnd && self->m_cfg.wakeupMsg != 0) {
        PostMessageW(self->m_cfg.notifyHwnd, self->m_cfg.wakeupMsg, 0, 0);
    }
}

bool MpvController::setOptStr(const char *name, const char *val)
{
    if (!m_mpv || !m_api || !m_api->set_option_string) {
        return false;
    }
    return m_api->set_option_string(m_mpv, name, val) >= 0;
}

bool MpvController::setPropStr(const char *name, const char *val)
{
    if (!m_mpv || !m_api || !m_api->set_property_string) {
        return false;
    }
    return m_api->set_property_string(m_mpv, name, val) >= 0;
}

bool MpvController::initialize(const MpvPlaybackConfig &cfg, MpvCallbacks callbacks, std::string *errorOut)
{
    shutdown();
    m_cfg = cfg;
    m_cb = std::move(callbacks);
    m_volume = std::clamp(cfg.initialVolume, 0, 100);
    m_muted = cfg.initialMuted;
    m_hlsBitrateCap = cfg.hlsBitrateCap;

    std::setlocale(LC_NUMERIC, "C");

    std::wstring dllPath = utf8ToWideLocal(cfg.mpvDllDirUtf8);
    if (!dllPath.empty() && dllPath.back() != L'\\' && dllPath.back() != L'/') {
        dllPath += L'\\';
    }
    dllPath += L"libmpv-2.dll";

    m_dll = LoadLibraryW(dllPath.c_str());
    if (!m_dll) {
        const DWORD err = GetLastError();
        if (errorOut) {
            *errorOut = "libmpv-2.dll yüklenemedi (" + formatWin32Error(err)
                        + "). Paket dosyalarının eksiksiz olduğundan emin olun.";
        }
        return false;
    }

    m_api = new Api();
    auto loadSym = [&](auto &fnPtr, const char *symName) -> bool {
        FARPROC p = GetProcAddress(m_dll, symName);
        if (!p) {
            if (errorOut) {
                *errorOut = std::string("libmpv-2.dll içinde gerekli sembol eksik: ") + symName;
            }
            return false;
        }
        fnPtr = reinterpret_cast<std::remove_reference_t<decltype(fnPtr)>>(reinterpret_cast<void *>(p));
        return true;
    };

    const bool allLoaded =
        loadSym(m_api->client_api_version, "mpv_client_api_version")
        && loadSym(m_api->create, "mpv_create")
        && loadSym(m_api->initialize, "mpv_initialize")
        && loadSym(m_api->terminate_destroy, "mpv_terminate_destroy")
        && loadSym(m_api->set_option, "mpv_set_option")
        && loadSym(m_api->set_option_string, "mpv_set_option_string")
        && loadSym(m_api->set_property, "mpv_set_property")
        && loadSym(m_api->set_property_string, "mpv_set_property_string")
        && loadSym(m_api->command, "mpv_command")
        && loadSym(m_api->command_async, "mpv_command_async")
        && loadSym(m_api->command_string, "mpv_command_string")
        && loadSym(m_api->observe_property, "mpv_observe_property")
        && loadSym(m_api->wait_event, "mpv_wait_event")
        && loadSym(m_api->set_wakeup_callback, "mpv_set_wakeup_callback")
        && loadSym(m_api->request_log_messages, "mpv_request_log_messages")
        && loadSym(m_api->error_string, "mpv_error_string");

    if (!allLoaded) {
        shutdown();
        return false;
    }

    m_apiVersion = m_api->client_api_version();

    m_mpv = m_api->create();
    if (!m_mpv) {
        if (errorOut) {
            *errorOut = "libmpv oynatıcı örneği oluşturulamadı.";
        }
        shutdown();
        return false;
    }

    // Mandatory security locks (identical to Zethropol OS Stage 05)
    setOptStr("config", "no");
    setOptStr("load-scripts", "no");
    setOptStr("ytdl", "no");
    setOptStr("load-unsafe-playlists", "no");
    setOptStr("osc", "no");
    setOptStr("osd-level", "0");
    setOptStr("input-default-bindings", "no");
    setOptStr("input-vo-keyboard", "no");
    setOptStr("force-window", "no");
    setOptStr("keep-open", "no");
    setOptStr("idle", "yes");

    // TLS verification using bundled up-to-date CA root store (independent of Windows 7 SChannel)
    setOptStr("tls-verify", "yes");
    if (!cfg.caBundlePathUtf8.empty()) {
        setOptStr("tls-ca-file", cfg.caBundlePathUtf8.c_str());
    }

    // Windows 7 Starter low-spec GPU & CPU configuration
    if (cfg.headlessSmokeMode) {
        setOptStr("vo", "null");
        setOptStr("ao", "null");
    } else {
        if (cfg.videoHwnd) {
            int64_t wid = static_cast<int64_t>(reinterpret_cast<intptr_t>(cfg.videoHwnd));
            m_api->set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);
        }
        // Direct3D 9 primary for Intel GMA / WDDM 1.0-1.1 stability; GPU fallback for modern Windows 10/11
        setOptStr("vo", "direct3d,gpu");
        setOptStr("gpu-api", "d3d11,opengl");
        setOptStr("hwdec", "no");
        setOptStr("ao", "wasapi,winmm");
    }

    setOptStr("aid", "auto");
    setOptStr("alang", "tr,tur,en");
    setOptStr("audio-client-name", "Zethropol TV");
    setOptStr("mute", m_muted ? "yes" : "no");
    setOptStr("volume", std::to_string(m_volume).c_str());

    // Low-RAM HLS tuning for Atom/Pentium CPUs
    setOptStr("cache", "yes");
    setOptStr("demuxer-max-bytes", "12MiB");
    setOptStr("demuxer-max-back-bytes", "4MiB");
    setOptStr("network-timeout", "15");
    setOptStr("hls-bitrate", m_hlsBitrateCap ? "2500000" : "max");
    setOptStr("user-agent", StreamResolver::kDesktopBrowserUa);

    const int initRc = m_api->initialize(m_mpv);
    if (initRc < 0) {
        if (errorOut) {
            *errorOut = std::string("libmpv başlatılamadı: ") + m_api->error_string(initRc);
        }
        shutdown();
        return false;
    }

    // Map double-click, F11 and ESC inside the video HWND to client messages (works even with load-scripts=no)
    if (!cfg.headlessSmokeMode) {
        m_api->command_string(m_mpv, "keybind MBTN_LEFT_DBL \"script-message toggle-fullscreen\"");
        m_api->command_string(m_mpv, "keybind F11 \"script-message toggle-fullscreen\"");
        m_api->command_string(m_mpv, "keybind ESC \"script-message exit-fullscreen\"");
    }

    m_api->request_log_messages(m_mpv, "warn");
    m_api->observe_property(m_mpv, 1, "paused-for-cache", MPV_FORMAT_FLAG);
    m_api->observe_property(m_mpv, 2, "current-ao", MPV_FORMAT_STRING);
    m_api->observe_property(m_mpv, 3, "current-vo", MPV_FORMAT_STRING);

    if (cfg.notifyHwnd && cfg.wakeupMsg != 0) {
        m_api->set_wakeup_callback(m_mpv, &MpvController::onWakeup, this);
    }

    applyAudioOptions();
    applyBitrateOption();
    return true;
}

void MpvController::shutdown()
{
    if (m_mpv && m_api) {
        if (m_api->set_wakeup_callback) {
            m_api->set_wakeup_callback(m_mpv, nullptr, nullptr);
        }
        if (m_api->terminate_destroy) {
            m_api->terminate_destroy(m_mpv);
        }
        m_mpv = nullptr;
    }
    if (m_api) {
        delete m_api;
        m_api = nullptr;
    }
    if (m_dll) {
        FreeLibrary(m_dll);
        m_dll = nullptr;
    }
    m_isPlaying = false;
    m_streamActive = false;
}

void MpvController::applyAudioOptions()
{
    if (!m_mpv || !m_api) {
        return;
    }
    int muteFlag = m_muted ? 1 : 0;
    m_api->set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
    double vol = static_cast<double>(std::clamp(m_volume, 0, 100));
    m_api->set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvController::applyBitrateOption()
{
    if (!m_mpv || !m_api) {
        return;
    }
    setPropStr("hls-bitrate", m_hlsBitrateCap ? "2500000" : "max");
}

bool MpvController::playUrl(const std::string &url,
                            const std::string &httpReferer,
                            const std::string &httpUserAgent)
{
    if (!m_mpv || !m_api) {
        return false;
    }
    if (!StreamSecurity::isAllowedMediaUrl(url)) {
        return false;
    }

    m_terminalHttpErrorSeen = false;
    m_terminalHttpErrorMsg.clear();
    m_isPlaying = false;
    m_streamActive = true;

    const std::string safeRef = StreamSecurity::isAllowedMediaUrl(httpReferer) ? httpReferer : std::string();
    setPropStr("referrer", safeRef.c_str());

    const std::string safeUa = StreamSecurity::sanitizeUserAgent(httpUserAgent, StreamResolver::kDesktopBrowserUa);
    setPropStr("user-agent", safeUa.c_str());

    applyAudioOptions();
    applyBitrateOption();

    const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
    return m_api->command_async(m_mpv, 0, cmd) >= 0;
}

void MpvController::stop()
{
    m_isPlaying = false;
    m_streamActive = false;
    m_terminalHttpErrorSeen = false;
    if (!m_mpv || !m_api) {
        return;
    }
    const char *cmd[] = {"stop", nullptr};
    m_api->command_async(m_mpv, 0, cmd);
}

void MpvController::setVolume(int volume)
{
    m_volume = std::clamp(volume, 0, 100);
    applyAudioOptions();
}

void MpvController::setMuted(bool muted)
{
    m_muted = muted;
    if (!m_muted && m_volume == 0) {
        m_volume = 80;
    }
    applyAudioOptions();
}

void MpvController::setHlsBitrateCap(bool cap720p)
{
    m_hlsBitrateCap = cap720p;
    applyBitrateOption();
}

void MpvController::processEvents()
{
    if (!m_mpv || !m_api) {
        return;
    }
    while (true) {
        mpv_event *ev = m_api->wait_event(m_mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) {
            break;
        }
        switch (ev->event_id) {
        case MPV_EVENT_PLAYBACK_RESTART:
            m_isPlaying = true;
            applyAudioOptions();
            if (m_cb.onPlaybackStarted) {
                m_cb.onPlaybackStarted();
            }
            break;

        case MPV_EVENT_END_FILE: {
            const auto *ef = static_cast<const mpv_event_end_file *>(ev->data);
            m_isPlaying = false;
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                m_streamActive = false;
                std::string msg = m_terminalHttpErrorSeen
                    ? m_terminalHttpErrorMsg
                    : (std::string("Yayın akışı açılamadı (") + m_api->error_string(ef->error) + ").");
                if (m_cb.onPlaybackError) {
                    m_cb.onPlaybackError(msg);
                }
            }
            break;
        }

        case MPV_EVENT_LOG_MESSAGE: {
            const auto *lm = static_cast<const mpv_event_log_message *>(ev->data);
            if (lm && lm->text && m_streamActive) {
                int httpCode = 0;
                if (containsHttpTerminalError(lm->text, httpCode) && !m_terminalHttpErrorSeen) {
                    m_terminalHttpErrorSeen = true;
                    m_terminalHttpErrorMsg = "Akış erişime kapalı / bulunamadı (HTTP " + std::to_string(httpCode) + ").";
                    const char *stopCmd[] = {"stop", nullptr};
                    m_api->command_async(m_mpv, 0, stopCmd);
                    m_streamActive = false;
                    m_isPlaying = false;
                    if (m_cb.onPlaybackError) {
                        m_cb.onPlaybackError(m_terminalHttpErrorMsg);
                    }
                }
            }
            break;
        }

        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *prop = static_cast<const mpv_event_property *>(ev->data);
            if (!prop) {
                break;
            }
            if (ev->reply_userdata == 1 && prop->format == MPV_FORMAT_FLAG && prop->data) {
                const bool buffering = (*static_cast<const int *>(prop->data)) != 0;
                if (m_cb.onBufferingChanged) {
                    m_cb.onBufferingChanged(buffering);
                }
            } else if (ev->reply_userdata == 2 && prop->format == MPV_FORMAT_STRING && prop->data) {
                const char *s = *static_cast<const char *const *>(prop->data);
                m_currentAo = s ? s : "";
                if (m_cb.onAudioOutputChanged) {
                    m_cb.onAudioOutputChanged(m_currentAo);
                }
            } else if (ev->reply_userdata == 3 && prop->format == MPV_FORMAT_STRING && prop->data) {
                const char *s = *static_cast<const char *const *>(prop->data);
                m_currentVo = s ? s : "";
            }
            break;
        }

        case MPV_EVENT_CLIENT_MESSAGE: {
            const auto *cm = static_cast<const mpv_event_client_message *>(ev->data);
            if (cm && cm->num_args > 0 && cm->args && cm->args[0] && m_cb.onClientMessage) {
                m_cb.onClientMessage(cm->args[0]);
            }
            break;
        }

        default:
            break;
        }
    }
}

} // namespace zethropol::tvwin7
