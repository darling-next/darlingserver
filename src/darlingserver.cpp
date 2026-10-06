/**
 * This file is part of Darling.
 *
 * Copyright (C) 2021 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <execinfo.h>
#include <exception>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <iostream>
#include <utility>
#include <linux/sched.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/signal.h>
#include <climits>

#include <darling-config.h>

#include <darlingserver/server.hpp>
#include <darlingserver/config.hpp>
#include <darlingserver/runtime-mode.hpp>

#ifndef DARLINGSERVER_INIT_PROCESS
	#define DARLINGSERVER_INIT_PROCESS "/sbin/launchd"
#endif

#ifndef DARLINGSERVER_XDG_USER_DIR_CMD
	#define DARLINGSERVER_XDG_USER_DIR_CMD "xdg-user-dir"
#endif

#if DSERVER_ASAN
	#include <sanitizer/lsan_interface.h>
#endif

// TODO: most of the code here was ported over from startup/darling.c; we should C++-ify it.

static int openDirectoryAt(int parentFD, const char* name, bool missingIsOkay = false) {
	struct stat st;
	if (fstatat(parentFD, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (missingIsOkay && errno == ENOENT) {
			return -1;
		}
		fprintf(stderr, "Cannot inspect prefix directory %s: %s\n",
			name, strerror(errno));
		exit(1);
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Prefix entry is not a trusted directory: %s\n", name);
		exit(1);
	}
	const int fd = openat(
		parentFD, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd == -1) {
		fprintf(stderr, "Cannot open prefix directory %s: %s\n",
			name, strerror(errno));
		exit(1);
	}
	struct stat opened;
	if (fstat(fd, &opened) == -1 ||
		opened.st_dev != st.st_dev ||
		opened.st_ino != st.st_ino) {
		fprintf(stderr, "Prefix directory changed while opening: %s\n", name);
		close(fd);
		exit(1);
	}
	return fd;
}

static unsigned long directoryTemporaryCounter = 0;

static int createDirectoryAt(int parentFD, const char* name, mode_t mode) {
	const std::string temporary =
		".darling-dir-" + std::to_string(getpid()) + "-" +
		std::to_string(++directoryTemporaryCounter);
	if (temporary.size() > NAME_MAX) {
		fprintf(stderr, "Prefix directory temporary name is too long\n");
		exit(1);
	}
	if (mkdirat(parentFD, temporary.c_str(), mode) == -1) {
		fprintf(stderr, "Cannot create prefix directory temporary: %s\n",
			strerror(errno));
		exit(1);
	}
	const int fd = openat(
		parentFD, temporary.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	struct stat opened;
	if (fd == -1 || fstat(fd, &opened) == -1 ||
		!S_ISDIR(opened.st_mode)) {
		const int saved = errno == 0 ? EAGAIN : errno;
		if (fd >= 0) {
			close(fd);
		}
		unlinkat(parentFD, temporary.c_str(), AT_REMOVEDIR);
		errno = saved;
		fprintf(stderr, "Cannot retain prefix directory temporary: %s\n",
			strerror(errno));
		exit(1);
	}
	if (renameat2(
			parentFD, temporary.c_str(), parentFD, name,
			RENAME_NOREPLACE) == -1) {
		const int saved = errno;
		close(fd);
		unlinkat(parentFD, temporary.c_str(), AT_REMOVEDIR);
		errno = saved;
		fprintf(stderr, "Cannot publish prefix directory %s safely: %s\n",
			name, strerror(errno));
		exit(1);
	}
	struct stat named;
	if (fstatat(parentFD, name, &named, AT_SYMLINK_NOFOLLOW) == -1 ||
		S_ISLNK(named.st_mode) ||
		!S_ISDIR(named.st_mode) ||
		named.st_dev != opened.st_dev ||
		named.st_ino != opened.st_ino) {
		close(fd);
		fprintf(stderr, "Prefix directory changed during creation: %s\n",
			name);
		exit(1);
	}
	return fd;
}

static int ensureDirectoryAt(int parentFD, const char* name, mode_t mode) {
	struct stat st;
	if (fstatat(parentFD, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (errno != ENOENT) {
			fprintf(stderr, "Cannot create prefix directory %s: %s\n",
				name, strerror(errno));
			exit(1);
		}
		return createDirectoryAt(parentFD, name, mode);
	} else if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Prefix entry is not a directory: %s\n", name);
		exit(1);
	}
	return openDirectoryAt(parentFD, name);
}

static void unlinkLeafAt(int parentFD, const char* name) {
	struct stat st;
	if (fstatat(parentFD, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (errno == ENOENT) {
			return;
		}
		fprintf(stderr, "Cannot inspect prefix entry %s: %s\n",
			name, strerror(errno));
		exit(1);
	}
	if (S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Refusing to unlink prefix directory as a leaf: %s\n",
			name);
		exit(1);
	}
	if (unlinkat(parentFD, name, 0) == -1) {
		fprintf(stderr, "Cannot unlink prefix entry %s: %s\n",
			name, strerror(errno));
		exit(1);
	}
}

static void fixPermissionsRecursiveFD(
	int directoryFD,
	uid_t originalUID,
	gid_t originalGID
) {
	if (fchown(directoryFD, originalUID, originalGID) == -1) {
		fprintf(stderr, "Cannot chown prefix directory: %s\n", strerror(errno));
	}
	const int scanFD = dup(directoryFD);
	if (scanFD == -1) {
		fprintf(stderr, "Cannot duplicate prefix directory: %s\n",
			strerror(errno));
		exit(1);
	}
	DIR* directory = fdopendir(scanFD);
	if (directory == nullptr) {
		close(scanFD);
		fprintf(stderr, "Cannot scan prefix directory: %s\n", strerror(errno));
		exit(1);
	}
	errno = 0;
	while (struct dirent* entry = readdir(directory)) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		struct stat st;
		if (fstatat(
			directoryFD, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
			fprintf(stderr, "Cannot inspect prefix child %s: %s\n",
				entry->d_name, strerror(errno));
			closedir(directory);
			exit(1);
		}
		if (!S_ISDIR(st.st_mode)) {
			continue;
		}
		const int childFD = openDirectoryAt(directoryFD, entry->d_name);
		fixPermissionsRecursiveFD(childFD, originalUID, originalGID);
		close(childFD);
		errno = 0;
	}
	if (errno != 0) {
		fprintf(stderr, "Cannot scan prefix directory: %s\n", strerror(errno));
		closedir(directory);
		exit(1);
	}
	closedir(directory);
}

static void chownRelativeAt(
	int rootFD,
	const char* relative,
	uid_t uid,
	gid_t gid
) {
	char copy[4096];
	if (relative == nullptr || relative[0] == '/' ||
		strlen(relative) >= sizeof(copy)) {
		fprintf(stderr, "Invalid relative prefix ownership path\n");
		exit(1);
	}
	strcpy(copy, relative);
	char* slash = strrchr(copy, '/');
	const char* leaf = copy;
	int parentFD = dup(rootFD);
	if (parentFD == -1) {
		fprintf(stderr, "Cannot duplicate prefix descriptor: %s\n",
			strerror(errno));
		exit(1);
	}
	if (slash != nullptr) {
		*slash = '\0';
		leaf = slash + 1;
		char* save = nullptr;
		for (char* component = strtok_r(copy, "/", &save);
			component != nullptr;
			component = strtok_r(nullptr, "/", &save)) {
			const int next = openDirectoryAt(parentFD, component, true);
			close(parentFD);
			parentFD = next;
			if (parentFD == -1) {
				return;
			}
		}
	}
	struct stat status;
	if (fstatat(parentFD, leaf, &status, AT_SYMLINK_NOFOLLOW) == -1) {
		if (errno == ENOENT) {
			close(parentFD);
			return;
		}
		fprintf(stderr, "Cannot inspect prefix ownership path %s: %s\n",
			relative, strerror(errno));
		close(parentFD);
		exit(1);
	}
	if (S_ISLNK(status.st_mode)) {
		fprintf(stderr, "Refusing to chown prefix symlink %s\n", relative);
		close(parentFD);
		exit(1);
	}
	if (fchownat(parentFD, leaf, uid, gid, AT_SYMLINK_NOFOLLOW) == -1) {
		fprintf(stderr, "Cannot chown prefix entry %s: %s\n",
			relative, strerror(errno));
		close(parentFD);
		exit(1);
	}
	close(parentFD);
}

const char* xdgDirectory(const char* name)
{
	static char dir[4096];
	char* cmd = (char*) malloc(sizeof(DARLINGSERVER_XDG_USER_DIR_CMD) + 1 + strlen(name));

	sprintf(cmd, DARLINGSERVER_XDG_USER_DIR_CMD " %s", name);

	FILE* proc = popen(cmd, "r");

	free(cmd);

	if (!proc)
		return NULL;

	fgets(dir, sizeof(dir)-1, proc);

	pclose(proc);

	size_t len = strlen(dir);
	if (len <= 1)
		return NULL;

	if (dir[len-1] == '\n')
		dir[len-1] = '\0';
	return dir;
}

void setupUserHome(int prefixFD, uid_t originalUID)
{
	char buf[4096];

	struct stat usersStat;
	errno = 0;
	const int usersStatus =
		fstatat(prefixFD, "Users", &usersStat, AT_SYMLINK_NOFOLLOW);
	if (usersStatus == 0 && !S_ISDIR(usersStat.st_mode)) {
		if (S_ISLNK(usersStat.st_mode)) {
			unlinkLeafAt(prefixFD, "Users");
		} else {
			fprintf(stderr, "Refusing non-directory prefix Users entry\n");
			exit(1);
		}
	} else if (usersStatus == -1 && errno != ENOENT) {
		fprintf(stderr, "Cannot inspect prefix Users directory: %s\n",
			strerror(errno));
		exit(1);
	}
	const int usersFD = ensureDirectoryAt(prefixFD, "Users", 0777);
	const int sharedFD = ensureDirectoryAt(usersFD, "Shared", 0777);
	close(sharedFD);

	const char* home = getenv("HOME");

	const char* login = NULL;
	struct passwd* pw = getpwuid(originalUID);

	if (pw != NULL)
		login = pw->pw_name;

	if (!login)
		login = getlogin();

	if (!login)
	{
		fprintf(stderr, "Cannot determine your user name\n");
		exit(1);
	}
	if (!home)
	{
		fprintf(stderr, "Cannot determine your home directory\n");
		exit(1);
	}

	const int userFD = ensureDirectoryAt(usersFD, login, 0755);
	close(usersFD);

	snprintf(buf, sizeof(buf), "/Volumes/SystemRoot%s", home);
	unlinkLeafAt(userFD, "LinuxHome");
	if (symlinkat(buf, userFD, "LinuxHome") == -1) {
		fprintf(stderr, "Cannot create LinuxHome link: %s\n", strerror(errno));
		exit(1);
	}

	static const char* xdgmap[][2] = {
		{ "DESKTOP", "Desktop" },
		{ "DOWNLOAD", "Downloads" },
		{ "PUBLICSHARE", "Public" },
		{ "DOCUMENTS", "Documents" },
		{ "MUSIC", "Music" },
		{ "PICTURES", "Pictures" },
		{ "VIDEOS", "Movies" },
	};

	for (int i = 0; i < sizeof(xdgmap) / sizeof(xdgmap[0]); i++)
	{
		const char* dir = xdgDirectory(xdgmap[i][0]);
		if (!dir)
			continue;

		snprintf(buf, sizeof(buf), "/Volumes/SystemRoot%s", dir);
		unlinkLeafAt(userFD, xdgmap[i][1]);
		if (symlinkat(buf, userFD, xdgmap[i][1]) == -1) {
			fprintf(stderr, "Cannot create XDG directory link %s: %s\n",
				xdgmap[i][1], strerror(errno));
			exit(1);
		}
	}
	close(userFD);
}

void setupCoredumpPattern(void)
{
	FILE* f = fopen("/proc/sys/kernel/core_pattern", "w");
	if (f != NULL)
	{
		// This is how macOS saves core dumps
		fputs("/cores/core.%p\n", f);
		fclose(f);
	}
}

static void wipeDirFD(int directoryFD)
{
	const int scanFD = dup(directoryFD);
	if (scanFD == -1) {
		fprintf(stderr, "Cannot duplicate prefix directory: %s\n",
			strerror(errno));
		exit(1);
	}
	DIR* dir = fdopendir(scanFD);
	if (dir == nullptr) {
		close(scanFD);
		fprintf(stderr, "Cannot scan prefix directory: %s\n", strerror(errno));
		exit(1);
	}
	errno = 0;
	while (struct dirent* ent = readdir(dir)) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
			continue;
		}
		struct stat st;
		if (fstatat(
			directoryFD, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
			fprintf(stderr, "Cannot inspect prefix state entry %s: %s\n",
				ent->d_name, strerror(errno));
			closedir(dir);
			exit(1);
		}
		if (S_ISDIR(st.st_mode)) {
			const int childFD = openDirectoryAt(directoryFD, ent->d_name);
			wipeDirFD(childFD);
			close(childFD);
			if (unlinkat(directoryFD, ent->d_name, AT_REMOVEDIR) == -1) {
				fprintf(stderr, "Cannot remove prefix state directory %s: %s\n",
					ent->d_name, strerror(errno));
				closedir(dir);
				exit(1);
			}
		} else if (unlinkat(directoryFD, ent->d_name, 0) == -1) {
			fprintf(stderr, "Cannot remove prefix state entry %s: %s\n",
				ent->d_name, strerror(errno));
			closedir(dir);
			exit(1);
		}
		errno = 0;
	}
	if (errno != 0) {
		fprintf(stderr, "Cannot scan prefix state directory: %s\n",
			strerror(errno));
		closedir(dir);
		exit(1);
	}
	closedir(dir);
}

void darlingPreInit(int prefixFD)
{
	// TODO: Run /usr/libexec/makewhatis
	const char* dirs[] = {
		"tmp",
		"run"
	};
	const int varFD = openDirectoryAt(prefixFD, "var", true);
	if (varFD == -1) {
		return;
	}
	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++)
	{
		const int childFD = openDirectoryAt(varFD, dirs[i], true);
		if (childFD != -1) {
			wipeDirFD(childFD);
			close(childFD);
		}
	}
	close(varFD);
}

void spawnLaunchd(
	const char* prefix,
	DarlingServer::RuntimeMode runtimeMode,
	int prefixFD
)
{
	puts("Bootstrapping the container with launchd...");

	// putenv("KQUEUE_DEBUG=1");

	char socketAnchor[64];
	const char* socketPrefix = prefix;
	if (DarlingServer::runtimeModeIsRootless(runtimeMode)) {
		// The parent server owns this anchor; guest exec must not need to retain it.
		snprintf(socketAnchor, sizeof(socketAnchor), "/proc/%d/fd/%d",
			getppid(), prefixFD);
		socketPrefix = socketAnchor;
	}
	auto tmp = (std::string(socketPrefix) + "/.darlingserver.sock");

	const char* initPath = getenv("DSERVER_INIT");

	if (!initPath) {
		initPath = DARLINGSERVER_INIT_PROCESS;
	}

	setenv("__mldr_DYLD_ROOT_PATH", LIBEXEC_PATH, 1);
	setenv("__mldr_sockpath", tmp.c_str(), 1);
	const std::string runtimeModeName(
		DarlingServer::runtimeModeName(runtimeMode));
	setenv("__mldr_runtime_mode", runtimeModeName.c_str(), 1);
	unsetenv("DARLING_RUNTIME_MODE");
	unsetenv("DARLING_ROOTLESS");
	unsetenv("DARLING_NOOVERLAYFS");
	unsetenv("DARLING_EUNION");
	const int prefixFlags = fcntl(prefixFD, F_GETFD);
	if (prefixFlags == -1 ||
		fcntl(prefixFD, F_SETFD, prefixFlags & ~FD_CLOEXEC) == -1) {
		fprintf(stderr, "Failed to retain trusted prefix descriptor for launchd: %s\n",
			strerror(errno));
		abort();
	}
	execl(DarlingServer::Config::defaultMldrPath.data(), "mldr!" LIBEXEC_PATH "/usr/libexec/darling/vchroot", "vchroot", prefix, initPath, NULL);

	fprintf(stderr, "Failed to exec launchd: %s\n", strerror(errno));
	abort();
}

static bool testEnvVar(const char* var_name) {
	const char* var = getenv(var_name);

	if (!var) {
		return false;
	}

	auto len = strlen(var);

	if (len > 0 && (var[0] == '1' || var[0] == 't' || var[0] == 'T')) {
		return true;
	}

	return false;
};

static bool isOnWsl1() {
	static bool initialized = false;
	static bool result = false;
	if (!initialized) {
		initialized = true;
		// All WSL systems have WSLENV set by default, while WSL_INTEROP is WSL2-specific.
		result = getenv("WSLENV") && !getenv("WSL_INTEROP");
	}

	return result;
}

static void setupEunionPrefix(int prefixFD) {
	const int markerFD = ensureDirectoryAt(prefixFD, ".union-work", 0700);
	close(markerFD);
}

static int compareTimespec(const timespec& a, const timespec& b) {
	if (a.tv_sec != b.tv_sec) {
		return (a.tv_sec > b.tv_sec) ? 1 : -1;
	} else if (a.tv_nsec != b.tv_nsec) {
		return (a.tv_nsec > b.tv_nsec) ? 1 : -1;
	} else {
		return 0;
	}
}

static unsigned long copyTemporaryCounter = 0;

static std::string copyTemporaryName() {
	return ".darling-copy-" + std::to_string(getpid()) + "-" +
		std::to_string(++copyTemporaryCounter);
}

static void applyDescriptorAttributes(
	int fd,
	const struct stat& source,
	bool preserveOwnership,
	const char* description
) {
	const struct timespec times[] = { source.st_atim, source.st_mtim };
	if (futimens(fd, times) == -1 ||
		(preserveOwnership &&
		 fchown(fd, source.st_uid, source.st_gid) == -1) ||
		fchmod(fd, source.st_mode & ALLPERMS) == -1) {
		fprintf(stderr, "Failed to set destination attributes %s: %s\n",
			description, strerror(errno));
		abort();
	}
}

static void applySymlinkAttributesAt(
	int parentFD,
	const char* name,
	const struct stat& source,
	bool preserveOwnership
) {
	if (!S_ISLNK(source.st_mode)) {
		fprintf(stderr, "Internal error: non-symlink attributes used for %s\n",
			name);
		abort();
	}
	const struct timespec times[] = { source.st_atim, source.st_mtim };
	if (utimensat(parentFD, name, times, AT_SYMLINK_NOFOLLOW) == -1) {
		fprintf(stderr, "Failed to set destination timestamp %s: %s\n",
			name, strerror(errno));
		abort();
	}
	if (preserveOwnership &&
		fchownat(
			parentFD, name, source.st_uid, source.st_gid,
			AT_SYMLINK_NOFOLLOW) == -1) {
		fprintf(stderr, "Failed to set destination owner %s: %s\n",
			name, strerror(errno));
		abort();
	}
}

static void replaceRegularFileAt(
	const std::string& sourcePath,
	int parentFD,
	const char* name,
	const struct stat& source,
	bool preserveOwnership
) {
	const int input = open(
		sourcePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (input == -1) {
		fprintf(stderr, "Failed to open source file %s: %s\n",
			sourcePath.c_str(), strerror(errno));
		abort();
	}
	const std::string temporary = copyTemporaryName();
	const int output = openat(
		parentFD, temporary.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		source.st_mode & ALLPERMS);
	if (output == -1) {
		fprintf(stderr, "Failed to create destination temporary file %s: %s\n",
			name, strerror(errno));
		close(input);
		abort();
	}
	const struct timespec times[] = { source.st_atim, source.st_mtim };
	char buffer[128 * 1024];
	for (;;) {
		const ssize_t count = read(input, buffer, sizeof(buffer));
		if (count == 0) {
			break;
		}
		if (count < 0) {
			fprintf(stderr, "Failed to read source file %s: %s\n",
				sourcePath.c_str(), strerror(errno));
			goto copy_failure;
		}
		ssize_t written = 0;
		while (written < count) {
			const ssize_t result =
				write(output, buffer + written, count - written);
			if (result <= 0) {
				fprintf(stderr, "Failed to write destination file %s: %s\n",
					name, strerror(errno));
				goto copy_failure;
			}
			written += result;
		}
	}
	if ((preserveOwnership &&
		 fchown(output, source.st_uid, source.st_gid) == -1) ||
		fchmod(output, source.st_mode & ALLPERMS) == -1 ||
		futimens(output, times) == -1 ||
		fsync(output) == -1) {
		fprintf(stderr, "Failed to finalize destination file %s: %s\n",
			name, strerror(errno));
		goto copy_failure;
	}
	if (close(input) == -1 || close(output) == -1) {
		fprintf(stderr, "Failed to close copied file %s: %s\n",
			name, strerror(errno));
		unlinkat(parentFD, temporary.c_str(), 0);
		abort();
	}
	if (renameat(parentFD, temporary.c_str(), parentFD, name) == -1) {
		fprintf(stderr, "Failed to publish destination file %s: %s\n",
			name, strerror(errno));
		unlinkat(parentFD, temporary.c_str(), 0);
		abort();
	}
	return;

copy_failure:
	close(input);
	close(output);
	unlinkat(parentFD, temporary.c_str(), 0);
	abort();
}

static void replaceSymlinkAt(
	const std::string& sourcePath,
	int parentFD,
	const char* name,
	const struct stat& source,
	bool preserveOwnership
) {
	std::string target;
	target.resize(static_cast<size_t>(source.st_size) + 1);
	const ssize_t length =
		readlink(sourcePath.c_str(), target.data(), target.size());
	if (length < 0 || static_cast<size_t>(length) >= target.size()) {
		fprintf(stderr, "Failed to read source symlink %s: %s\n",
			sourcePath.c_str(), strerror(errno));
		abort();
	}
	target.resize(static_cast<size_t>(length));
	const std::string temporary = copyTemporaryName();
	if (symlinkat(target.c_str(), parentFD, temporary.c_str()) == -1) {
		fprintf(stderr, "Failed to create destination symlink %s: %s\n",
			name, strerror(errno));
		abort();
	}
	applySymlinkAttributesAt(
		parentFD, temporary.c_str(), source, preserveOwnership);
	if (renameat(parentFD, temporary.c_str(), parentFD, name) == -1) {
		fprintf(stderr, "Failed to publish destination symlink %s: %s\n",
			name, strerror(errno));
		unlinkat(parentFD, temporary.c_str(), 0);
		abort();
	}
}

static void copyAndSetAttributesAt(
	std::string& sourcePath,
	int destinationParentFD,
	const char* destinationName,
	bool preserveOwnership
) {
	struct stat source;
	if (lstat(sourcePath.c_str(), &source) == -1) {
		fprintf(stderr, "Failed to stat source %s: %s\n",
			sourcePath.c_str(), strerror(errno));
		abort();
	}
	struct stat destination;
	errno = 0;
	bool destinationExists =
		fstatat(
			destinationParentFD, destinationName, &destination,
			AT_SYMLINK_NOFOLLOW) == 0;
	if (!destinationExists && errno != ENOENT) {
		fprintf(stderr, "Failed to stat destination %s: %s\n",
			destinationName, strerror(errno));
		abort();
	}

	if (S_ISDIR(source.st_mode)) {
		if (destinationExists && !S_ISDIR(destination.st_mode)) {
			fprintf(stderr, "Destination type mismatch for %s\n",
				destinationName);
			abort();
		}
		const int destinationFD = destinationExists
			? openDirectoryAt(destinationParentFD, destinationName)
			: createDirectoryAt(
				destinationParentFD, destinationName,
				source.st_mode & ALLPERMS);
		DIR* sourceDirectory = opendir(sourcePath.c_str());
		if (sourceDirectory == nullptr) {
			fprintf(stderr, "Failed to open source directory %s: %s\n",
				sourcePath.c_str(), strerror(errno));
			close(destinationFD);
			abort();
		}
		errno = 0;
		while (struct dirent* entry = readdir(sourceDirectory)) {
			if (!strcmp(entry->d_name, ".") ||
				!strcmp(entry->d_name, "..")) {
				continue;
			}
			const size_t originalLength = sourcePath.size();
			sourcePath.push_back('/');
			sourcePath.append(entry->d_name);
			copyAndSetAttributesAt(
				sourcePath, destinationFD, entry->d_name,
				preserveOwnership);
			sourcePath.resize(originalLength);
			errno = 0;
		}
		if (errno != 0) {
			fprintf(stderr, "Failed to scan source directory %s: %s\n",
				sourcePath.c_str(), strerror(errno));
			closedir(sourceDirectory);
			close(destinationFD);
			abort();
		}
		closedir(sourceDirectory);
		applyDescriptorAttributes(
			destinationFD, source, preserveOwnership, destinationName);
		close(destinationFD);
		return;
	}

	if (destinationExists &&
		(source.st_mode & S_IFMT) != (destination.st_mode & S_IFMT)) {
		fprintf(stderr, "Destination type mismatch for %s\n", destinationName);
		abort();
	}
	if (destinationExists) {
		const int order = compareTimespec(source.st_mtim, destination.st_mtim);
		if (order < 0) {
			return;
		}
		if (order == 0) {
			if (S_ISLNK(source.st_mode)) {
				applySymlinkAttributesAt(
					destinationParentFD, destinationName, source,
					preserveOwnership);
			} else {
				const int destinationFD = openat(
					destinationParentFD, destinationName,
					O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
				struct stat opened;
				if (destinationFD == -1 ||
					fstat(destinationFD, &opened) == -1 ||
					(opened.st_mode & S_IFMT) !=
						(source.st_mode & S_IFMT)) {
					if (destinationFD >= 0) {
						close(destinationFD);
					}
					fprintf(stderr,
						"Destination changed while updating %s\n",
						destinationName);
					abort();
				}
				applyDescriptorAttributes(
					destinationFD, source, preserveOwnership,
					destinationName);
				close(destinationFD);
			}
			return;
		}
	}
	if (S_ISREG(source.st_mode)) {
		replaceRegularFileAt(
			sourcePath, destinationParentFD, destinationName, source,
			preserveOwnership);
	} else if (S_ISLNK(source.st_mode)) {
		replaceSymlinkAt(
			sourcePath, destinationParentFD, destinationName, source,
			preserveOwnership);
	} else {
		fprintf(stderr, "Unsupported source file type for %s\n",
			sourcePath.c_str());
		abort();
	}
}

static void copyDirectoryContentsAt(
	const char* sourceRoot,
	int destinationFD,
	bool preserveOwnership
) {
	std::string sourcePath(sourceRoot);
	struct stat source;
	if (lstat(sourcePath.c_str(), &source) == -1 ||
		!S_ISDIR(source.st_mode)) {
		fprintf(stderr, "Invalid runtime source directory %s: %s\n",
			sourceRoot, strerror(errno));
		abort();
	}
	DIR* sourceDirectory = opendir(sourceRoot);
	if (sourceDirectory == nullptr) {
		fprintf(stderr, "Failed to open runtime source directory %s: %s\n",
			sourceRoot, strerror(errno));
		abort();
	}
	errno = 0;
	while (struct dirent* entry = readdir(sourceDirectory)) {
		if (!strcmp(entry->d_name, ".") ||
			!strcmp(entry->d_name, "..")) {
			continue;
		}
		const size_t originalLength = sourcePath.size();
		sourcePath.push_back('/');
		sourcePath.append(entry->d_name);
		copyAndSetAttributesAt(
			sourcePath, destinationFD, entry->d_name, preserveOwnership);
		sourcePath.resize(originalLength);
		errno = 0;
	}
	if (errno != 0) {
		fprintf(stderr, "Failed to scan runtime source directory %s: %s\n",
			sourceRoot, strerror(errno));
		closedir(sourceDirectory);
		abort();
	}
	closedir(sourceDirectory);
	applyDescriptorAttributes(
		destinationFD, source, preserveOwnership, "prefix root");
}

static void temp_drop_privileges(uid_t uid, gid_t gid, bool rootless) {
	if (rootless) return;
	// it's important to drop GID first, because non-root users can't change their GID
	if (setresgid(gid, gid, 0) < 0) {
		fprintf(stderr, "Failed to temporarily drop group privileges\n");
		exit(1);
	}
	if (setresuid(uid, uid, 0) < 0) {
		fprintf(stderr, "Failed to temporarily drop user privileges\n");
		exit(1);
	}
};

static void perma_drop_privileges(uid_t uid, gid_t gid, bool rootless) {
	if (rootless) return;
	if (setresgid(gid, gid, gid) < 0) {
		fprintf(stderr, "Failed to drop group privileges\n");
		exit(1);
	}
	if (setresuid(uid, uid, uid) < 0) {
		fprintf(stderr, "Failed to drop user privileges\n");
		exit(1);
	}
};

static void regain_privileges(bool rootless) {
	if (rootless) return;
	if (seteuid(0) < 0) {
		fprintf(stderr, "Failed to regain root EUID\n");
		exit(1);
	}
	if (setegid(0) < 0) {
		fprintf(stderr, "Failed to regain root EGID\n");
		exit(1);
	}

	if (setresuid(0, 0, 0) < 0) {
		fprintf(stderr, "Failed to regain root privileges\n");
		exit(1);
	}
	if (setresgid(0, 0, 0) < 0) {
		fprintf(stderr, "Failed to regain root privileges\n");
		exit(1);
	}
};

static int parseInheritedFD(const char* value, const char* description) {
	char* end = nullptr;
	errno = 0;
	const long parsed = strtol(value, &end, 10);
	if (
		errno != 0 ||
		end == value ||
		*end != '\0' ||
		parsed < 0 ||
		parsed > INT_MAX
	) {
		fprintf(stderr, "Invalid inherited %s descriptor\n", description);
		exit(1);
	}
	return static_cast<int>(parsed);
}

static void makeDescriptorCloseOnExec(int fd, const char* description) {
	const int flags = fcntl(fd, F_GETFD);
	if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		fprintf(stderr, "Cannot protect inherited %s descriptor: %s\n",
			description, strerror(errno));
		exit(1);
	}
}

#if DSERVER_ASAN
static void handle_sigusr1(int signum) {
	__lsan_do_recoverable_leak_check();
};
#endif

// perf#30 (doc section 199): a crash probe. MEASURED: the server stops between two statements of the
// process-control completion -- a plain store into a mapping that the address guard one statement earlier said was
// current -- and it emits nothing at all afterwards: no signal message, no terminate, no further log line. A server
// that dies silently is the "instrument that cannot answer" class again, so the fault address and the signal are
// reported here with raw writes only (no stdio, no allocation, nothing that can itself fail).
extern "C" void dserver_crash_probe(int sig, siginfo_t* si, void* uc) {
	static const char pre[] = "[dserver-CRASH sig=";
	static const char mid[] = " addr=0x";
	char digits[20];
	int n = 0;
	unsigned long long v;
	long a, d_, sp_, l;

	for (int fd = 2; fd >= 1; --fd) {
		a = 1; d_ = fd; sp_ = (long)pre; l = sizeof(pre) - 1;
		__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
		a = 1; d_ = fd; sp_ = (long)digits; l = (long)(n = 0);
		v = (unsigned long long)sig;
		if (v == 0) { digits[n++] = '0'; }
		while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
		{
			char rev[20]; int rn = n, i2 = 0;
			for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
			for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
			n = rn;
		}
		a = 1; d_ = fd; sp_ = (long)digits; l = n;
		__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
		a = 1; d_ = fd; sp_ = (long)mid; l = sizeof(mid) - 1;
		__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
		v = si ? (unsigned long long)(unsigned long)si->si_addr : 0;
		n = 0;
		if (v == 0) { digits[n++] = '0'; }
		while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
		{
			char rev[20]; int rn = n, i2 = 0;
			for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
			for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
			n = rn;
		}
		a = 1; d_ = fd; sp_ = (long)digits; l = n;
		__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
		// perf#30 (doc section 221): THE FAULTING INSTRUCTION POINTER. MEASURED need: a crash was reported as
		// "sig=b addr=0x684" -- a small offset with no base -- and the address hypothesis built on it did not
		// survive an API check, so the location has to be reported rather than inferred. `llvm-nm` resolves this
		// against the server binary exactly as it resolved the guest's `_dserver_rpc_fork_wait_for_child + 0x19`.
		{
			// An ADDRESS needs a BASE to become an offset: the runtime pc is meaningless without the load address,
			// and printing the address of a known symbol in the same image makes the subtraction possible exactly
			// as it did for the guest probe. No assumption about non-PIE is made.
			{
				static const char ss[7] = ",self=";
				unsigned long long me = (unsigned long long)(uintptr_t)&dserver_crash_probe;
				a = 1; d_ = fd; sp_ = (long)ss; l = sizeof(ss) - 1;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
				v = me; n = 0;
				if (v == 0) { digits[n++] = '0'; }
				while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
				{
					char rev[20]; int rn = n, i2 = 0;
					for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
					for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
					n = rn;
				}
				a = 1; d_ = fd; sp_ = (long)digits; l = n;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
			}
			// perf#30 (doc section 223): THE CALLER. MEASURED need: two different attempts both died at
			// `ipc_port_destroy + 0x3c`, so the location inside the callee says nothing about WHICH of its callers
			// reached it. The return address of the faulting frame names that caller, and it is printed as an
			// offset-from-self delta exactly like the others so ASLR cannot hide it.
			{
				static const char rs[8] = ",ret=0x";
				unsigned long long ra = 0;
				if (uc != NULL) {
#if defined(__x86_64__)
					unsigned long long rsp = (unsigned long long)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RSP];
					if (rsp != 0) { ra = *(unsigned long long*)(uintptr_t)rsp; }
#endif
				}
				a = 1; d_ = fd; sp_ = (long)rs; l = sizeof(rs) - 1;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
				v = ra; n = 0;
				if (v == 0) { digits[n++] = '0'; }
				while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
				{
					char rev[20]; int rn = n, i2 = 0;
					for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
					for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
					n = rn;
				}
				a = 1; d_ = fd; sp_ = (long)digits; l = n;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
			}
			// perf#30 (doc section 223): A MINI STACK WALK. MEASURED need: `ipc_port_destroy + 0x3c` was reached
			// from at least two different attempts and the single return address printed earlier was MEASURED to be
			// a stack pointer rather than a code address, so the caller could not be named from it. Printing the
			// faulting stack pointer and the first words of that stack gives the resolver a real chain to walk: every
			// word that is a code address in this image resolves to a symbol exactly like `pc` does.
			{
				static const char sps[7] = ",sp=0x";
				unsigned long long rsp0 = 0;
				if (uc != NULL) {
#if defined(__x86_64__)
					rsp0 = (unsigned long long)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RSP];
#endif
				}
				a = 1; d_ = fd; sp_ = (long)sps; l = sizeof(sps) - 1;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
				v = rsp0; n = 0;
				if (v == 0) { digits[n++] = '0'; }
				while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
				{
					char rev[20]; int rn = n, i2 = 0;
					for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
					for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
					n = rn;
				}
				a = 1; d_ = fd; sp_ = (long)digits; l = n;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
			}
			for (int wi = 0; wi < 8; ++wi) {
				unsigned long long w = 0;
				if (uc != NULL) {
#if defined(__x86_64__)
					unsigned long long rspw = (unsigned long long)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RSP];
					if (rspw != 0) { w = *(unsigned long long*)(uintptr_t)(rspw + (unsigned long long)wi * 8ull); }
#endif
				}
				// MEASURED: the first version wrote only ",w0" (three bytes, without the '=' and without the 0x
				// prefix), so the number ran straight on to the tag and the resolver could not parse a single word --
				// an instrument that prints its values unparseably is the same class as one that cannot print them.
				static const char ws[] = ",w0=0x";
				char tag[8];
				tag[0] = ws[0]; tag[1] = ws[1]; tag[2] = (char)('0' + wi);
				tag[3] = ws[3]; tag[4] = ws[4]; tag[5] = ws[5]; tag[6] = '\0';
				a = 1; d_ = fd; sp_ = (long)tag; l = 6;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
				v = w; n = 0;
				if (v == 0) { digits[n++] = '0'; }
				while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
				{
					char rev[20]; int rn = n, i2 = 0;
					for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
					for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
					n = rn;
				}
				a = 1; d_ = fd; sp_ = (long)digits; l = n;
				__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
			}
			static const char pcs[7] = ",pc=0x";
			unsigned long long pc = 0;
			if (uc != NULL) {
#if defined(__x86_64__)
				pc = (unsigned long long)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
				pc = (unsigned long long)((ucontext_t*)uc)->uc_mcontext.gregs[REG_EIP];
#endif
			}
			a = 1; d_ = fd; sp_ = (long)pcs; l = sizeof(pcs) - 1;
			__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
			v = pc;
			n = 0;
			if (v == 0) { digits[n++] = '0'; }
			while (v) { unsigned dd = (unsigned)(v & 0xf); digits[n++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10)); v >>= 4; }
			{
				char rev[20]; int rn = n, i2 = 0;
				for (; i2 < rn; ++i2) { rev[i2] = digits[rn - 1 - i2]; }
				for (i2 = 0; i2 < rn; ++i2) { digits[i2] = rev[i2]; }
				n = rn;
			}
			a = 1; d_ = fd; sp_ = (long)digits; l = n;
			__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
		}
		a = 1; d_ = fd; sp_ = (long)"\n"; l = 1;
		__asm__ volatile("syscall" : "+a"(a), "+D"(d_), "+S"(sp_), "+d"(l) : : "rcx", "r11", "memory");
	}
	_exit(91);
}

static void dserver_install_crash_probe(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = dserver_crash_probe;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
}

// perf#30 (doc section 253): A TERMINATE HANDLER WITH A BACKTRACE. MEASURED need: the server died with
// `terminate called after throwing an instance of 'std::length_error' (basic_string::_M_replace_aux)` -- a C++
// exception, so there is no faulting instruction to decode, the crash probe's registers point into another object, and
// no core is written because the host core pattern pipes to apport. The throw site was therefore unknowable from every
// instrument this project had. `backtrace` is glibc and this is a Linux binary, so the answer is one call away.
static void dserver_terminate_handler(void) {
	static const char tag[] = "[dserver-TERMINATE] backtrace follows\n";
	ssize_t ignored = write(2, tag, sizeof(tag) - 1);
	(void)ignored;
	void* frames[48];
	int n = backtrace(frames, 48);
	backtrace_symbols_fd(frames, n, 2);
	_exit(134);
}

/* RLIMIT_NOFILE budget for the bootstrap closure (dar-dar6x4-perf-5dq.34).
 *
 * The server no longer raises this limit, so the inherited value has to carry the whole closure and the
 * requirement is stated once, here. It is DERIVED from the descriptor high-water mark each role reaches
 * while it establishes its anchors, measured on the canonical build (docs/nofile-virtual-cap-plan.md):
 *
 *     darlingserver 50   (largest: ring control page, doorbell eventfd, launchd socket, accepted guest
 *                         connections)   launchd 28   shellspawn 23   launchctl 21   launcher 10
 *
 * A role's budget is its measured peak plus a quarter for descriptor churn while anchors are established
 * (the closure opens listeners, sockets and pipes in one burst), and the closure requirement is the
 * largest role budget aligned up to a multiple of the alignment below:
 *
 *     align16(50 * 5 / 4) = align16(62) = 64
 *
 * Cross-check, and explicitly not the source of the value: the truthful-no-raise build boots at 59 and
 * fails at 58 (bisected), so the measured wall sits under this requirement, which is what a budget with
 * churn headroom should look like. If a role's peak moves, this comment and the numbers move with it; do
 * not paste a fresh wall value in without redoing the derivation.
 */
