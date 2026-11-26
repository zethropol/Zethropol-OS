#include "MainWindow.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>

namespace zethropol::tvwin7 {

namespace {

constexpr UINT WM_APP_MPV_WAKEUP = WM_APP + 10;
constexpr UINT WM_APP_RESOLVE_STATUS = WM_APP + 11;
constexpr UINT WM_APP_RESOLVE_DONE = WM_APP + 12;

constexpr WORD kAppIconResourceId = 101;

constexpr int kIdCategoryBase = 1000; // 1000..1006
constexpr int kIdChannelList = 1100;
constexpr int kIdBtnRefresh = 1201;
constexpr int kIdBtnQuality = 1202;
constexpr int kIdBtnMute = 1203;
constexpr int kIdVolumeSlider = 1204;
constexpr int kIdBtnFullscreen = 1205;
constexpr int kIdBtnSidebar = 1206;

constexpr COLORREF kColBg = RGB(13, 17, 23);
constexpr COLORREF kColPanel = RGB(17, 22, 29);
constexpr COLORREF kColBar = RGB(22, 27, 34);
constexpr COLORREF kColBorder = RGB(48, 54, 61);
constexpr COLORREF kColCard = RGB(26, 33, 44);
constexpr COLORREF kColCardHover = RGB(36, 46, 62);
constexpr COLORREF kColAccent = RGB(14, 165, 233);
constexpr COLORREF kColAccentDark = RGB(12, 74, 110);
constexpr COLORREF kColLive = RGB(34, 197, 94);
constexpr COLORREF kColWarn = RGB(245, 158, 11);
constexpr COLORREF kColError = RGB(239, 68, 68);
constexpr COLORREF kColText = RGB(248, 250, 252);
constexpr COLORREF kColMutedText = RGB(148, 163, 184);

struct ResolveStatusPayload
{
    uint64_t requestId = 0;
    std::string text;
};

struct ResolveDonePayload
{
    uint64_t requestId = 0;
    ResolveOutcome outcome;
};

} // namespace

std::wstring MainWindow::utf8ToWide(const std::string &s)
{
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string MainWindow::wideToUtf8(const std::wstring &w)
{
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

MainWindow::MainWindow()
{
    m_savedPlacement.length = sizeof(WINDOWPLACEMENT);
}

MainWindow::~MainWindow()
{
    cancelActiveWorker();
    m_mpv.shutdown();
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_fontBold) DeleteObject(m_fontBold);
    if (m_fontNormal) DeleteObject(m_fontNormal);
    if (m_fontSmall) DeleteObject(m_fontSmall);
    if (m_iconBig) DestroyIcon(m_iconBig);
    if (m_iconSmall) DestroyIcon(m_iconSmall);
    if (m_brushBg) DeleteObject(m_brushBg);
    if (m_brushPanel) DeleteObject(m_brushPanel);
    if (m_brushBar) DeleteObject(m_brushBar);
}

bool MainWindow::create(HINSTANCE hInstance,
                        const std::string &exeDirUtf8,
                        const std::string &statePathUtf8,
                        const std::string &caBundlePathUtf8)
{
    m_hInst = hInstance;
    m_exeDirUtf8 = exeDirUtf8;
    m_statePathUtf8 = statePathUtf8;
    m_caBundlePathUtf8 = caBundlePathUtf8;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    m_channels = ChannelCatalog::buildTurkish31Channels();
    m_categories = ChannelCatalog::uiCategories();
    m_state = ChannelCatalog::loadStateFromFile(m_statePathUtf8);
    // Always open with Haber category selected per Windows 7 Starter UX spec
    m_state.selectedCategory = "Haber";

    m_brushBg = CreateSolidBrush(kColBg);
    m_brushPanel = CreateSolidBrush(kColPanel);
    m_brushBar = CreateSolidBrush(kColBar);

    m_fontTitle = CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_fontBold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_fontSmall = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    m_iconBig = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    m_iconSmall = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::staticWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = m_iconBig;
    wc.hIconSm = m_iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_brushBg;
    wc.lpszClassName = L"ZethropolTvWin7Main";
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    WNDCLASSEXW vwc{};
    vwc.cbSize = sizeof(vwc);
    vwc.style = CS_DBLCLKS;
    vwc.lpfnWndProc = &MainWindow::staticVideoProc;
    vwc.hInstance = hInstance;
    vwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vwc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vwc.lpszClassName = L"ZethropolTvWin7Video";
    RegisterClassExW(&vwc);

    // 960x540 fits comfortably on 1024x600 Windows 7 Starter netbooks
    const int initW = 960;
    const int initH = 540;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int posX = std::max(0, (screenW - initW) / 2);
    const int posY = std::max(0, (screenH - initH) / 2);

    m_hwnd = CreateWindowExW(
        0,
        L"ZethropolTvWin7Main",
        L"Zethropol TV — Windows 7 Portable (32-bit)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        posX, posY, initW, initH,
        nullptr, nullptr, hInstance, this);

    if (!m_hwnd) {
        return false;
    }

    if (m_iconBig) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_iconBig));
    }
    if (m_iconSmall) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_iconSmall));
    }

    createControls();
    layoutControls();

    std::string httpsErr;
    if (!m_https.initialize(m_caBundlePathUtf8, &httpsErr)) {
        setStatus("TLS HATA", kColError, httpsErr);
    }

    MpvPlaybackConfig mpvCfg;
    mpvCfg.videoHwnd = m_videoHost;
    mpvCfg.notifyHwnd = m_hwnd;
    mpvCfg.wakeupMsg = WM_APP_MPV_WAKEUP;
    mpvCfg.caBundlePathUtf8 = m_caBundlePathUtf8;
    mpvCfg.mpvDllDirUtf8 = m_exeDirUtf8;
    mpvCfg.initialVolume = m_state.volume;
    mpvCfg.initialMuted = m_state.muted;
    mpvCfg.hlsBitrateCap = m_state.hlsBitrateCap;

    MpvCallbacks mpvCb;
    mpvCb.onPlaybackStarted = [this]() {
        if (m_activeChannelIndex >= 0 && m_activeChannelIndex < static_cast<int>(m_channels.size())) {
            const auto &ch = m_channels[static_cast<std::size_t>(m_activeChannelIndex)];
            const std::string qLabel = m_state.hlsBitrateCap ? "720p" : "Tam Kalite";
            setStatus("CANLI", kColLive, ch.name + "  •  " + ch.group + "  (" + qLabel + ")");
        }
    };
    mpvCb.onBufferingChanged = [this](bool buffering) {
        if (buffering && m_activeChannelIndex >= 0) {
            const auto &ch = m_channels[static_cast<std::size_t>(m_activeChannelIndex)];
            setStatus("ARABELLEK", kColWarn, ch.name + " yükleniyor...");
        } else if (!buffering && m_mpv.isPlaying() && m_activeChannelIndex >= 0) {
            const auto &ch = m_channels[static_cast<std::size_t>(m_activeChannelIndex)];
            const std::string qLabel = m_state.hlsBitrateCap ? "720p" : "Tam Kalite";
            setStatus("CANLI", kColLive, ch.name + "  •  " + ch.group + "  (" + qLabel + ")");
        }
    };
    mpvCb.onPlaybackError = [this](const std::string &err) { onMpvPlaybackError(err); };
    mpvCb.onClientMessage = [this](const std::string &cmd) {
        if (cmd == "toggle-fullscreen") {
            toggleFullscreen();
        } else if (cmd == "exit-fullscreen") {
            exitFullscreen();
        }
    };

    std::string mpvErr;
    if (!m_mpv.initialize(mpvCfg, std::move(mpvCb), &mpvErr)) {
        setStatus("MPV HATA", kColError, mpvErr);
        MessageBoxW(m_hwnd, utf8ToWide(mpvErr).c_str(), L"Zethropol TV — Başlatma Hatası", MB_OK | MB_ICONERROR);
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    applyCategory("Haber", false);
    const int startupIdx = ChannelCatalog::findStartupChannelIndex(m_channels, m_state);
    if (startupIdx >= 0) {
        selectChannelByIndex(startupIdx, false);
    }
    return true;
}

