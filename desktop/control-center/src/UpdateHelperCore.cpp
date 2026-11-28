#include "UpdateHelperCore.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

namespace zethropol::update_helper {

namespace {

constexpr const char *kSubcommandApplyOfficial = "apply-official";
constexpr const char *kSubcommandRemoveOrphans = "remove-orphans";

const std::unordered_set<std::string> &protectedExactPackages()
{
    static const std::unordered_set<std::string> kExact = {
        "zethropol",
        "base",
        "base-devel",
        "glibc",
        "gcc-libs",
        "libgcc",
        "dbus",
        "dbus-broker",
        "dbus-broker-units",
        "polkit",
        "polkit-kde-agent",
        "pacman",
        "pacman-contrib",
        "arch-update",
        "cachy-update",
        "snapper",
        "snap-pac",
        "limine-snapper-sync",
        "btrfs-progs",
        "mkinitcpio",
        "dracut",
        "sudo",
        "sudo-rs",
        "networkmanager",
        "ufw",
        "firewalld",
        "pipewire",
        "wireplumber",
        "mesa",
        "konsole",
        "dolphin",
        "systemsettings",
        "sddm",
        "power-profiles-daemon",
        "smartmontools",
        "fwupd",
        "util-linux",
        "efibootmgr",
        "cryptsetup",
        "lvm2",
        "amd-ucode",
        "intel-ucode",
        "shim",
        "sbctl",
    };
    return kExact;
}

const std::vector<std::string> &protectedPackagePrefixes()
{
    static const std::vector<std::string> kPrefixes = {
        "zethropol-",
        "linux",
        "systemd",
        "plasma-",
        "kwin",
        "qt6-",
        "limine",
        "grub",
    };
    return kPrefixes;
}

std::string trimWhitespace(const std::string &input)
{
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])))
        ++start;
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;
    return input.substr(start, end - start);
}

std::string toLowerAscii(const std::string &input)
{
    std::string lowered;
    lowered.reserve(input.size());
    for (char ch : input)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return lowered;
}

std::vector<std::string> parseUniqueLines(const std::string &raw)
{
    std::vector<std::string> items;
    std::unordered_set<std::string> seen;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = trimWhitespace(line);
        if (!trimmed.empty() && seen.insert(trimmed).second)
            items.push_back(trimmed);
    }
    return items;
}

std::string joinCsv(const std::vector<std::string> &items)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        out += items[i];
    }
    return out;
}

void emitMarker(std::ostream &out, const std::string &payload)
{
    out << "[ZETHROPOL-UPDATE] " << payload << '\n';
    out.flush();
}

bool checkRecursiveClosureSafe(const HelperConfig &config,
                               const std::vector<std::string> &candidates,
                               std::vector<std::string> *protectedHits,
                               bool *queryOk)
{
    if (protectedHits)
        protectedHits->clear();
    if (queryOk)
        *queryOk = false;
    if (candidates.empty()) {
        if (queryOk)
            *queryOk = true;
        return true;
    }

    std::vector<std::string> args = {"-Rs", "--print", "--print-format", "%n", "--"};
    args.insert(args.end(), candidates.begin(), candidates.end());
    const CommandResult res = runSanitizedCommand(config.pacmanBin, args, nullptr);
    if (!res.started || res.exitCode != 0)
        return false;

    std::size_t validCount = 0;
    std::unordered_set<std::string> seenHits;
    std::istringstream iss(res.output);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = trimWhitespace(line);
        if (trimmed.empty())
            continue;
        if (trimmed.starts_with("warning:") || trimmed.starts_with("::"))
            continue;
        if (!isValidPackageName(trimmed))
            return false;
        ++validCount;
        if (isProtectedOrphanPackage(trimmed)) {
            if (protectedHits && seenHits.insert(trimmed).second)
                protectedHits->push_back(trimmed);
        }
    }

    if (validCount == 0)
        return false;
    if (queryOk)
        *queryOk = true;
    return !protectedHits || protectedHits->empty();
}