static constexpr rlim_t kLargestRoleDescriptorPeak = 50;
static constexpr rlim_t kDescriptorChurnNumerator = 5;
static constexpr rlim_t kDescriptorChurnDenominator = 4;
static constexpr rlim_t kClosureRequirementAlign = 16;
static constexpr rlim_t kMinimumInheritedNofile =
	((kLargestRoleDescriptorPeak * kDescriptorChurnNumerator / kDescriptorChurnDenominator
	  + kClosureRequirementAlign - 1) / kClosureRequirementAlign) * kClosureRequirementAlign;
static constexpr const char* kNofileRequirementReason =
	"largest role peak (darlingserver anchors, 50 descriptors) plus a quarter for bootstrap churn, aligned to 16";

int main(int argc, char** argv) {
	dserver_install_crash_probe();
	std::set_terminate(dserver_terminate_handler);

	const char* prefix = NULL;
	uid_t originalUID = -1;
	gid_t originalGID = -1;
	int pipefd = -1;
	bool fix_permissions = false;
	pid_t launchdGlobalPID = -1;
	int childWaitFDs[2];
	struct rlimit core_limit;

	char *opts;
	char putOld[4096];
	if (argc != 9) {
		fprintf(stderr, "darlingserver is not meant to be started manually\n");
		exit(1);
	}

#if DSERVER_EXTENDED_DEBUG
	if (getenv("DSERVER_WAIT4DEBUGGER")) {
		volatile bool debugged = false;
		while (!debugged);
	}
#endif

	sscanf(argv[5], "%d", &originalUID);
	sscanf(argv[6], "%d", &originalGID);
	sscanf(argv[7], "%d", &pipefd);

	if (argv[8][0] == '1') {
		fix_permissions = true;
	}

	DarlingServer::RuntimeMode runtimeMode;
	std::string prefixPath;
	std::string workdirPath;
	DarlingServer::RuntimePrefixCapability runtimePrefix = [&]() {
	try {
		runtimeMode = DarlingServer::requireRuntimeModeFromEnvironment(
			DARLING_RUNTIME_EUNION_CAPABLE != 0);
		DarlingServer::InheritedRuntimePrefix inherited(
			parseInheritedFD(argv[1], "prefix"),
			parseInheritedFD(argv[2], "prefix parent"),
			argv[3],
			parseInheritedFD(argv[4], "prefix workdir")
		);
		auto anchored = DarlingServer::anchorRuntimeModePrefix(
			std::move(inherited), runtimeMode, originalUID, originalGID);
		prefixPath = anchored.prefixProcPath();
		workdirPath = anchored.workdirProcPath();
		return anchored;
	} catch (const DarlingServer::RuntimeModeError& error) {
		fprintf(stderr, "Cannot select Darling runtime mode: %s\n",
			error.what());
		exit(1);
	}
	}();
	const int prefixFD = runtimePrefix.prefixFD();
	const int workdirFD = runtimePrefix.workdirFD();
	makeDescriptorCloseOnExec(prefixFD, "prefix");
	makeDescriptorCloseOnExec(workdirFD, "prefix workdir");
	prefix = prefixPath.c_str();
	const bool rootless = DarlingServer::runtimeModeIsRootless(runtimeMode);

	if (!rootless && (getuid() != 0 || getgid() != 0)) {
		fprintf(stderr, "darlingserver needs to start as root\n");
		exit(1);
	}
	if (rootless) {
		try {
			DarlingServer::validateRootlessProcessCredentials(
				originalUID, originalGID);
		} catch (const DarlingServer::RuntimeModeError& error) {
			fprintf(stderr, "Cannot enter rootless runtime mode: %s\n",
				error.what());
			exit(1);
		}
	}
	if (rootless && prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
		fprintf(stderr, "Cannot enable rootless child reaping: %s\n", strerror(errno));
		exit(1);
	}

	// temporarily drop privileges to perform some prefix work
	temp_drop_privileges(originalUID, originalGID, rootless);
	setupUserHome(prefixFD, originalUID);
	//setupCoredumpPattern();
	regain_privileges(rootless);

	// TRUTHFUL NOFILE (dar-dar6x4-perf-5dq.34). The server keeps the RLIMIT_NOFILE it inherited and passes
	// it on unchanged. It used to read /proc/sys/fs/nr_open, raise itself to that maximum and push the
	// launchd child back down to the saved default; the ASAN special case existed only because that raise
	// confused ASAN's descriptor sweep. Both halves are gone: with no raise there is nothing for a future
	// creation path to leak and no compensating restore to keep in step with it.
	//
	// What the closure needs is therefore CHECKED, not raised, and the check runs here -- before any guest
	// process exists -- so a host whose inherited limit cannot carry the bootstrap closure gets one
	// diagnostic and a normal non-zero exit instead of a guest that dies through a signal after a
	// shellspawn timeout.
	{
		struct rlimit inherited_nofile;
		if (getrlimit(RLIMIT_NOFILE, &inherited_nofile) != 0) {
			fprintf(stderr, "Failed to read inherited FD rlimit: %s\n", strerror(errno));
			exit(1);
		}
		if (inherited_nofile.rlim_cur < kMinimumInheritedNofile) {
			fprintf(stderr,
				"Insufficient RLIMIT_NOFILE: the inherited soft limit is %lu, but the bootstrap closure needs %lu (%s). "
				"Raise the limit for the process that starts the server (for example, ulimit -n %lu) and retry; "
				"no guest process has been started.\n",
				(unsigned long)inherited_nofile.rlim_cur,
				(unsigned long)kMinimumInheritedNofile,
				kNofileRequirementReason,
				(unsigned long)kMinimumInheritedNofile);
			exit(1);
		}
	}

	if (getrlimit(RLIMIT_CORE, &core_limit) != 0) {
		fprintf(stderr, "Failed to read default core rlimit: %s\n", strerror(errno));
	} else {
		// increase the core limit to the maximum
		core_limit.rlim_cur = core_limit.rlim_max;

		if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
			fprintf(stderr, "Warning: failed to increase core limit: %s\n", strerror(errno));
		}
	}

	if (!rootless) {
		// Since overlay cannot be mounted inside user namespaces, we have to setup a new mount namespace
		// and do the mount while we can be root
		if (unshare(CLONE_NEWNS) != 0)
		{
			fprintf(stderr, "Cannot unshare PID and mount namespaces: %s\n", strerror(errno));
			exit(1);
		}

		int shmMountFlags = MS_NOSUID | MS_NODEV;
		// Workaround for dumb Microsoft bug: https://github.com/microsoft/WSL/issues/8777
		if (!isOnWsl1())
		{
			shmMountFlags |= MS_NOEXEC;
		}

		umount("/dev/shm");
		if (mount("tmpfs", "/dev/shm", "tmpfs", shmMountFlags, NULL) != 0)
		{
			fprintf(stderr, "Cannot mount new /dev/shm: %s\n", strerror(errno));
			exit(1);
		}
	}
	const bool useOverlayFs =
		DarlingServer::runtimeModeUsesOverlay(runtimeMode);
	const bool useEunionPrefix =
		DarlingServer::runtimeModeUsesEunion(runtimeMode);

	if (useOverlayFs) {
		// Because systemd marks / as MS_SHARED and we would inherit this into the overlay mount,
		// causing it not to be unmounted once the init process dies.
		if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0)
		{
			fprintf(stderr, "Cannot remount / as slave: %s\n", strerror(errno));
			exit(1);
		}

		opts = (char*) malloc(
			strlen(prefix) + workdirPath.size() + sizeof(LIBEXEC_PATH) + 100);

		const char* opts_fmt = "lowerdir=%s,upperdir=%s,workdir=%s,index=off";

		sprintf(opts, opts_fmt, LIBEXEC_PATH, prefix, workdirPath.c_str());

		// Mount overlay onto our prefix
		if (mount("overlay", prefix, "overlay", 0, opts) != 0)
		{
			if (errno == EINVAL) {
				opts_fmt = "lowerdir=%s,upperdir=%s,workdir=%s";
				sprintf(opts, opts_fmt, LIBEXEC_PATH, prefix, workdirPath.c_str());
				if (mount("overlay", prefix, "overlay", 0, opts) == 0) {
					goto mount_ok;
				}
			}
			fprintf(stderr, "Cannot mount overlay: %s\n", strerror(errno));
			exit(1);
		}

	mount_ok:
		free(opts);
	} else if (useEunionPrefix) {
		setupEunionPrefix(prefixFD);
	} else {
		copyDirectoryContentsAt(LIBEXEC_PATH, prefixFD, !rootless);
	}

	// This is executed once at prefix creation
	if (fix_permissions && !rootless) {
		const char* extra_paths[] = {
			"/private/etc/passwd",
			"/private/etc/master.passwd",
			"/private/etc/group",
		};
		fixPermissionsRecursiveFD(prefixFD, originalUID, originalGID);

		for (size_t i = 0; i < sizeof(extra_paths) / sizeof(*extra_paths); ++i) {
			chownRelativeAt(
				prefixFD, extra_paths[i] + 1,
				originalUID, originalGID);
		}
	}

	// temporarily drop privileges and do some prefix work
	temp_drop_privileges(originalUID, originalGID, rootless);
	darlingPreInit(prefixFD);
	regain_privileges(rootless);

	// Tell the parent we're ready
	write(pipefd, ".", 1);
	close(pipefd);

	if (pipe(childWaitFDs) != 0) {
		std::cerr << "Failed to create child waiting pipe: " << strerror(errno) << std::endl;
		exit(1);
	}

	if (rootless) {
		launchdGlobalPID = fork();
	} else {
		// we have to use `clone` rather than `fork` to create the process in its own PID namespace
		// and still be able to spawn new processes and threads of our own
		launchdGlobalPID = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, 0);
	}

	if (launchdGlobalPID < 0) {
		fprintf(stderr, "Failed to fork to start launchd: %s\n", strerror(errno));
		exit(1);
	} else if (launchdGlobalPID == 0) {
		// this is the child
		char buf[1];

		close(childWaitFDs[1]);

		if (!rootless) {
			snprintf(putOld, sizeof(putOld), "%s/proc", prefix);

			// mount procfs for our new PID namespace
			if (mount("proc", putOld, "proc", 0, "") != 0)
			{
				fprintf(stderr, "Cannot mount procfs: %s\n", strerror(errno));
				exit(1);
			}
		}
		// drop our privileges now
		perma_drop_privileges(originalUID, originalGID, rootless);
		prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

		// dar-pot: bind this launchd's lifetime to darlingserver's. We are PID 1
		// of the guest PID namespace; if darlingserver (our parent) ever dies --
		// crash, SIGKILL, or the launcher exiting -- the kernel would otherwise
		// leave us (and thus every guest/mldr in this namespace) running,
		// orphaned, until they accumulate and wedge the next fresh boot. Asking
		// for SIGKILL on parent death makes ns-PID-1 die with the server; the
		// kernel then cascade-kills the whole guest namespace. No orphaned guests
		// can outlive their server. This MUST come AFTER perma_drop_privileges (a
		// credential change clears the pdeathsig) and BEFORE spawnLaunchd (so no
		// guest can exist before the binding is in place). The pdeathsig also
		// survives the exec into mldr/launchd (mldr is not set-uid, so exec keeps
		// it). The classic "parent died in the clone->here window" race is already
		// covered by the childWaitFDs handshake below: we arm pdeathsig before
		// that read(), so a parent that already died triggers our SIGKILL rather
		// than us proceeding to boot an orphaned launchd. (Note: getppid() cannot
		// detect it here -- across the PID-ns boundary it always reports 0.)
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);

		// No FD-limit restore here (dar-dar6x4-perf-5dq.34): the parent does not raise its own limit any
		// more, so this child simply keeps the inherited one -- the same truthful value every other
		// process of the closure gets.

		// wait for the parent to give us the green light
		read(childWaitFDs[0], buf, 1);
		close(childWaitFDs[0]);

		spawnLaunchd(prefix, runtimeMode, prefixFD);
		__builtin_unreachable();
	}

	// this is the parent
	close(childWaitFDs[0]);

	// drop our privileges
	perma_drop_privileges(originalUID, originalGID, rootless);
	prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

#if DSERVER_ASAN
	// set up a signal handler to print leak info
	struct sigaction leak_info_action;
	leak_info_action.sa_handler = handle_sigusr1;
	leak_info_action.sa_flags = SA_RESTART;
	sigemptyset(&leak_info_action.sa_mask);
	sigaction(SIGUSR1, &leak_info_action, NULL);
#endif

	// create the server
	auto server = new DarlingServer::Server(
		prefix, prefixFD, rootless ? launchdGlobalPID : 0);

	// tell the child to go ahead; the socket has been created
	write(childWaitFDs[1], ".", 1);
	close(childWaitFDs[1]);

	// start the main loop
	server->start();

	// this should never happen
	std::cerr << "Server exited main loop!" << std::endl;
	delete server;

	return 1;
};