void MainWindow::createControls()
{
    m_videoHost = CreateWindowExW(
        0, L"ZethropolTvWin7Video", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100, m_hwnd, nullptr, m_hInst, this);

    m_categoryButtons.clear();
    for (std::size_t i = 0; i < m_categories.size(); ++i) {
        const std::wstring label = utf8ToWide(m_categories[i]);
        HWND btn = CreateWindowExW(
            0, L"BUTTON", label.c_str(),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 60, 28,
            m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCategoryBase + static_cast<int>(i))),
            m_hInst, nullptr);
        m_categoryButtons.push_back(btn);
    }

    m_channelList = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 280, 300,
        m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChannelList)),
        m_hInst, nullptr);
    SendMessageW(m_channelList, LB_SETITEMHEIGHT, 0, 42);

    auto makeBarBtn = [&](int id, const wchar_t *text) {
        return CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 74, 34,
            m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            m_hInst, nullptr);
    };

    m_btnRefresh = makeBarBtn(kIdBtnRefresh, L"Yenile");
    m_btnQuality = makeBarBtn(kIdBtnQuality, m_state.hlsBitrateCap ? L"720p HD" : L"Tam Kalite");
    m_btnMute = makeBarBtn(kIdBtnMute, m_state.muted ? L"Sessiz" : L"Ses Açık");
    m_btnFullscreen = makeBarBtn(kIdBtnFullscreen, L"Tam Ekran");
    m_btnSidebar = makeBarBtn(kIdBtnSidebar, L"Kanallar");

    m_volumeSlider = CreateWindowExW(
        0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 96, 28,
        m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdVolumeSlider)),
        m_hInst, nullptr);
    SendMessageW(m_volumeSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(m_volumeSlider, TBM_SETPOS, TRUE, m_state.volume);
}

