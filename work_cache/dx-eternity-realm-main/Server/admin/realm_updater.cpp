/**
 * @file realm_updater.cpp
 * @brief Semi-Automatic Linux Self-Updater implementation for dx-eternity-realm
 *        (default repo: https://github.com/mrphantastik/dx-eternity-realm).
 */
#include "admin/realm_updater.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sodium.h>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace devilution::realm {

namespace {

bool IsSafeUrlChar(char c)
{
	const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	const bool isSafePunct = (c == '-' || c == '_' || c == '.' || c == '/' || c == ':' || c == '?' || c == '=' || c == '&' || c == '%');
	return isAlphaNum || isSafePunct;
}

bool IsSafeTokenChar(char c)
{
	const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	return isAlphaNum || c == '_' || c == '-';
}

bool IsSimpleOwnerRepo(const std::string &s)
{
	if (s.size() < 3 || s.size() > 120)
		return false;
	const size_t slashPos = s.find('/');
	if (slashPos == std::string::npos || slashPos == 0 || slashPos + 1 >= s.size())
		return false;
	if (s.find('/', slashPos + 1) != std::string::npos || s.find("..") != std::string::npos)
		return false;
	for (char c : s) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		    || c == '-' || c == '_' || c == '.' || c == '/';
		if (!ok)
			return false;
	}
	return true;
}

bool CopyFileContents(const std::string &srcPath, const std::string &dstPath)
{
	std::ifstream in(srcPath, std::ios::binary);
	if (!in.is_open())
		return false;
	std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
		return false;
	out << in.rdbuf();
	out.flush();
	return in.good() || in.eof();
}

bool ComputeFileBlake2b256Hex(const std::string &filePath, std::string &outHex)
{
	outHex.clear();
	std::ifstream in(filePath, std::ios::binary);
	if (!in.is_open())
		return false;

	crypto_generichash_state state {};
	crypto_generichash_init(&state, nullptr, 0, 32);

	char buf[16384];
	while (in.good()) {
		in.read(buf, sizeof(buf));
		const std::streamsize n = in.gcount();
		if (n > 0) {
			crypto_generichash_update(&state, reinterpret_cast<const unsigned char *>(buf), static_cast<unsigned long long>(n));
		}
	}

	uint8_t digest[32] {};
	crypto_generichash_final(&state, digest, sizeof(digest));

	char hexBuf[65] {};
	sodium_bin2hex(hexBuf, sizeof(hexBuf), digest, sizeof(digest));
	outHex = hexBuf;
	return true;
}

bool EqualsHexCaseInsensitive(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
		const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
		if (ca != cb)
			return false;
	}
	return true;
}

#ifndef _WIN32
int RunProcessNoShell(const char *file, char *const argv[], bool setChildSelfTestEnv, const std::string &stdinPayload = "")
{
	int pipeFds[2] = { -1, -1 };
	if (!stdinPayload.empty()) {
		if (pipe(pipeFds) != 0)
			return -1;
	}

	const pid_t pid = fork();
	if (pid < 0) {
		if (pipeFds[0] >= 0) {
			close(pipeFds[0]);
			close(pipeFds[1]);
		}
		return -1;
	}

	if (pid == 0) {
		if (setChildSelfTestEnv) {
			setenv("DXER_UPDATER_CHILD_SELFTEST", "1", 1);
		}
		if (pipeFds[0] >= 0) {
			dup2(pipeFds[0], STDIN_FILENO);
			close(pipeFds[0]);
			close(pipeFds[1]);
		}
		const int devNull = open("/dev/null", O_WRONLY);
		if (devNull >= 0) {
			dup2(devNull, STDOUT_FILENO);
			dup2(devNull, STDERR_FILENO);
			close(devNull);
		}
		execvp(file, argv);
		_exit(127);
	}

	if (pipeFds[1] >= 0) {
		close(pipeFds[0]);
		const ssize_t written = write(pipeFds[1], stdinPayload.data(), stdinPayload.size());
		(void)written;
		close(pipeFds[1]);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}
#endif

} // namespace

std::string ResolveCurrentExecutablePath(const char *argv0)
{
#ifndef _WIN32
	char buf[4096] {};
	const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		return std::string(buf);
	}
#endif
	return (argv0 != nullptr && argv0[0] != '\0') ? std::string(argv0) : std::string("misc/dx-eternity-realm");
}

