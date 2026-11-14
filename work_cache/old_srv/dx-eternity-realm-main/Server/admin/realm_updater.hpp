/**
 * @file realm_updater.hpp
 * @brief Semi-Automatic Linux Self-Updater for the DX:Eternity Closed Realm Server (`dx-eternity-realm`).
 *
 * Default Repository: https://github.com/mrphantastik/dx-eternity-realm (`mrphantastik/dx-eternity-realm`)
 *
 * Supports two update modes on Linux VPS:
 * 1. Git Source Pull & Build Mode (`source = "git"`):
 *    - Works natively with private GitHub repos (`https://github.com/mrphantastik/dx-eternity-realm`)
 *      authenticated via SSH deploy keys or Git credentials on the VPS.
 *    - Runs `git pull --ff-only`, builds into a staged binary, runs `--self-test`, backs up `.bak`,
 *      and atomically swaps + `execv()` restarts.
 * 2. GitHub Release Binary Download Mode (`source = "mrphantastik/dx-eternity-realm"` or `https://...`):
 *    - Downloads the prebuilt Linux static binary (`dx-eternity-realm`), supporting private repos via
 *      `GITHUB_TOKEN` environment variable or an optional GitHub Personal Access Token (`githubToken`),
 *      verifies optional BLAKE2b-256 hex digest, runs `--self-test`, backs up `.bak`, and atomically
 *      swaps + `execv()` restarts.
 */
#pragma once

#include <string>

namespace devilution::realm {

inline constexpr const char *DefaultGitHubRepo = "mrphantastik/dx-eternity-realm";

struct SelfUpdateResult {
	bool ok = false;
	std::string message;
	std::string resolvedUrl;
	std::string targetExecutablePath;
	std::string backupExecutablePath;
	std::string computedBlake2b256Hex;
};

/**
 * @brief Resolves the canonical filesystem path of the currently running server binary
 * (`/proc/self/exe` on Linux, falling back to `argv0`).
 */
std::string ResolveCurrentExecutablePath(const char *argv0);

/**
 * @brief Expands `"git"`, a GitHub `owner/repo` shorthand (defaulting to `mrphantastik/dx-eternity-realm`),
 * or validates an explicit `https://` / `file://` URL.
 * Returns an empty string if the input contains unsafe or invalid characters.
 */
std::string NormalizeUpdateSourceUrl(const std::string &repoOrUrl);

/**
 * @brief Stages and verifies a Linux server update to `<targetExePath>.new` (via `"git"` pull+build,
 * GitHub Release download with optional private-repo `githubToken` / `GITHUB_TOKEN`, or `file://`),
 * verifies optional BLAKE2b-256 hex digest (`expectedBlake2bHex`), runs `<targetExePath>.new --self-test`,
 * backs up `<targetExePath>` to `<targetExePath>.bak`, and atomically swaps `<targetExePath>.new` into `<targetExePath>`.
 */
SelfUpdateResult StageAndApplyLinuxSelfUpdate(
    const std::string &repoOrUrl,
    const std::string &targetExePath,
    const std::string &expectedBlake2bHex = "",
    const std::string &githubToken = "");

/**
 * @brief Replaces the current process image in-place via POSIX `execv(targetExePath, argv)`.
 * Should only be called after `RealmServer::Stop()`, `RealmAdminServer::Stop()`, and `RealmDatabase::Close()`.
 */
[[noreturn]] void ExecRestartSelf(const std::string &targetExePath, char *const argv[]);

} // namespace devilution::realm