int runApplyOfficial(const HelperConfig &config, std::ostream &out, std::ostream &err)
{
    // 1. Mandatory fail-closed Snapper pre-snapshot before touching pacman.
    emitMarker(out, "STAGE=pre-snapshot STATUS=starting");
    const CommandResult preRes = runSanitizedCommand(
        config.snapperBin,
        {
            "-c", config.snapperConfig,
            "create",
            "-t", "pre",
            "-p",
            "--cleanup-algorithm", "number",
            "--userdata", "important=yes,zethropol=pre-update",
            "-d", "Zethropol pre-update",
        },
        nullptr);

    std::uint32_t preSnapshotId = 0;
    if (!preRes.started || preRes.exitCode != 0 || !parseSnapshotId(preRes.output, &preSnapshotId)) {
        emitMarker(out, "STAGE=pre-snapshot STATUS=failed");
        if (!preRes.output.empty())
            err << preRes.output << '\n';
        if (!preRes.error.empty())
            err << "zethropol-update-helper: " << preRes.error << '\n';
        err << "zethropol-update-helper: mandatory pre-update snapshot failed; aborting before pacman.\n";
        err.flush();
        return kExitPreSnapshotFailed;
    }
    emitMarker(out, "STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=" + std::to_string(preSnapshotId));

    // Re-verify pacman lock right before invoking pacman to narrow race window.
    if (pathExists(config.pacmanLockPath)) {
        emitMarker(out, "STAGE=blocked REASON=pacman-lock PATH=" + config.pacmanLockPath);
        err << "zethropol-update-helper: pacman lock appeared before upgrade; preserving pre-snapshot #"
            << preSnapshotId << ".\n";
        err.flush();
        return kExitPacmanLocked;
    }

    // 2. Official repository full system upgrade (never uses --nodeps, --overwrite, or --force).
    emitMarker(out, "STAGE=pacman-upgrade STATUS=starting");
    const CommandResult pacmanRes = runSanitizedCommand(
        config.pacmanBin,
        {"-Syu", "--noconfirm"},
        &out);

    const bool pacmanOk = pacmanRes.started && pacmanRes.exitCode == 0;
    if (pacmanOk) {
        emitMarker(out, "STAGE=pacman-upgrade STATUS=ok");
    } else {
        emitMarker(out,
                   "STAGE=pacman-upgrade STATUS=failed EXIT_CODE="
                       + std::to_string(pacmanRes.exitCode));
    }

    // 3. Paired Snapper post-snapshot (attempted even on pacman failure so pre/post pair stays consistent).
    emitMarker(out, "STAGE=post-snapshot STATUS=starting PRE_SNAPSHOT_ID=" + std::to_string(preSnapshotId));
    const std::string postDesc = pacmanOk
        ? "Zethropol post-update (ok)"
        : "Zethropol post-update (failed)";
    const CommandResult postRes = runSanitizedCommand(
        config.snapperBin,
        {
            "-c", config.snapperConfig,
            "create",
            "-t", "post",
            "--pre-number", std::to_string(preSnapshotId),
            "-p",
            "--cleanup-algorithm", "number",
            "--userdata", "important=yes,zethropol=post-update",
            "-d", postDesc,
        },
        nullptr);

    std::uint32_t postSnapshotId = 0;
    const bool postOk = postRes.started && postRes.exitCode == 0
        && parseSnapshotId(postRes.output, &postSnapshotId);
    if (postOk) {
        emitMarker(out,
                   "STAGE=post-snapshot STATUS=ok SNAPSHOT_ID="
                       + std::to_string(postSnapshotId)
                       + " PRE_SNAPSHOT_ID=" + std::to_string(preSnapshotId));
    } else {
        emitMarker(out,
                   "STAGE=post-snapshot STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        if (!postRes.output.empty())
            err << postRes.output << '\n';
        err.flush();
    }

    // 4. Sync Limine bootloader snapshot entries when limine-snapper-sync is installed.
    if (isExecutableFile(config.limineSyncBin)) {
        emitMarker(out, "STAGE=limine-sync STATUS=starting");
        const CommandResult limineRes = runSanitizedCommand(config.limineSyncBin, {}, &out);
        if (limineRes.started && limineRes.exitCode == 0) {
            emitMarker(out, "STAGE=limine-sync STATUS=ok");
        } else {
            emitMarker(out,
                       "STAGE=limine-sync STATUS=warning EXIT_CODE="
                           + std::to_string(limineRes.exitCode));
        }
    } else {
        emitMarker(out, "STAGE=limine-sync STATUS=skipped");
    }

    if (!pacmanOk) {
        emitMarker(out,
                   "STAGE=complete STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        return kExitPacmanFailed;
    }

    if (!postOk) {
        emitMarker(out,
                   "STAGE=complete STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        return kExitPostSnapshotFailed;
    }

    emitMarker(out,
               "STAGE=complete STATUS=ok PRE_SNAPSHOT_ID="
                   + std::to_string(preSnapshotId)
                   + " POST_SNAPSHOT_ID=" + std::to_string(postSnapshotId));
    return kExitOk;
}

int runRemoveOrphans(const HelperConfig &config, std::ostream &out, std::ostream &err)
{
    // 1. Live root-side orphan discovery (zero caller-supplied package arguments).
    emitMarker(out, "STAGE=orphan-discovery STATUS=starting");
    const CommandResult discRes = runSanitizedCommand(config.pacmanBin, {"-Qdtq"}, nullptr);
    if (!discRes.started) {
        emitMarker(out, "STAGE=orphan-discovery STATUS=failed");
        if (!discRes.error.empty())
            err << "zethropol-update-helper: " << discRes.error << '\n';
        err.flush();
        emitMarker(out, "STAGE=complete STATUS=failed");
        return kExitPacmanFailed;
    }

    const std::string trimmedDiscovery = trimWhitespace(discRes.output);
    if (discRes.exitCode == 1 && trimmedDiscovery.empty()) {
        emitMarker(out, "STAGE=orphan-discovery STATUS=empty");
        emitMarker(out, "STAGE=complete STATUS=noop");
        return kExitOk;
    }
    if (discRes.exitCode != 0) {
        emitMarker(out,
                   "STAGE=orphan-discovery STATUS=failed EXIT_CODE="
                       + std::to_string(discRes.exitCode));
        if (!discRes.output.empty())
            err << discRes.output << '\n';
        err.flush();
        emitMarker(out, "STAGE=complete STATUS=failed");
        return kExitPacmanFailed;
    }

    // 2. Validate package names and filter out zethropol-* and core OS packages.
    const std::vector<std::string> discovered = parseUniqueLines(discRes.output);
    std::vector<std::string> candidates;
    std::vector<std::string> directProtectedSkipped;
    for (const auto &pkg : discovered) {
        if (!isValidPackageName(pkg))
            continue;
        if (isProtectedOrphanPackage(pkg)) {
            directProtectedSkipped.push_back(pkg);
            continue;
        }
        candidates.push_back(pkg);
    }

    if (!directProtectedSkipped.empty()) {
        emitMarker(out,
                   "STAGE=orphan-filter STATUS=protected-skipped PACKAGES="
                       + joinCsv(directProtectedSkipped));
    }

    if (candidates.empty()) {
        emitMarker(out, "STAGE=orphan-discovery STATUS=empty");
        emitMarker(out, "STAGE=complete STATUS=noop");
        return kExitOk;
    }

    // 3. Verify recursive removal closure (pacman -Rs --print --print-format %n) never touches protected packages.
    std::vector<std::string> safeOrphans;
    std::vector<std::string> recursiveProtectedSkipped;
    std::vector<std::string> batchProtectedHits;
    bool batchQueryOk = false;
    bool anyQueryFailure = false;

    if (checkRecursiveClosureSafe(config, candidates, &batchProtectedHits, &batchQueryOk)) {
        safeOrphans = candidates;
    } else {
        if (!batchQueryOk)
            anyQueryFailure = true;
        for (const auto &cand : candidates) {
            std::vector<std::string> trial = safeOrphans;
            trial.push_back(cand);
            std::vector<std::string> trialHits;
            bool trialOk = false;
            if (checkRecursiveClosureSafe(config, trial, &trialHits, &trialOk)) {
                safeOrphans = std::move(trial);
            } else {
                if (!trialOk)
                    anyQueryFailure = true;
                else
                    recursiveProtectedSkipped.push_back(cand);
            }
        }
    }

    if (!recursiveProtectedSkipped.empty()) {
        emitMarker(out,
                   "STAGE=orphan-filter STATUS=protected-skipped REASON=recursive-dependency PACKAGES="
                       + joinCsv(recursiveProtectedSkipped));
    }

    if (safeOrphans.empty()) {
        if (anyQueryFailure && directProtectedSkipped.empty() && recursiveProtectedSkipped.empty()) {
            emitMarker(out, "STAGE=orphan-discovery STATUS=failed REASON=dry-run-failed");
            err << "zethropol-update-helper: recursive dependency dry-run failed; refusing to remove orphans.\n";
            err.flush();
            emitMarker(out, "STAGE=complete STATUS=failed");
            return kExitPacmanFailed;
        }
        emitMarker(out, "STAGE=orphan-discovery STATUS=empty");
        emitMarker(out, "STAGE=complete STATUS=noop");
        return kExitOk;
    }

    emitMarker(out,
               "STAGE=orphan-discovery STATUS=ok COUNT="
                   + std::to_string(safeOrphans.size())
                   + " PACKAGES=" + joinCsv(safeOrphans));

    // 4. Mandatory fail-closed Snapper pre-snapshot before removing any package.
    emitMarker(out, "STAGE=pre-snapshot STATUS=starting");
    const CommandResult preRes = runSanitizedCommand(
        config.snapperBin,
        {
            "-c", config.snapperConfig,
            "create",
            "-t", "pre",
            "-p",
            "--cleanup-algorithm", "number",
            "--userdata", "important=yes,zethropol=pre-orphan-cleanup",
            "-d", "Zethropol pre-orphan-cleanup",
        },
        nullptr);

    std::uint32_t preSnapshotId = 0;
    if (!preRes.started || preRes.exitCode != 0 || !parseSnapshotId(preRes.output, &preSnapshotId)) {
        emitMarker(out, "STAGE=pre-snapshot STATUS=failed");
        if (!preRes.output.empty())
            err << preRes.output << '\n';
        if (!preRes.error.empty())
            err << "zethropol-update-helper: " << preRes.error << '\n';
        err << "zethropol-update-helper: mandatory pre-orphan-cleanup snapshot failed; aborting before pacman.\n";
        err.flush();
        return kExitPreSnapshotFailed;
    }
    emitMarker(out, "STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=" + std::to_string(preSnapshotId));

    // Re-verify pacman lock right before invoking pacman to narrow race window.
    if (pathExists(config.pacmanLockPath)) {
        emitMarker(out, "STAGE=blocked REASON=pacman-lock PATH=" + config.pacmanLockPath);
        err << "zethropol-update-helper: pacman lock appeared before orphan cleanup; preserving pre-snapshot #"
            << preSnapshotId << ".\n";
        err.flush();
        return kExitPacmanLocked;
    }

    // 5. Remove verified safe orphans (never uses --nodeps, --overwrite, or --force).
    emitMarker(out, "STAGE=orphan-cleanup STATUS=starting COUNT=" + std::to_string(safeOrphans.size()));
    std::vector<std::string> removeArgs = {"-Rns", "--noconfirm", "--"};
    removeArgs.insert(removeArgs.end(), safeOrphans.begin(), safeOrphans.end());
    const CommandResult pacmanRes = runSanitizedCommand(config.pacmanBin, removeArgs, &out);

    const bool pacmanOk = pacmanRes.started && pacmanRes.exitCode == 0;
    if (pacmanOk) {
        emitMarker(out, "STAGE=orphan-cleanup STATUS=ok");
    } else {
        emitMarker(out,
                   "STAGE=orphan-cleanup STATUS=failed EXIT_CODE="
                       + std::to_string(pacmanRes.exitCode));
    }

    // 6. Paired Snapper post-snapshot (attempted even on pacman failure so pre/post pair stays consistent).
    emitMarker(out, "STAGE=post-snapshot STATUS=starting PRE_SNAPSHOT_ID=" + std::to_string(preSnapshotId));
    const std::string postDesc = pacmanOk
        ? "Zethropol post-orphan-cleanup (ok)"
        : "Zethropol post-orphan-cleanup (failed)";
    const CommandResult postRes = runSanitizedCommand(
        config.snapperBin,
        {
            "-c", config.snapperConfig,
            "create",
            "-t", "post",
            "--pre-number", std::to_string(preSnapshotId),
            "-p",
            "--cleanup-algorithm", "number",
            "--userdata", "important=yes,zethropol=post-orphan-cleanup",
            "-d", postDesc,
        },
        nullptr);

    std::uint32_t postSnapshotId = 0;
    const bool postOk = postRes.started && postRes.exitCode == 0
        && parseSnapshotId(postRes.output, &postSnapshotId);
    if (postOk) {
        emitMarker(out,
                   "STAGE=post-snapshot STATUS=ok SNAPSHOT_ID="
                       + std::to_string(postSnapshotId)
                       + " PRE_SNAPSHOT_ID=" + std::to_string(preSnapshotId));
    } else {
        emitMarker(out,
                   "STAGE=post-snapshot STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        if (!postRes.output.empty())
            err << postRes.output << '\n';
        err.flush();
    }

    // 7. Sync Limine bootloader snapshot entries when limine-snapper-sync is installed.
    if (isExecutableFile(config.limineSyncBin)) {
        emitMarker(out, "STAGE=limine-sync STATUS=starting");
        const CommandResult limineRes = runSanitizedCommand(config.limineSyncBin, {}, &out);
        if (limineRes.started && limineRes.exitCode == 0) {
            emitMarker(out, "STAGE=limine-sync STATUS=ok");
        } else {
            emitMarker(out,
                       "STAGE=limine-sync STATUS=warning EXIT_CODE="
                           + std::to_string(limineRes.exitCode));
        }
    } else {
        emitMarker(out, "STAGE=limine-sync STATUS=skipped");
    }

    if (!pacmanOk) {
        emitMarker(out,
                   "STAGE=complete STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        return kExitPacmanFailed;
    }

    if (!postOk) {
        emitMarker(out,
                   "STAGE=complete STATUS=failed PRE_SNAPSHOT_ID="
                       + std::to_string(preSnapshotId));
        return kExitPostSnapshotFailed;
    }

    emitMarker(out,
               "STAGE=complete STATUS=ok PRE_SNAPSHOT_ID="
                   + std::to_string(preSnapshotId)
                   + " POST_SNAPSHOT_ID=" + std::to_string(postSnapshotId));
    return kExitOk;
}

} // namespace

bool validateArgs(int argc, const char *const *argv, std::string *errorOut)
{
    if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
        if (errorOut)
            *errorOut = "usage: zethropol-update-helper <apply-official|remove-orphans>";
        return false;
    }
    const std::string subcommand(argv[1]);
    if (subcommand != kSubcommandApplyOfficial && subcommand != kSubcommandRemoveOrphans) {
        if (errorOut)
            *errorOut = "unsupported subcommand or unexpected flags";
        return false;
    }
    return true;
}

bool isValidPackageName(const std::string &name)
{
    if (name.empty() || name.size() > 128)
        return false;
    if (name.front() == '-' || name.front() == '.')
        return false;
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9')
            || ch == '@' || ch == '.' || ch == '_' || ch == '+' || ch == '-';
        if (!ok)
            return false;
    }
    return true;
}

bool isProtectedOrphanPackage(const std::string &name)
{
    const std::string lower = toLowerAscii(trimWhitespace(name));
    if (lower.empty())
        return true;
    if (protectedExactPackages().contains(lower))
        return true;
    for (const auto &prefix : protectedPackagePrefixes()) {
        if (lower.starts_with(prefix))
            return true;
    }
    return false;
}

bool parseSnapshotId(const std::string &raw, std::uint32_t *idOut)
{
    if (idOut)
        *idOut = 0;

    std::istringstream iss(raw);
    std::string line;
    std::string candidate;
    while (std::getline(iss, line)) {
        const std::string trimmed = trimWhitespace(line);
        if (!trimmed.empty())
            candidate = trimmed;
    }

    if (candidate.empty() || candidate.size() > 10)
        return false;

    for (char ch : candidate) {
        if (ch < '0' || ch > '9')
            return false;
    }

    std::uint64_t value = 0;
    for (char ch : candidate) {
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
        if (value > std::numeric_limits<std::uint32_t>::max())
            return false;
    }

    if (value == 0)
        return false;

    if (idOut)
        *idOut = static_cast<std::uint32_t>(value);
    return true;
}

bool pathExists(const std::string &path)
{
    if (path.empty())
        return false;
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

bool isExecutableFile(const std::string &path)
{
    if (path.empty())
        return false;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

CommandResult runSanitizedCommand(const std::string &program,
                                  const std::vector<std::string> &args,
                                  std::ostream *liveStream)
{
    CommandResult result;
    if (!isExecutableFile(program)) {
        result.error = "executable not found or not executable: " + program;
        return result;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        result.error = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return result;
    }

    if (pid == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[1]);

        std::vector<std::string> ownedArgs;
        ownedArgs.reserve(args.size() + 1);
        ownedArgs.push_back(program);
        for (const auto &arg : args)
            ownedArgs.push_back(arg);

        std::vector<char *> execArgv;
        execArgv.reserve(ownedArgs.size() + 1);
        for (auto &arg : ownedArgs)
            execArgv.push_back(arg.data());
        execArgv.push_back(nullptr);

        // Minimal, strictly sanitized environment: never pass SKIP_AUTOSNAP or SNAP_PAC_SKIP.
        std::vector<std::string> ownedEnv = {
            "PATH=/usr/bin",
            "LC_ALL=C.UTF-8",
            "TERM=dumb",
            "NO_COLOR=1",
        };
        std::vector<char *> execEnvp;
        execEnvp.reserve(ownedEnv.size() + 1);
        for (auto &kv : ownedEnv)
            execEnvp.push_back(kv.data());
        execEnvp.push_back(nullptr);

        ::execve(program.c_str(), execArgv.data(), execEnvp.data());
        _exit(127);
    }

    ::close(pipeFds[1]);
    result.started = true;

    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(pipeFds[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<std::size_t>(n));
            if (liveStream) {
                liveStream->write(buffer, n);
                liveStream->flush();
            }
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(pipeFds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            result.error = std::string("waitpid failed: ") + std::strerror(errno);
            return result;
        }
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    } else {
        result.exitCode = 1;
    }
    return result;
}

int runHelper(int argc,
              const char *const *argv,
              const HelperConfig &config,
              std::ostream &out,
              std::ostream &err)
{
    std::string argError;
    if (!validateArgs(argc, argv, &argError)) {
        err << "zethropol-update-helper: " << argError << '\n';
        err.flush();
        return kExitInvalidArgs;
    }

    if (config.requireRoot && ::geteuid() != 0) {
        err << "zethropol-update-helper: root privileges required\n";
        err.flush();
        return kExitPermissionDenied;
    }

    if (pathExists(config.pacmanLockPath)) {
        emitMarker(out, "STAGE=blocked REASON=pacman-lock PATH=" + config.pacmanLockPath);
        err << "zethropol-update-helper: active pacman lock detected at "
            << config.pacmanLockPath << "; refusing to proceed without modifying the lock.\n";
        err.flush();
        return kExitPacmanLocked;
    }

    if (!isExecutableFile(config.snapperBin)) {
        emitMarker(out, "STAGE=pre-snapshot STATUS=failed REASON=snapper-missing");
        err << "zethropol-update-helper: snapper executable missing at " << config.snapperBin << '\n';
        err.flush();
        return kExitPreSnapshotFailed;
    }

    if (!isExecutableFile(config.pacmanBin)) {
        emitMarker(out, "STAGE=preflight STATUS=failed REASON=pacman-missing");
        err << "zethropol-update-helper: pacman executable missing at " << config.pacmanBin << '\n';
        err.flush();
        return kExitPacmanFailed;
    }

    emitMarker(out, "STAGE=preflight STATUS=ok");

    const std::string subcommand(argv[1]);
    if (subcommand == kSubcommandRemoveOrphans)
        return runRemoveOrphans(config, out, err);
    return runApplyOfficial(config, out, err);
}

} // namespace zethropol::update_helper
