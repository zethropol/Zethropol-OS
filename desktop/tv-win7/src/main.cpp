#include "ChannelCatalog.h"
#include "HttpsClient.h"
#include "MainWindow.h"
#include "MpvController.h"
#include "StreamResolver.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace zethropol::tvwin7;

namespace {

std::string getExecutableDirUtf8()
{
    wchar_t pathBuf[MAX_PATH * 2] = {};
    const DWORD len = GetModuleFileNameW(nullptr, pathBuf, static_cast<DWORD>(sizeof(pathBuf) / sizeof(pathBuf[0])));
    if (len == 0) {
        return ".";
    }
    std::wstring full(pathBuf, len);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return ".";
    }
    return MainWindow::wideToUtf8(full.substr(0, slash));
}

bool isDirectoryWritable(const std::string &dirUtf8)
{
    const std::wstring probe = MainWindow::utf8ToWide(dirUtf8 + "\\.ztv_write_probe");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

std::string resolvePortableStatePath(const std::string &exeDirUtf8)
{
    if (isDirectoryWritable(exeDirUtf8)) {
        return exeDirUtf8 + "\\state.json";
    }
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
        const std::wstring dir = std::wstring(appData) + L"\\ZethropolTV";
        CreateDirectoryW(dir.c_str(), nullptr);
        return MainWindow::wideToUtf8(dir + L"\\state.json");
    }
    return exeDirUtf8 + "\\state.json";
}

int runSmokeTest(const std::string &exeDirUtf8,
                 const std::string &statePathUtf8,
                 const std::string &caBundlePathUtf8,
                 bool withNetwork)
{
    std::ostringstream log;
    bool allOk = true;

    log << "=== Zethropol TV Win7 32-bit Portable Smoke Test ===\n";
    log << "Arch pointer size: " << sizeof(void *) * 8 << "-bit\n";
    log << "ExeDir: " << exeDirUtf8 << "\n";

    const auto channels = ChannelCatalog::buildTurkish31Channels();
    log << "Channels loaded: " << channels.size() << "\n";
    if (channels.size() != 31) {
        allOk = false;
    }

    // 1. Portable state write/read check
    {
        PortableState st;
        st.selectedCategory = "Haber";
        st.lastChannelId = "TRTHaber.tr";
        st.volume = 80;
        st.hlsBitrateCap = true;
        const bool saved = ChannelCatalog::saveStateToFile(statePathUtf8, st);
        const PortableState loaded = ChannelCatalog::loadStateFromFile(statePathUtf8);
        const bool ok = saved && loaded.selectedCategory == "Haber"
                        && loaded.lastChannelId == "TRTHaber.tr"
                        && loaded.hlsBitrateCap;
        log << "[PortableState] " << (ok ? "PASS" : "FAIL") << " (" << statePathUtf8 << ")\n";
        if (!ok) allOk = false;
    }

    // 2. libmpv-2.dll load & security/Win7 option initialization check
    {
        MpvController mpv;
        MpvPlaybackConfig cfg;
        cfg.caBundlePathUtf8 = caBundlePathUtf8;
        cfg.mpvDllDirUtf8 = exeDirUtf8;
        cfg.headlessSmokeMode = true;
        cfg.hlsBitrateCap = true;
        std::string err;
        const bool ok = mpv.initialize(cfg, MpvCallbacks{}, &err);
        log << "[libmpv-2.dll] " << (ok ? "PASS" : "FAIL");
        if (ok) {
            log << " (api=" << std::hex << mpv.clientApiVersion() << std::dec << ")\n";
        } else {
            log << " (" << err << ")\n";
            allOk = false;
        }
        mpv.shutdown();
    }

    // 3. OpenSSL 3.x + cacert.pem TLS initialization (+ optional live Turkish broadcaster resolve)
    {
        HttpsClient https;
        std::string err;
        const bool initOk = https.initialize(caBundlePathUtf8, &err);
        log << "[OpenSSL3+CA] " << (initOk ? "PASS" : "FAIL");
        if (!initOk) {
            log << " (" << err << ")\n";
            allOk = false;
        } else {
            log << "\n";
            if (withNetwork) {
                HttpFetcher fetcher = [&](const HttpRequest &req) { return https.fetch(req); };
                const std::vector<std::string> probeIds = {"TRTHaber.tr", "CNNTurk.tr", "ATV.tr", "MinikaGO.tr"};
                for (const std::string &id : probeIds) {
                    const int idx = ChannelCatalog::findIndexById(channels, id);
                    if (idx < 0) continue;
                    const auto &ch = channels[static_cast<std::size_t>(idx)];
                    auto sess = StreamResolver::createSession(ch, false);
                    const auto out = StreamResolver::resolveNext(sess, fetcher);
                    log << "[Resolve:" << ch.name << "] " << (out.ok ? "PASS -> " + out.streamUrl : "FAIL -> " + out.errorMessage) << "\n";
                    if (!out.ok) allOk = false;
                }
            }
        }
    }

    log << "RESULT: " << (allOk ? "PASS" : "FAIL") << "\n";

    const std::string reportPath = exeDirUtf8 + "\\smoke_test_result.txt";
    std::ofstream ofs(reportPath, std::ios::binary | std::ios::trunc);
    ofs << log.str();
    ofs.close();

    // Also emit to stdout if attached under Wine console
    std::cout << log.str() << std::flush;
    return allOk ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int)
{
    const std::string exeDir = getExecutableDirUtf8();
    const std::string statePath = resolvePortableStatePath(exeDir);
    const std::string caPath = exeDir + "\\cacert.pem";

    const std::wstring cmdLine = lpCmdLine ? std::wstring(lpCmdLine) : std::wstring();
    if (cmdLine.find(L"--smoke-test") != std::wstring::npos) {
        const bool withNet = (cmdLine.find(L"--smoke-test-net") != std::wstring::npos);
        return runSmokeTest(exeDir, statePath, caPath, withNet);
    }

    MainWindow win;
    if (!win.create(hInstance, exeDir, statePath, caPath)) {
        MessageBoxW(nullptr,
                    L"Zethropol TV penceresi oluşturulamadı.",
                    L"Zethropol TV",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    return win.runMessageLoop();
}
