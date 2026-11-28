#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace zethropol::update_helper {

inline constexpr int kExitOk = 0;
inline constexpr int kExitInvalidArgs = 2;
inline constexpr int kExitPermissionDenied = 3;
inline constexpr int kExitPacmanFailed = 5;
inline constexpr int kExitPreSnapshotFailed = 11;
inline constexpr int kExitPostSnapshotFailed = 12;
inline constexpr int kExitPacmanLocked = 17;

struct HelperConfig {
    std::string pacmanLockPath = "/var/lib/pacman/db.lck";
    std::string snapperBin = "/usr/bin/snapper";
    std::string pacmanBin = "/usr/bin/pacman";
    std::string limineSyncBin = "/usr/bin/limine-snapper-sync";
    std::string snapperConfig = "root";
    bool requireRoot = true;
};

struct CommandResult {
    bool started = false;
    int exitCode = -1;
    std::string output;
    std::string error;
};

bool validateArgs(int argc, const char *const *argv, std::string *errorOut = nullptr);
bool isValidPackageName(const std::string &name);
bool isProtectedOrphanPackage(const std::string &name);
bool parseSnapshotId(const std::string &raw, std::uint32_t *idOut);
bool pathExists(const std::string &path);
bool isExecutableFile(const std::string &path);

CommandResult runSanitizedCommand(const std::string &program,
                                  const std::vector<std::string> &args,
                                  std::ostream *liveStream);

int runHelper(int argc,
              const char *const *argv,
              const HelperConfig &config,
              std::ostream &out,
              std::ostream &err);

} // namespace zethropol::update_helper
