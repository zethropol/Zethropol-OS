#pragma once

#include "ChannelCatalog.h"
#include "HttpsClient.h"
#include "MpvController.h"
#include "StreamResolver.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace zethropol::tvwin7 {

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    bool create(HINSTANCE hInstance,
                const std::string &exeDirUtf8,
                const std::string &statePathUtf8,
                const std::string &caBundlePathUtf8);
    int runMessageLoop();

    static std::wstring utf8ToWide(const std::string &s);
    static std::string wideToUtf8(const std::wstring &w);

private:
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK staticVideoProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layoutControls();
    void applyCategory(const std::string &category, bool updateSelection);
    void populateChannelList();
    void selectChannelByIndex(int channelIndex, bool forcePageResolve = false);
    void startWorkerResolve(std::shared_ptr<StreamResolveSession> session, uint64_t requestId);
    void cancelActiveWorker();
    void onWorkerStatus(uint64_t requestId, const std::string &statusText);
    void onWorkerCompleted(uint64_t requestId, const ResolveOutcome &outcome);
    void onMpvPlaybackError(const std::string &errMsg);

    void toggleFullscreen();
    void exitFullscreen();
    void toggleSidebar();
    void toggleMute();
    void toggleQualityCap();
    void setStatus(const std::string &badge, COLORREF badgeColor, const std::string &text);
    void saveState();

    void drawOwnerButton(const DRAWITEMSTRUCT *dis);
    void drawChannelListItem(const DRAWITEMSTRUCT *dis);
    void paintMainBackground(HDC hdc);

    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_videoHost = nullptr;
    HWND m_channelList = nullptr;
    HWND m_volumeSlider = nullptr;
    HWND m_btnRefresh = nullptr;
    HWND m_btnQuality = nullptr;
    HWND m_btnMute = nullptr;
    HWND m_btnFullscreen = nullptr;
    HWND m_btnSidebar = nullptr;
    std::vector<HWND> m_categoryButtons;

    HFONT m_fontTitle = nullptr;
    HFONT m_fontBold = nullptr;
    HFONT m_fontNormal = nullptr;
    HFONT m_fontSmall = nullptr;
    HICON m_iconBig = nullptr;
    HICON m_iconSmall = nullptr;
    HBRUSH m_brushBg = nullptr;
    HBRUSH m_brushPanel = nullptr;
    HBRUSH m_brushBar = nullptr;

    std::string m_exeDirUtf8;
    std::string m_statePathUtf8;
    std::string m_caBundlePathUtf8;

    std::vector<ChannelEntry> m_channels;
    std::vector<std::string> m_categories;
    std::vector<int> m_filteredIndices;
    PortableState m_state;
    int m_activeChannelIndex = -1;

    HttpsClient m_https;
    MpvController m_mpv;

    std::shared_ptr<StreamResolveSession> m_activeSession;
    std::atomic<uint64_t> m_requestSeq{0};
    std::shared_ptr<std::atomic<bool>> m_activeCancelFlag;
    std::thread m_workerThread;

    bool m_fullscreen = false;
    bool m_sidebarVisible = true;
    DWORD m_savedStyle = 0;
    WINDOWPLACEMENT m_savedPlacement{};

    std::string m_statusBadge = "HAZIR";
    COLORREF m_statusBadgeColor = RGB(14, 165, 233);
    std::string m_statusText = "Zethropol TV başlatılıyor...";
};

} // namespace zethropol::tvwin7
