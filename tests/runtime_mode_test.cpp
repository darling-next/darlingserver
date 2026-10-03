#include <darlingserver/runtime-mode.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

using namespace DarlingServer;

static void require(bool condition, const char* message)
{
	if (!condition) {
		std::cerr << "FAIL " << message << '\n';
		std::exit(1);
	}
}

static void clearEnvironment()
{
	unsetenv("DARLING_RUNTIME_MODE");
	unsetenv("DARLING_ROOTLESS");
	unsetenv("DARLING_NOOVERLAYFS");
	unsetenv("DARLING_EUNION");
}

static void requireMode(const char* name, RuntimeMode expected)
{
	clearEnvironment();
	setenv("DARLING_RUNTIME_MODE", name, 1);
	require(requireRuntimeModeFromEnvironment(true) == expected,
		"canonical runtime mode did not propagate to darlingserver");
}

static void requireFailure(bool capable)
{
	try {
		(void)requireRuntimeModeFromEnvironment(capable);
	} catch (const RuntimeModeError&) {
		return;
	}
	require(false, "invalid runtime environment was accepted");
}

static void writeState(const std::filesystem::path& directory)
{
	struct stat status;
	require(stat(directory.c_str(), &status) == 0,
		"stat runtime prefix state fixture");
	std::ofstream output(directory / ".darling-prefix-state-v2");
	output << "DARLING_PREFIX_STATE_V2\n";
	output << "schema_version=2\n";
	output << "runtime_mode=rootless-eunion\n";
	output << "generation=1\n";
	output << "prefix_device=" << status.st_dev << '\n';
	output << "prefix_inode=" << status.st_ino << '\n';
	output << "owner_uid=" << getuid() << '\n';
	output << "owner_gid=" << getgid() << '\n';
	output << "provenance=darling-runtime-prefix-lifecycle-v2\n";
	require(output.good(), "write typed runtime prefix state fixture");
	output.close();
	require(chmod(
			(directory / ".darling-prefix-state-v2").c_str(), 0600) == 0,
		"restrict typed runtime prefix state fixture");
}

static void requireRejectedPrefixFD(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD,
	const char* message
)
{
	try {
		InheritedRuntimePrefix inherited(
			dup(prefixFD), dup(parentFD), leaf, dup(workdirFD));
		(void)anchorRuntimeModePrefix(
			std::move(inherited), RuntimeMode::RootlessEunion,
			getuid(), getgid());
		require(false, message);
	} catch (const RuntimeModeError&) {
	}
}

static RuntimePrefixCapability requireAnchoredPrefix(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD,
	RuntimeMode mode = RuntimeMode::RootlessEunion
)
{
	const int prefixCopy = dup(prefixFD);
	const int parentCopy = dup(parentFD);
	const int workdirCopy = dup(workdirFD);
	require(prefixCopy >= 0 && parentCopy >= 0 && workdirCopy >= 0,
		"duplicate inherited descriptor fixture");
	InheritedRuntimePrefix inherited(
		prefixCopy, parentCopy, leaf, workdirCopy);
	return anchorRuntimeModePrefix(
		std::move(inherited), mode, getuid(), getgid());
}