void MainWindow::layoutControls()
{
    if (!m_hwnd) {
        return;
    }
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));

    if (m_fullscreen) {
        MoveWindow(m_videoHost, 0, 0, w, h, TRUE);
        ShowWindow(m_channelList, SW_HIDE);
        for (HWND b : m_categoryButtons) ShowWindow(b, SW_HIDE);
        ShowWindow(m_btnRefresh, SW_HIDE);
        ShowWindow(m_btnQuality, SW_HIDE);
        ShowWindow(m_btnMute, SW_HIDE);
        ShowWindow(m_volumeSlider, SW_HIDE);
        ShowWindow(m_btnFullscreen, SW_HIDE);
        ShowWindow(m_btnSidebar, SW_HIDE);
        return;
    }

    const int barH = 56;
    const int sidebarW = m_sidebarVisible ? ((w < 1050) ? 286 : 324) : 0;
    const int videoW = std::max(160, w - sidebarW);
    const int videoH = std::max(120, h - barH);

    MoveWindow(m_videoHost, 0, 0, videoW, videoH, TRUE);

    // Bottom bar controls (right-aligned compact buttons, left side reserved for status paint)
    ShowWindow(m_btnRefresh, SW_SHOW);
    ShowWindow(m_btnQuality, SW_SHOW);
    ShowWindow(m_btnMute, SW_SHOW);
    ShowWindow(m_volumeSlider, SW_SHOW);
    ShowWindow(m_btnFullscreen, SW_SHOW);
    ShowWindow(m_btnSidebar, SW_SHOW);

    const int btnY = videoH + 11;
    const int btnH = 34;
    const int gap = 6;
    int rx = w - 10;

    auto placeRight = [&](HWND ctrl, int bw, int bh, int y) {
        rx -= bw;
        MoveWindow(ctrl, rx, y, bw, bh, TRUE);
        rx -= gap;
    };

    placeRight(m_btnSidebar, 76, btnH, btnY);
    placeRight(m_btnFullscreen, 82, btnH, btnY);
    placeRight(m_volumeSlider, 88, 28, videoH + 14);
    placeRight(m_btnMute, 76, btnH, btnY);
    placeRight(m_btnQuality, 84, btnH, btnY);
    placeRight(m_btnRefresh, 64, btnH, btnY);

    // Right channel sidebar
    if (m_sidebarVisible) {
        const int sx = videoW + 8;
        const int innerW = sidebarW - 16;
        const int headerH = 34;

        // 2 rows of category buttons (4 on row 1, 3 on row 2)
        const int catH = 28;
        const int catGap = 4;
        const int row1Count = 4;
        const int row2Count = static_cast<int>(m_categoryButtons.size()) - row1Count;

        const int w1 = (innerW - catGap * (row1Count - 1)) / row1Count;
        for (int i = 0; i < row1Count && i < static_cast<int>(m_categoryButtons.size()); ++i) {
            const int bx = sx + i * (w1 + catGap);
            const int bw = (i == row1Count - 1) ? (sx + innerW - bx) : w1;
            ShowWindow(m_categoryButtons[static_cast<std::size_t>(i)], SW_SHOW);
            MoveWindow(m_categoryButtons[static_cast<std::size_t>(i)], bx, headerH, bw, catH, TRUE);
        }

        if (row2Count > 0) {
            const int w2 = (innerW - catGap * (row2Count - 1)) / row2Count;
            const int row2Y = headerH + catH + catGap;
            for (int j = 0; j < row2Count; ++j) {
                const int idx = row1Count + j;
                const int bx = sx + j * (w2 + catGap);
                const int bw = (j == row2Count - 1) ? (sx + innerW - bx) : w2;
                ShowWindow(m_categoryButtons[static_cast<std::size_t>(idx)], SW_SHOW);
                MoveWindow(m_categoryButtons[static_cast<std::size_t>(idx)], bx, row2Y, bw, catH, TRUE);
            }
        }

        const int listY = headerH + (catH + catGap) * 2 + 6;
        const int listH = std::max(80, videoH - listY - 8);
        ShowWindow(m_channelList, SW_SHOW);
        MoveWindow(m_channelList, sx, listY, innerW, listH, TRUE);
    } else {
        ShowWindow(m_channelList, SW_HIDE);
        for (HWND b : m_categoryButtons) {
            ShowWindow(b, SW_HIDE);
        }
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::applyCategory(const std::string &category, bool updateSelection)
{
    m_state.selectedCategory = category;
    m_filteredIndices = ChannelCatalog::filterIndicesByCategory(m_channels, category);
    populateChannelList();
    for (HWND b : m_categoryButtons) {
        InvalidateRect(b, nullptr, FALSE);
    }
    if (updateSelection) {
        saveState();
    }
}

void MainWindow::populateChannelList()
{
    if (!m_channelList) {
        return;
    }
    SendMessageW(m_channelList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(m_channelList, LB_RESETCONTENT, 0, 0);

    int curSel = -1;
    for (std::size_t row = 0; row < m_filteredIndices.size(); ++row) {
        const int chIdx = m_filteredIndices[row];
        const auto &ch = m_channels[static_cast<std::size_t>(chIdx)];
        const std::wstring wName = utf8ToWide(ch.name);
        const LRESULT added = SendMessageW(m_channelList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wName.c_str()));
        if (added >= 0) {
            SendMessageW(m_channelList, LB_SETITEMDATA, static_cast<WPARAM>(added), static_cast<LPARAM>(chIdx));
            if (chIdx == m_activeChannelIndex) {
                curSel = static_cast<int>(added);
            }
        }
    }
    if (curSel >= 0) {
        SendMessageW(m_channelList, LB_SETCURSEL, static_cast<WPARAM>(curSel), 0);
    }
    SendMessageW(m_channelList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_channelList, nullptr, TRUE);
}

void MainWindow::cancelActiveWorker()
{
    ++m_requestSeq;
    if (m_activeCancelFlag) {
        m_activeCancelFlag->store(true);
    }
    if (m_workerThread.joinable()) {
        m_workerThread.detach();
    }
    m_activeCancelFlag.reset();
}

void MainWindow::selectChannelByIndex(int channelIndex, bool forcePageResolve)
{
    if (channelIndex < 0 || channelIndex >= static_cast<int>(m_channels.size())) {
        return;
    }
    cancelActiveWorker();
    m_activeChannelIndex = channelIndex;
    const auto &ch = m_channels[static_cast<std::size_t>(channelIndex)];
    m_state.lastChannelId = ch.id;
    saveState();

    // Sync selection highlight in visible list if channel is in current category
    for (std::size_t row = 0; row < m_filteredIndices.size(); ++row) {
        if (m_filteredIndices[row] == channelIndex) {
            SendMessageW(m_channelList, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
            break;
        }
    }
    InvalidateRect(m_channelList, nullptr, FALSE);

    const std::wstring title = utf8ToWide(ch.name + " — Zethropol TV (Win7 32-bit)");
    SetWindowTextW(m_hwnd, title.c_str());

    setStatus("BAĞLANIYOR", kColWarn, ch.name + " açılıyor...");

    m_activeSession = std::make_shared<StreamResolveSession>(StreamResolver::createSession(ch, forcePageResolve));
    const uint64_t reqId = ++m_requestSeq;
    startWorkerResolve(m_activeSession, reqId);
}

void MainWindow::startWorkerResolve(std::shared_ptr<StreamResolveSession> session, uint64_t requestId)
{
    if (m_workerThread.joinable()) {
        m_workerThread.detach();
    }
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_activeCancelFlag = cancelFlag;
    HWND targetHwnd = m_hwnd;

    m_workerThread = std::thread([this, session, requestId, cancelFlag, targetHwnd]() {
        HttpFetcher fetcher = [this](const HttpRequest &req) {
            return m_https.fetch(req);
        };
        auto statusCb = [targetHwnd, requestId, cancelFlag](const std::string &msg) {
            if (cancelFlag->load()) {
                return;
            }
            auto *payload = new ResolveStatusPayload{requestId, msg};
            if (!PostMessageW(targetHwnd, WM_APP_RESOLVE_STATUS, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        };

        const ResolveOutcome outcome = StreamResolver::resolveNext(*session, fetcher, cancelFlag.get(), statusCb);
        if (cancelFlag->load()) {
            return;
        }
        auto *donePayload = new ResolveDonePayload{requestId, outcome};
        if (!PostMessageW(targetHwnd, WM_APP_RESOLVE_DONE, 0, reinterpret_cast<LPARAM>(donePayload))) {
            delete donePayload;
        }
    });
}

void MainWindow::onWorkerStatus(uint64_t requestId, const std::string &statusText)
{
    if (requestId != m_requestSeq.load()) {
        return;
    }
    setStatus("ÇÖZÜMLENİYOR", kColWarn, statusText);
}

void MainWindow::onWorkerCompleted(uint64_t requestId, const ResolveOutcome &outcome)
{
    if (requestId != m_requestSeq.load() || outcome.canceled) {
        return;
    }
    if (!outcome.ok) {
        m_mpv.stop();
        const std::string badge = outcome.drmDetected ? "KAPALI / DRM" : "YAYIN YOK";
        setStatus(badge, kColError, outcome.errorMessage);
        return;
    }
    if (m_activeChannelIndex >= 0 && m_activeChannelIndex < static_cast<int>(m_channels.size())) {
        const auto &ch = m_channels[static_cast<std::size_t>(m_activeChannelIndex)];
        setStatus("YÜKLENİYOR", kColWarn, ch.name + " akışı başlatılıyor...");
    }
    if (!m_mpv.playUrl(outcome.streamUrl, outcome.httpReferer, outcome.httpUserAgent)) {
        onMpvPlaybackError("Yayın oynatıcıya aktarılamadı.");
    }
}

void MainWindow::onMpvPlaybackError(const std::string &errMsg)
{
    if (m_activeSession && !m_activeSession->pendingCandidates.empty()) {
        if (m_activeSession->lastFailureReason.empty()) {
            m_activeSession->lastFailureReason = errMsg;
        }
        setStatus("YEDEK KAYNAK", kColWarn, m_activeSession->channelName + " için yedek resmi kaynak deneniyor...");
        const uint64_t reqId = ++m_requestSeq;
        startWorkerResolve(m_activeSession, reqId);
        return;
    }
    setStatus("YAYIN YOK", kColError, errMsg);
}

void MainWindow::toggleFullscreen()
{
    if (m_fullscreen) {
        exitFullscreen();
        return;
    }
    m_savedStyle = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    GetWindowPlacement(m_hwnd, &m_savedPlacement);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        m_fullscreen = true;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(m_savedStyle & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(m_hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        layoutControls();
    }
}

void MainWindow::exitFullscreen()
{
    if (!m_fullscreen) {
        return;
    }
    m_fullscreen = false;
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(m_savedStyle));
    SetWindowPlacement(m_hwnd, &m_savedPlacement);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    layoutControls();
}

void MainWindow::toggleSidebar()
{
    m_sidebarVisible = !m_sidebarVisible;
    layoutControls();
}

void MainWindow::toggleMute()
{
    m_state.muted = !m_state.muted;
    m_mpv.setMuted(m_state.muted);
    if (!m_state.muted && m_state.volume == 0) {
        m_state.volume = 80;
        SendMessageW(m_volumeSlider, TBM_SETPOS, TRUE, m_state.volume);
    }
    SetWindowTextW(m_btnMute, m_state.muted ? L"Sessiz" : L"Ses Açık");
    InvalidateRect(m_btnMute, nullptr, FALSE);
    saveState();
}

void MainWindow::toggleQualityCap()
{
    m_state.hlsBitrateCap = !m_state.hlsBitrateCap;
    m_mpv.setHlsBitrateCap(m_state.hlsBitrateCap);
    SetWindowTextW(m_btnQuality, m_state.hlsBitrateCap ? L"720p HD" : L"Tam Kalite");
    InvalidateRect(m_btnQuality, nullptr, FALSE);
    saveState();
    // Reload active channel cleanly so libmpv switches HLS variant immediately
    if (m_activeChannelIndex >= 0) {
        selectChannelByIndex(m_activeChannelIndex, false);
    }
}

void MainWindow::setStatus(const std::string &badge, COLORREF badgeColor, const std::string &text)
{
    m_statusBadge = badge;
    m_statusBadgeColor = badgeColor;
    m_statusText = text;
    if (m_hwnd && !m_fullscreen) {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        RECT barRc{0, std::max(0, static_cast<int>(rc.bottom) - 56), rc.right, rc.bottom};
        InvalidateRect(m_hwnd, &barRc, FALSE);
    }
}

void MainWindow::saveState()
{
    ChannelCatalog::saveStateToFile(m_statePathUtf8, m_state);
}

void MainWindow::paintMainBackground(HDC hdc)
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (m_fullscreen) {
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return;
    }

    const int barH = 56;
    const int sidebarW = m_sidebarVisible ? ((w < 1050) ? 286 : 324) : 0;
    const int videoW = std::max(160, w - sidebarW);
    const int videoH = std::max(120, h - barH);

    // Sidebar background & header
    if (m_sidebarVisible) {
        RECT sideRc{videoW, 0, w, videoH};
        FillRect(hdc, &sideRc, m_brushPanel);

        HPEN pen = CreatePen(PS_SOLID, 1, kColBorder);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, videoW, 0, nullptr);
        LineTo(hdc, videoW, videoH);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kColText);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, m_fontTitle));
        RECT titleRc{videoW + 10, 6, w - 10, 30};
        const std::wstring header = utf8ToWide("ZETHROPOL TV  (" + std::to_string(m_filteredIndices.size()) + ")");
        DrawTextW(hdc, header.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }

    // Bottom control bar background & top border
    RECT barRc{0, videoH, w, h};
    FillRect(hdc, &barRc, m_brushBar);
    HPEN bPen = CreatePen(PS_SOLID, 1, kColBorder);
    HPEN oldBPen = static_cast<HPEN>(SelectObject(hdc, bPen));
    MoveToEx(hdc, 0, videoH, nullptr);
    LineTo(hdc, w, videoH);
    SelectObject(hdc, oldBPen);
    DeleteObject(bPen);

    // Status pill & live text on left half of bottom bar
    const int statusRight = std::max(120, w - 520);
    SetBkMode(hdc, TRANSPARENT);

    RECT pillRc{12, videoH + 15, 114, videoH + 41};
    HBRUSH pillBrush = CreateSolidBrush(m_statusBadgeColor);
    HPEN pillPen = CreatePen(PS_SOLID, 1, m_statusBadgeColor);
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, pillBrush));
    HPEN oldPn = static_cast<HPEN>(SelectObject(hdc, pillPen));
    RoundRect(hdc, pillRc.left, pillRc.top, pillRc.right, pillRc.bottom, 8, 8);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPn);
    DeleteObject(pillBrush);
    DeleteObject(pillPen);

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, m_fontSmall));
    SetTextColor(hdc, RGB(10, 14, 20));
    const std::wstring wBadge = utf8ToWide(m_statusBadge);
    DrawTextW(hdc, wBadge.c_str(), -1, &pillRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, m_fontBold);
    SetTextColor(hdc, kColText);
    RECT txtRc{124, videoH + 12, statusRight, videoH + 44};
    const std::wstring wTxt = utf8ToWide(m_statusText);
    DrawTextW(hdc, wTxt.c_str(), -1, &txtRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

void MainWindow::drawOwnerButton(const DRAWITEMSTRUCT *dis)
{
    if (!dis) {
        return;
    }
    const int id = static_cast<int>(dis->CtlID);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    bool isActive = false;
    if (id >= kIdCategoryBase && id < kIdCategoryBase + static_cast<int>(m_categories.size())) {
        const std::size_t idx = static_cast<std::size_t>(id - kIdCategoryBase);
        isActive = (m_categories[idx] == m_state.selectedCategory);
    } else if (id == kIdBtnMute) {
        isActive = m_state.muted;
    } else if (id == kIdBtnQuality) {
        isActive = !m_state.hlsBitrateCap;
    }

    COLORREF fill = kColCard;
    COLORREF border = kColBorder;
    COLORREF txtCol = kColText;

    if (isActive) {
        fill = (id == kIdBtnMute) ? RGB(127, 29, 29) : kColAccent;
        border = fill;
        txtCol = (id == kIdBtnMute) ? kColText : RGB(8, 12, 18);
    } else if (pressed) {
        fill = kColCardHover;
        border = kColAccent;
    }

    HBRUSH bg = CreateSolidBrush(fill);
    HPEN pn = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(dis->hDC, bg));
    HPEN oldPn = static_cast<HPEN>(SelectObject(dis->hDC, pn));
    FillRect(dis->hDC, &dis->rcItem, m_brushPanel);
    RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 6, 6);
    SelectObject(dis->hDC, oldBr);
    SelectObject(dis->hDC, oldPn);
    DeleteObject(bg);
    DeleteObject(pn);

    wchar_t label[64] = {};
    GetWindowTextW(dis->hwndItem, label, 64);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, txtCol);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, m_fontSmall));
    RECT trc = dis->rcItem;
    DrawTextW(dis->hDC, label, -1, &trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldFont);
}