std::string NormalizeUpdateSourceUrl(const std::string &repoOrUrl)
{
	const std::string input = repoOrUrl.empty() ? std::string(DefaultGitHubRepo) : repoOrUrl;
	if (input == "git")
		return "git";

	if (input.size() > 512)
		return {};

	for (char c : input) {
		if (!IsSafeUrlChar(c))
			return {};
	}

	// Expand bare https://github.com/mrphantastik/dx-eternity-realm repo URL to latest release asset
	const std::string ghPrefix = "https://github.com/";
	if (input.rfind(ghPrefix, 0) == 0) {
		const std::string tail = input.substr(ghPrefix.size());
		if (IsSimpleOwnerRepo(tail)) {
			return ghPrefix + tail + "/releases/latest/download/dx-eternity-realm";
		}
		return input;
	}

	if (input.rfind("https://", 0) == 0 || input.rfind("file://", 0) == 0) {
		return input;
	}

	if (IsSimpleOwnerRepo(input)) {
		return "https://github.com/" + input + "/releases/latest/download/dx-eternity-realm";
	}

	return {};
}

SelfUpdateResult StageAndApplyLinuxSelfUpdate(
    const std::string &repoOrUrl,
    const std::string &targetExePath,
    const std::string &expectedBlake2bHex,
    const std::string &githubToken)
{
	SelfUpdateResult result {};
	result.targetExecutablePath = targetExePath;
	result.backupExecutablePath = targetExePath + ".bak";
	result.resolvedUrl = NormalizeUpdateSourceUrl(repoOrUrl);

	if (result.resolvedUrl.empty()) {
		result.message = "Invalid or unsafe update URL / GitHub owner/repo string";
		return result;
	}
	if (targetExePath.empty()) {
		result.message = "Target executable path is empty";
		return result;
	}

#ifdef _WIN32
	(void)expectedBlake2bHex;
	(void)githubToken;
	result.message = "Linux POSIX atomic self-updater is only supported on Linux builds";
	return result;
#else
	const std::string stagedNewPath = targetExePath + ".new";
	unlink(stagedNewPath.c_str());

	// 1. Stage candidate binary (`file://`, `"git"` pull+build, or `https://` GitHub Release download)
	if (result.resolvedUrl.rfind("file://", 0) == 0) {
		const std::string localSrc = result.resolvedUrl.substr(7);
		if (!CopyFileContents(localSrc, stagedNewPath)) {
			result.message = "Failed to stage local update binary from " + localSrc;
			return result;
		}
	} else if (result.resolvedUrl == "git") {
		// Private repo git pull + cmake build mode
		char *gitPullArgv[] = {
			const_cast<char *>("git"),
			const_cast<char *>("pull"),
			const_cast<char *>("--ff-only"),
			nullptr
		};
		const int pullCode = RunProcessNoShell("git", gitPullArgv, false);
		if (pullCode != 0) {
			result.message = "git pull --ff-only failed (exit=" + std::to_string(pullCode) + ")";
			return result;
		}
		std::string stagedBuildDir = ".deps/build-linux-update";
		char *cmakeCfgArgv[] = {
			const_cast<char *>("cmake"),
			const_cast<char *>("-B"),
			stagedBuildDir.data(),
			const_cast<char *>("-G"),
			const_cast<char *>("Ninja"),
			const_cast<char *>("-DCMAKE_BUILD_TYPE=Release"),
			std::string("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + stagedBuildDir).data(),
			nullptr
		};
		if (RunProcessNoShell("cmake", cmakeCfgArgv, false) != 0) {
			result.message = "cmake configure failed during git self-update";
			return result;
		}
		char *cmakeBuildArgv[] = {
			const_cast<char *>("cmake"),
			const_cast<char *>("--build"),
			stagedBuildDir.data(),
			const_cast<char *>("-j4"),
			nullptr
		};
		if (RunProcessNoShell("cmake", cmakeBuildArgv, false) != 0
		    || !CopyFileContents(stagedBuildDir + "/dx-eternity-realm", stagedNewPath)) {
			result.message = "cmake build failed during git self-update";
			return result;
		}
	} else {
		// Resolve optional GitHub token for private repo https://github.com/mrphantastik/dx-eternity-realm
		std::string effectiveToken = githubToken;
		if (effectiveToken.empty()) {
			if (const char *envTok = std::getenv("GITHUB_TOKEN")) {
				effectiveToken = envTok;
			}
		}
		for (char c : effectiveToken) {
			if (!IsSafeTokenChar(c)) {
				effectiveToken.clear();
				break;
			}
		}

		std::string urlArg = result.resolvedUrl;
		std::string outArg = stagedNewPath;
		int dlCode = -1;

		if (!effectiveToken.empty()) {
			// Pass Authorization header via stdin (--config -) so the private GitHub token is never exposed in `ps aux`
			const std::string curlConfig = "header = \"Authorization: Bearer " + effectiveToken + "\"\n"
			                               "header = \"Accept: application/octet-stream\"\n";
			char *curlAuthArgv[] = {
				const_cast<char *>("curl"),
				const_cast<char *>("-fsSL"),
				const_cast<char *>("--max-time"),
				const_cast<char *>("60"),
				const_cast<char *>("--config"),
				const_cast<char *>("-"),
				const_cast<char *>("-o"),
				outArg.data(),
				urlArg.data(),
				nullptr
			};
			dlCode = RunProcessNoShell("curl", curlAuthArgv, false, curlConfig);
		} else {
			char *curlArgv[] = {
				const_cast<char *>("curl"),
				const_cast<char *>("-fsSL"),
				const_cast<char *>("--max-time"),
				const_cast<char *>("60"),
				const_cast<char *>("-o"),
				outArg.data(),
				urlArg.data(),
				nullptr
			};
			dlCode = RunProcessNoShell("curl", curlArgv, false);
		}

		if (dlCode != 0) {
			unlink(stagedNewPath.c_str());
			result.message = "Failed to download release binary from " + result.resolvedUrl
			    + " (exit=" + std::to_string(dlCode) + "; for private repo mrphantastik/dx-eternity-realm, set GITHUB_TOKEN or use mode 'git')";
			return result;
		}
	}

	// 2. Compute and optionally verify BLAKE2b-256 digest of the staged binary
	if (!ComputeFileBlake2b256Hex(stagedNewPath, result.computedBlake2b256Hex)) {
		unlink(stagedNewPath.c_str());
		result.message = "Failed to read staged binary for BLAKE2b-256 verification";
		return result;
	}
	if (!expectedBlake2bHex.empty() && !EqualsHexCaseInsensitive(result.computedBlake2b256Hex, expectedBlake2bHex)) {
		unlink(stagedNewPath.c_str());
		result.message = "BLAKE2b-256 checksum mismatch (expected " + expectedBlake2bHex
		    + ", got " + result.computedBlake2b256Hex + ")";
		return result;
	}

	// 3. Mark staged binary executable and run pre-flight `--self-test`
	if (chmod(stagedNewPath.c_str(), 0755) != 0) {
		unlink(stagedNewPath.c_str());
		result.message = "chmod(0755) failed on staged binary";
		return result;
	}

	std::string selfTestExe = stagedNewPath;
	char *selfTestArgv[] = {
		selfTestExe.data(),
		const_cast<char *>("--self-test"),
		nullptr
	};
	const int selfTestCode = RunProcessNoShell(stagedNewPath.c_str(), selfTestArgv, true);
	if (selfTestCode != 0) {
		unlink(stagedNewPath.c_str());
		result.message = "Staged binary failed pre-flight --self-test (exit=" + std::to_string(selfTestCode) + "); update aborted safely";
		return result;
	}

	// 4. Backup existing binary to `<targetExePath>.bak` and atomically swap `<targetExePath>.new` -> `<targetExePath>`
	CopyFileContents(targetExePath, result.backupExecutablePath);
	chmod(result.backupExecutablePath.c_str(), 0755);

	if (std::rename(stagedNewPath.c_str(), targetExePath.c_str()) != 0) {
		unlink(stagedNewPath.c_str());
		result.message = "Atomic rename() of verified binary over target path failed";
		return result;
	}

	result.ok = true;
	result.message = "Verified update (--self-test passed, BLAKE2b=" + result.computedBlake2b256Hex.substr(0, 16)
	    + "...) and atomically installed to " + targetExePath;
	return result;
#endif
}

[[noreturn]] void ExecRestartSelf(const std::string &targetExePath, char *const argv[])
{
#ifndef _WIN32
	execv(targetExePath.c_str(), argv);
#else
	(void)targetExePath;
	(void)argv;
#endif
	std::exit(0);
}

} // namespace devilution::realm
