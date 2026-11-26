#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

struct mpv_handle;

namespace zethropol::tvwin7 {

struct MpvPlaybackConfig
{
    HWND videoHwnd = nullptr;
    HWND notifyHwnd = nullptr;
    UINT wakeupMsg = 0;
    std::string caBundlePathUtf8;
    std::string mpvDllDirUtf8;
    int initialVolume = 80;
    bool initialMuted = false;
    bool hlsBitrateCap = true;
    bool headlessSmokeMode = false;
};

struct MpvCallbacks
{
    std::function<void()> onPlaybackStarted;
    std::function<void(bool buffering)> onBufferingChanged;
    std::function<void(const std::string &errorMsg)> onPlaybackError;
    std::function<void(const std::string &cmd)> onClientMessage;
    std::function<void(const std::string &aoName)> onAudioOutputChanged;
};

class MpvController
{
public:
    MpvController();
    ~MpvController();

    MpvController(const MpvController &) = delete;
    MpvController &operator=(const MpvController &) = delete;

    bool initialize(const MpvPlaybackConfig &cfg, MpvCallbacks callbacks, std::string *errorOut = nullptr);
    void shutdown();

    bool playUrl(const std::string &url,
                 const std::string &httpReferer = {},
                 const std::string &httpUserAgent = {});
    void stop();
    void setVolume(int volume);
    void setMuted(bool muted);
    void setHlsBitrateCap(bool cap720p);
    void processEvents();

    bool isInitialized() const { return m_mpv != nullptr; }
    bool isPlaying() const { return m_isPlaying; }
    std::string currentAudioOutput() const { return m_currentAo; }
    std::string currentVideoOutput() const { return m_currentVo; }
    unsigned long clientApiVersion() const { return m_apiVersion; }

private:
    static void onWakeup(void *ctx);
    void applyAudioOptions();
    void applyBitrateOption();
    bool setOptStr(const char *name, const char *val);
    bool setPropStr(const char *name, const char *val);

    HMODULE m_dll = nullptr;
    mpv_handle *m_mpv = nullptr;
    MpvPlaybackConfig m_cfg;
    MpvCallbacks m_cb;

    int m_volume = 80;
    bool m_muted = false;
    bool m_hlsBitrateCap = true;
    bool m_isPlaying = false;
    bool m_streamActive = false;
    bool m_terminalHttpErrorSeen = false;
    std::string m_terminalHttpErrorMsg;
    std::string m_currentAo;
    std::string m_currentVo;
    unsigned long m_apiVersion = 0;

    // Function pointers loaded dynamically from libmpv-2.dll
    struct Api;
    Api *m_api = nullptr;
};

} // namespace zethropol::tvwin7