void MainWindow::drawChannelListItem(const DRAWITEMSTRUCT *dis)
{
    if (!dis || dis->itemID == static_cast<UINT>(-1)) {
        return;
    }
    const int chIdx = static_cast<int>(dis->itemData);
    if (chIdx < 0 || chIdx >= static_cast<int>(m_channels.size())) {
        return;
    }
    const auto &ch = m_channels[static_cast<std::size_t>(chIdx)];
    const bool isActive = (chIdx == m_activeChannelIndex);
    const bool isSelected = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bgCol = isActive ? kColAccentDark : (isSelected ? kColCardHover : kColPanel);
    HBRUSH bg = CreateSolidBrush(bgCol);
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);

    if (isActive) {
        RECT accentRc{dis->rcItem.left, dis->rcItem.top + 3, dis->rcItem.left + 4, dis->rcItem.bottom - 3};
        HBRUSH ac = CreateSolidBrush(kColAccent);
        FillRect(dis->hDC, &accentRc, ac);
        DeleteObject(ac);
    }

    // Subtle bottom divider
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(30, 38, 49));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
    MoveToEx(dis->hDC, dis->rcItem.left + 6, dis->rcItem.bottom - 1, nullptr);
    LineTo(dis->hDC, dis->rcItem.right - 6, dis->rcItem.bottom - 1);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    SetBkMode(dis->hDC, TRANSPARENT);

    // Channel number on left
    char numBuf[16] = {};
    std::snprintf(numBuf, sizeof(numBuf), "%02d", ch.number);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, m_fontSmall));
    SetTextColor(dis->hDC, isActive ? kColAccent : kColMutedText);
    RECT numRc{dis->rcItem.left + 10, dis->rcItem.top, dis->rcItem.left + 36, dis->rcItem.bottom};
    const std::wstring wNum = utf8ToWide(numBuf);
    DrawTextW(dis->hDC, wNum.c_str(), -1, &numRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Bold high-contrast channel name
    SelectObject(dis->hDC, m_fontBold);
    SetTextColor(dis->hDC, kColText);
    RECT nameRc{dis->rcItem.left + 40, dis->rcItem.top + 2, dis->rcItem.right - 78, dis->rcItem.bottom - 2};
    const std::wstring wName = utf8ToWide(ch.name);
    DrawTextW(dis->hDC, wName.c_str(), -1, &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Category label on right
    SelectObject(dis->hDC, m_fontSmall);
    SetTextColor(dis->hDC, isActive ? kColText : kColMutedText);
    RECT grpRc{dis->rcItem.right - 74, dis->rcItem.top, dis->rcItem.right - 8, dis->rcItem.bottom};
    const std::wstring wGrp = utf8ToWide(ch.group);
    DrawTextW(dis->hDC, wGrp.c_str(), -1, &grpRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dis->hDC, oldFont);
}

LRESULT CALLBACK MainWindow::staticVideoProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        const auto *cs = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto *self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_LBUTTONDBLCLK && self) {
        self->toggleFullscreen();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto *cs = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        auto *self = static_cast<MainWindow *>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    auto *self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        layoutControls();
        return 0;

    case WM_GETMINMAXINFO: {
        auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
        mmi->ptMinTrackSize.x = 800;
        mmi->ptMinTrackSize.y = 480;
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(m_hwnd, &ps);
        paintMainBackground(hdc);
        EndPaint(m_hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wParam), kColPanel);
        SetTextColor(reinterpret_cast<HDC>(wParam), kColText);
        return reinterpret_cast<LRESULT>(m_brushPanel);

    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wParam), kColBar);
        SetTextColor(reinterpret_cast<HDC>(wParam), kColText);
        return reinterpret_cast<LRESULT>(m_brushBar);

    case WM_DRAWITEM: {
        const auto *dis = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
        if (dis && dis->CtlID == static_cast<UINT>(kIdChannelList)) {
            drawChannelListItem(dis);
            return TRUE;
        }
        drawOwnerButton(dis);
        return TRUE;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notif = HIWORD(wParam);
        if (id >= kIdCategoryBase && id < kIdCategoryBase + static_cast<int>(m_categories.size())) {
            applyCategory(m_categories[static_cast<std::size_t>(id - kIdCategoryBase)], true);
            return 0;
        }
        if (id == kIdChannelList && (notif == LBN_SELCHANGE || notif == LBN_DBLCLK)) {
            const LRESULT sel = SendMessageW(m_channelList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                const int chIdx = static_cast<int>(SendMessageW(m_channelList, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0));
                if (chIdx != m_activeChannelIndex || notif == LBN_DBLCLK) {
                    selectChannelByIndex(chIdx, false);
                }
            }
            return 0;
        }
        if (id == kIdBtnRefresh) {
            if (m_activeChannelIndex >= 0) {
                selectChannelByIndex(m_activeChannelIndex, false);
            }
            return 0;
        }
        if (id == kIdBtnQuality) {
            toggleQualityCap();
            return 0;
        }
        if (id == kIdBtnMute) {
            toggleMute();
            return 0;
        }
        if (id == kIdBtnFullscreen) {
            toggleFullscreen();
            return 0;
        }
        if (id == kIdBtnSidebar) {
            toggleSidebar();
            return 0;
        }
        break;
    }

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == m_volumeSlider) {
            const int vol = static_cast<int>(SendMessageW(m_volumeSlider, TBM_GETPOS, 0, 0));
            m_state.volume = std::clamp(vol, 0, 100);
            if (m_state.muted && m_state.volume > 0) {
                m_state.muted = false;
                SetWindowTextW(m_btnMute, L"Ses Açık");
                InvalidateRect(m_btnMute, nullptr, FALSE);
            }
            m_mpv.setMuted(m_state.muted);
            m_mpv.setVolume(m_state.volume);
            saveState();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_F11) {
            toggleFullscreen();
            return 0;
        }
        if (wParam == VK_ESCAPE && m_fullscreen) {
            exitFullscreen();
            return 0;
        }
        if (wParam == 'M') {
            toggleMute();
            return 0;
        }
        if (wParam == 'R' && m_activeChannelIndex >= 0) {
            selectChannelByIndex(m_activeChannelIndex, false);
            return 0;
        }
        if (wParam == 'Q') {
            toggleQualityCap();
            return 0;
        }
        if (wParam == 'C' && !m_fullscreen) {
            toggleSidebar();
            return 0;
        }
        break;

    case WM_LBUTTONDBLCLK:
        toggleFullscreen();
        return 0;

    case WM_APP_MPV_WAKEUP:
        m_mpv.processEvents();
        return 0;

    case WM_APP_RESOLVE_STATUS: {
        auto *p = reinterpret_cast<ResolveStatusPayload *>(lParam);
        if (p) {
            onWorkerStatus(p->requestId, p->text);
            delete p;
        }
        return 0;
    }

    case WM_APP_RESOLVE_DONE: {
        auto *p = reinterpret_cast<ResolveDonePayload *>(lParam);
        if (p) {
            onWorkerCompleted(p->requestId, p->outcome);
            delete p;
        }
        return 0;
    }

    case WM_DESTROY:
        cancelActiveWorker();
        saveState();
        m_mpv.shutdown();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

int MainWindow::runMessageLoop()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_F11) {
                toggleFullscreen();
                continue;
            }
            if (msg.wParam == VK_ESCAPE && m_fullscreen) {
                exitFullscreen();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace zethropol::tvwin7