int main()
{
	static_assert(!std::is_copy_constructible_v<InheritedRuntimePrefix>);
	static_assert(!std::is_copy_assignable_v<InheritedRuntimePrefix>);
	static_assert(std::is_nothrow_move_constructible_v<InheritedRuntimePrefix>);
	static_assert(!std::is_copy_constructible_v<RuntimePrefixCapability>);
	static_assert(!std::is_copy_assignable_v<RuntimePrefixCapability>);
	static_assert(std::is_nothrow_move_constructible_v<RuntimePrefixCapability>);
	static_assert(sizeof(RuntimePrefixCapability) == 2 * sizeof(int));

	requireMode("privileged-overlay", RuntimeMode::PrivilegedOverlay);
	requireMode("privileged-copy", RuntimeMode::PrivilegedCopy);
	requireMode("privileged-eunion", RuntimeMode::PrivilegedEunion);
	requireMode("rootless-eunion", RuntimeMode::RootlessEunion);

	clearEnvironment();
	requireFailure(true);
	setenv("DARLING_RUNTIME_MODE", "unknown", 1);
	requireFailure(true);
	setenv("DARLING_RUNTIME_MODE", "rootless-eunion", 1);
	requireFailure(false);
	setenv("DARLING_ROOTLESS", "1", 1);
	requireFailure(true);
	clearEnvironment();

	validateRootlessProcessCredentials(getuid(), getgid());
	try {
		validateRootlessProcessCredentials(0, getgid());
		require(false, "root user identity was accepted for rootless mode");
	} catch (const RuntimeModeError&) {
	}
	try {
		validateRootlessProcessCredentials(getuid(), 0);
		require(false, "root group identity was accepted for rootless mode");
	} catch (const RuntimeModeError&) {
	}

	char fixture[] = "/tmp/dserver-runtime-mode.XXXXXX";
	char* root = mkdtemp(fixture);
	require(root != nullptr, "mkdtemp");
	const std::filesystem::path prefix =
		std::filesystem::path(root) / "prefix";
	std::filesystem::create_directory(prefix);
	const std::filesystem::path workdir =
		std::filesystem::path(root) / "prefix.workdir";
	std::filesystem::create_directory(workdir);
	require(chmod(prefix.c_str(), 0755) == 0 &&
			chmod(workdir.c_str(), 0755) == 0,
		"normalize prefix fixture directory modes");
	const std::string marker =
		(prefix / ".darling-prefix-state-v2").string();
	const int parentFD =
		open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	const int prefixFD =
		open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	const int workdirFD =
		open(workdir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	require(parentFD >= 0 && prefixFD >= 0 && workdirFD >= 0,
		"open prefix fixture descriptors");

	try {
		(void)requireAnchoredPrefix(
			prefixFD, parentFD, "prefix", workdirFD);
		require(false, "missing marker was accepted");
	} catch (const RuntimeModeError&) {
		require(!std::filesystem::exists(marker),
			"missing-marker validation mutated the prefix");
	}

	writeState(prefix);
	auto validated = requireAnchoredPrefix(
		prefixFD, parentFD, "prefix", workdirFD);
	const std::string retainedAlias = validated.prefixProcPath();
	struct stat aliasStatus;
	struct stat prefixStatus;
	require(stat(retainedAlias.c_str(), &aliasStatus) == 0,
		"retained prefix alias stat");
	require(fstat(prefixFD, &prefixStatus) == 0 &&
		aliasStatus.st_dev == prefixStatus.st_dev &&
		aliasStatus.st_ino == prefixStatus.st_ino,
		"retained prefix alias does not name exact descriptor");
	try {
		(void)requireAnchoredPrefix(
			prefixFD, parentFD, "prefix", workdirFD,
			RuntimeMode::PrivilegedOverlay);
		require(false, "mismatched marker was accepted");
	} catch (const RuntimeModeError&) {
		std::ifstream input(marker);
		std::string content;
		std::getline(input, content);
		require(content == "DARLING_PREFIX_STATE_V2",
			"mismatch validation mutated the typed state");
	}

	std::filesystem::path target = std::filesystem::path(root) / "target";
	std::filesystem::create_directories(target);
	writeState(target);
	std::filesystem::path sentinel = target / "sentinel";
	{
		std::ofstream output(sentinel);
		output << "unchanged\n";
	}
	const std::filesystem::path parked =
		std::filesystem::path(root) / "prefix.parked";
	require(rename(prefix.c_str(), parked.c_str()) == 0,
		"park inspected prefix");
	require(symlink(target.c_str(), prefix.c_str()) == 0,
		"replace inspected prefix with symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"rename-swap runtime prefix symlink was accepted");
	struct stat retainedStatus;
	require(stat(retainedAlias.c_str(), &retainedStatus) == 0 &&
		retainedStatus.st_dev == prefixStatus.st_dev &&
		retainedStatus.st_ino == prefixStatus.st_ino,
		"rename-swap redirected retained prefix alias");
	{
		std::ifstream input(sentinel);
		std::string content;
		std::getline(input, content);
		require(content == "unchanged",
			"rename-swap rejection mutated its target");
	}
	require(unlink(prefix.c_str()) == 0 &&
		rename(parked.c_str(), prefix.c_str()) == 0,
		"restore inspected prefix fixture");
	(void)requireAnchoredPrefix(
		prefixFD, parentFD, "prefix", workdirFD);
	const std::filesystem::path parkedWorkdir =
		std::filesystem::path(root) / "prefix.workdir.parked";
	require(rename(workdir.c_str(), parkedWorkdir.c_str()) == 0,
		"park inspected workdir");
	require(symlink(target.c_str(), workdir.c_str()) == 0,
		"replace inspected workdir with symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"rename-swap runtime workdir symlink was accepted");
	require(unlink(workdir.c_str()) == 0 &&
		rename(parkedWorkdir.c_str(), workdir.c_str()) == 0,
		"restore inspected workdir fixture");
	(void)requireAnchoredPrefix(
		prefixFD, parentFD, "prefix", workdirFD);

	std::filesystem::remove(marker);
	require(symlink(target.c_str(), marker.c_str()) == 0,
		"create marker symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"runtime marker symlink was accepted");
	require(!std::filesystem::exists(target / "mutated"),
		"marker symlink rejection mutated target");

	close(prefixFD);
	close(workdirFD);
	close(parentFD);
	std::filesystem::remove_all(target);
	std::filesystem::remove_all(prefix);
	std::filesystem::remove_all(workdir);
	std::filesystem::remove(root);
	std::cout << "DSERVER_RUNTIME_MODE_CONTRACT_OK\n";
	return 0;
}
