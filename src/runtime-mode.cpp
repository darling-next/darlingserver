#define _GNU_SOURCE 1

#include <darlingserver/runtime-mode.hpp>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace DarlingServer {

static constexpr const char* kCanonicalEnvironment = "DARLING_RUNTIME_MODE";
static constexpr const char* kStateName = ".darling-prefix-state-v2";
static constexpr const char* kStateHeader = "DARLING_PREFIX_STATE_V2";
static constexpr const char* kStateProvenance =
	"darling-runtime-prefix-lifecycle-v2";

static void closeOwnedFD(int& fd) noexcept
{
	if (fd >= 0)
		close(fd);
	fd = -1;
}

InheritedRuntimePrefix::InheritedRuntimePrefix(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD
) :
	prefixFD_(prefixFD),
	parentFD_(parentFD),
	workdirFD_(workdirFD)
{
	if (leaf == nullptr ||
		*leaf == '\0' ||
		std::strcmp(leaf, ".") == 0 ||
		std::strcmp(leaf, "..") == 0 ||
		std::strchr(leaf, '/') != nullptr ||
		strnlen(leaf, NAME_MAX + 1) > NAME_MAX) {
		closeOwnedFD(prefixFD_);
		closeOwnedFD(parentFD_);
		closeOwnedFD(workdirFD_);
		throw RuntimeModeError(
			"inherited runtime prefix leaf is invalid");
	}
	const size_t length = std::strlen(leaf);
	leaf_.fill('\0');
	std::memcpy(leaf_.data(), leaf, length + 1);
}

InheritedRuntimePrefix::~InheritedRuntimePrefix()
{
	closeOwnedFD(prefixFD_);
	closeOwnedFD(parentFD_);
	closeOwnedFD(workdirFD_);
}

InheritedRuntimePrefix::InheritedRuntimePrefix(
	InheritedRuntimePrefix&& other
) noexcept :
	prefixFD_(other.prefixFD_),
	parentFD_(other.parentFD_),
	leaf_(other.leaf_),
	workdirFD_(other.workdirFD_)
{
	other.prefixFD_ = -1;
	other.parentFD_ = -1;
	other.workdirFD_ = -1;
	other.leaf_.fill('\0');
}

InheritedRuntimePrefix& InheritedRuntimePrefix::operator=(
	InheritedRuntimePrefix&& other
) noexcept
{
	if (this != &other) {
		closeOwnedFD(prefixFD_);
		closeOwnedFD(parentFD_);
		closeOwnedFD(workdirFD_);
		prefixFD_ = other.prefixFD_;
		parentFD_ = other.parentFD_;
		leaf_ = other.leaf_;
		workdirFD_ = other.workdirFD_;
		other.prefixFD_ = -1;
		other.parentFD_ = -1;
		other.workdirFD_ = -1;
		other.leaf_.fill('\0');
	}
	return *this;
}

RuntimePrefixCapability::RuntimePrefixCapability(
	int prefixFD,
	int workdirFD
) noexcept :
	prefixFD_(prefixFD),
	workdirFD_(workdirFD)
{
}

RuntimePrefixCapability::~RuntimePrefixCapability()
{
	closeOwnedFD(prefixFD_);
	closeOwnedFD(workdirFD_);
}

RuntimePrefixCapability::RuntimePrefixCapability(
	RuntimePrefixCapability&& other
) noexcept :
	prefixFD_(other.prefixFD_),
	workdirFD_(other.workdirFD_)
{
	other.prefixFD_ = -1;
	other.workdirFD_ = -1;
}

RuntimePrefixCapability& RuntimePrefixCapability::operator=(
	RuntimePrefixCapability&& other
) noexcept
{
	if (this != &other) {
		closeOwnedFD(prefixFD_);
		closeOwnedFD(workdirFD_);
		prefixFD_ = other.prefixFD_;
		workdirFD_ = other.workdirFD_;
		other.prefixFD_ = -1;
		other.workdirFD_ = -1;
	}
	return *this;
}

int RuntimePrefixCapability::prefixFD() const noexcept
{
	return prefixFD_;
}

int RuntimePrefixCapability::workdirFD() const noexcept
{
	return workdirFD_;
}

static uint64_t parseUnsignedField(
	std::string_view line,
	std::string_view key
)
{
	if (line.size() <= key.size() ||
		line.substr(0, key.size()) != key)
		throw RuntimeModeError("runtime prefix state field is malformed");
	uint64_t value = 0;
	const char* begin = line.data() + key.size();
	const char* end = line.data() + line.size();
	auto parsed = std::from_chars(begin, end, value, 10);
	if (parsed.ec != std::errc() || parsed.ptr != end)
		throw RuntimeModeError("runtime prefix state numeric field is malformed");
	return value;
}

static std::array<std::string_view, 9> splitStateLines(
	const std::string& content
)
{
	std::array<std::string_view, 9> lines;
	size_t offset = 0;
	for (size_t index = 0; index < lines.size(); ++index) {
		size_t newline = content.find('\n', offset);
		if (newline == std::string::npos)
			throw RuntimeModeError("runtime prefix state is truncated");
		lines[index] = std::string_view(content).substr(
			offset, newline - offset);
		offset = newline + 1;
	}
	if (offset != content.size())
		throw RuntimeModeError("runtime prefix state has extra fields");
	return lines;
}

static std::string readPrefixState(
	int prefixFD,
	uid_t ownerUID,
	gid_t ownerGID
)
{
	struct stat named;
	if (fstatat(prefixFD, kStateName, &named,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(named.st_mode) ||
		!S_ISREG(named.st_mode) ||
		named.st_nlink != 1 ||
		(named.st_mode & 07777) != 0600 ||
		named.st_uid != ownerUID ||
		named.st_gid != ownerGID ||
		named.st_size <= 0 ||
		named.st_size >= 1024)
		throw RuntimeModeError(
			"runtime prefix has no valid typed state");
	int fd = openat(prefixFD, kStateName,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		throw RuntimeModeError(
			"cannot open runtime prefix state");
	struct stat opened;
	if (fstat(fd, &opened) != 0 ||
		opened.st_dev != named.st_dev ||
		opened.st_ino != named.st_ino ||
		!S_ISREG(opened.st_mode)) {
		close(fd);
		throw RuntimeModeError(
			"runtime prefix state changed during inspection");
	}
	std::string content(static_cast<size_t>(opened.st_size), '\0');
	size_t offset = 0;
	while (offset < content.size()) {
		ssize_t length = read(fd, content.data() + offset,
			content.size() - offset);
		if (length < 0 && errno == EINTR)
			continue;
		if (length <= 0) {
			close(fd);
			throw RuntimeModeError(
				"cannot read runtime prefix state");
		}
		offset += static_cast<size_t>(length);
	}
	if (close(fd) != 0)
		throw RuntimeModeError(
			"cannot close runtime prefix state");
	if (content.find('\0') != std::string::npos)
		throw RuntimeModeError(
			"runtime prefix state contains a NUL byte");
	return content;
}

static void validateTypedPrefixState(
	int prefixFD,
	const struct stat& prefixStatus,
	RuntimeMode mode,
	uid_t ownerUID,
	gid_t ownerGID
)
{
	const std::string content =
		readPrefixState(prefixFD, ownerUID, ownerGID);
	auto lines = splitStateLines(content);
	if (lines[0] != kStateHeader)
		throw RuntimeModeError("runtime prefix state schema is malformed");
	uint64_t schema = parseUnsignedField(lines[1], "schema_version=");
	if (schema > 2)
		throw RuntimeModeError("runtime prefix state uses a newer schema");
	if (schema != 2)
		throw RuntimeModeError("runtime prefix state schema is incompatible");
	const std::string modeField = "runtime_mode=" +
		std::string(runtimeModeName(mode));
	if (lines[2] != modeField)
		throw RuntimeModeError("runtime prefix mode mismatch");
	uint64_t generation = parseUnsignedField(lines[3], "generation=");
	uint64_t device = parseUnsignedField(lines[4], "prefix_device=");
	uint64_t inode = parseUnsignedField(lines[5], "prefix_inode=");
	uint64_t uid = parseUnsignedField(lines[6], "owner_uid=");
	uint64_t gid = parseUnsignedField(lines[7], "owner_gid=");
	const std::string provenanceField =
		"provenance=" + std::string(kStateProvenance);
	if (generation == 0 ||
		prefixStatus.st_uid != ownerUID ||
		prefixStatus.st_gid != ownerGID ||
		device != static_cast<uint64_t>(prefixStatus.st_dev) ||
		inode != static_cast<uint64_t>(prefixStatus.st_ino) ||
		uid != static_cast<uint64_t>(ownerUID) ||
		gid != static_cast<uint64_t>(ownerGID) ||
		lines[8] != provenanceField)
		throw RuntimeModeError(
			"runtime prefix typed state is incompatible");
}

std::string_view runtimeModeName(RuntimeMode mode)
{
	switch (mode) {
		case RuntimeMode::PrivilegedOverlay:
			return "privileged-overlay";
		case RuntimeMode::PrivilegedCopy:
			return "privileged-copy";
		case RuntimeMode::PrivilegedEunion:
			return "privileged-eunion";
		case RuntimeMode::RootlessEunion:
			return "rootless-eunion";
	}
	throw RuntimeModeError("invalid Darling runtime mode");
}

static RuntimeMode parseRuntimeMode(const char* value)
{
	if (value == nullptr)
		throw RuntimeModeError("canonical DARLING_RUNTIME_MODE is missing");
	if (std::strcmp(value, "privileged-overlay") == 0)
		return RuntimeMode::PrivilegedOverlay;
	if (std::strcmp(value, "privileged-copy") == 0)
		return RuntimeMode::PrivilegedCopy;
	if (std::strcmp(value, "privileged-eunion") == 0)
		return RuntimeMode::PrivilegedEunion;
	if (std::strcmp(value, "rootless-eunion") == 0)
		return RuntimeMode::RootlessEunion;
	throw RuntimeModeError("canonical DARLING_RUNTIME_MODE is invalid");
}

RuntimeMode requireRuntimeModeFromEnvironment(bool eunionCapable)
{
	if (std::getenv("DARLING_ROOTLESS") != nullptr ||
		std::getenv("DARLING_NOOVERLAYFS") != nullptr ||
		std::getenv("DARLING_EUNION") != nullptr)
		throw RuntimeModeError(
			"legacy runtime flags are forbidden after launcher normalization");

	RuntimeMode mode = parseRuntimeMode(std::getenv(kCanonicalEnvironment));
	if (runtimeModeUsesEunion(mode) && !eunionCapable)
		throw RuntimeModeError(
			"canonical runtime mode requires an E-UNION-capable build");
	return mode;
}

bool runtimeModeIsRootless(RuntimeMode mode)
{
	return mode == RuntimeMode::RootlessEunion;
}

bool runtimeModeUsesOverlay(RuntimeMode mode)
{
	return mode == RuntimeMode::PrivilegedOverlay;
}

bool runtimeModeUsesEunion(RuntimeMode mode)
{
	return mode == RuntimeMode::PrivilegedEunion ||
		mode == RuntimeMode::RootlessEunion;
}

void validateRootlessProcessCredentials(
	uid_t expectedUID,
	gid_t expectedGID
)
{
	if (expectedUID == 0 || expectedGID == 0)
		throw RuntimeModeError(
			"rootless mode requires non-root invoking user and group IDs");

	uid_t realUID;
	uid_t effectiveUID;
	uid_t savedUID;
	gid_t realGID;
	gid_t effectiveGID;
	gid_t savedGID;
	if (getresuid(&realUID, &effectiveUID, &savedUID) != 0 ||
		getresgid(&realGID, &effectiveGID, &savedGID) != 0)
		throw RuntimeModeError(
			std::string("cannot inspect rootless process credentials: ") +
			std::strerror(errno));
	if (realUID != expectedUID ||
		effectiveUID != expectedUID ||
		savedUID != expectedUID ||
		realGID != expectedGID ||
		effectiveGID != expectedGID ||
		savedGID != expectedGID)
		throw RuntimeModeError(
			"rootless process retained unexpected real, effective, or saved credentials");
}

RuntimePrefixCapability anchorRuntimeModePrefix(
	InheritedRuntimePrefix&& inherited,
	RuntimeMode mode,
	uid_t ownerUID,
	gid_t ownerGID
)
{
	const int prefixFD = inherited.prefixFD_;
	const int parentFD = inherited.parentFD_;
	const int workdirFD = inherited.workdirFD_;
	const char* leaf = inherited.leaf_.data();
	if (prefixFD < 0 || parentFD < 0 || workdirFD < 0)
		throw RuntimeModeError("inherited runtime prefix handle is invalid");

	struct stat opened;
	struct stat parent;
	struct stat named;
	struct stat workdirOpened;
	struct stat workdirNamed;
	if (fstat(prefixFD, &opened) != 0 ||
		fstat(parentFD, &parent) != 0 ||
		!S_ISDIR(opened.st_mode) ||
		!S_ISDIR(parent.st_mode))
		throw RuntimeModeError(
			"inherited runtime prefix descriptors are not directories");
	if (fstatat(parentFD, leaf, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(named.st_mode) ||
		!S_ISDIR(named.st_mode) ||
		named.st_dev != opened.st_dev ||
		named.st_ino != opened.st_ino)
		throw RuntimeModeError(
			"runtime prefix name changed before darlingserver handoff");
	std::string workdirLeaf(leaf);
	workdirLeaf += ".workdir";
	if (workdirLeaf.size() > NAME_MAX ||
		fstat(workdirFD, &workdirOpened) != 0 ||
		!S_ISDIR(workdirOpened.st_mode) ||
		(workdirOpened.st_mode & 07777) != 0755 ||
		workdirOpened.st_uid != ownerUID ||
		workdirOpened.st_gid != ownerGID ||
		fstatat(
			parentFD, workdirLeaf.c_str(), &workdirNamed,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(workdirNamed.st_mode) ||
		!S_ISDIR(workdirNamed.st_mode) ||
		workdirNamed.st_dev != workdirOpened.st_dev ||
		workdirNamed.st_ino != workdirOpened.st_ino)
		throw RuntimeModeError(
			"runtime prefix workdir changed before darlingserver handoff");

	validateTypedPrefixState(
		prefixFD, opened, mode, ownerUID, ownerGID);

	if (close(inherited.parentFD_) != 0)
		throw RuntimeModeError(
			"cannot close validated runtime prefix parent descriptor");
	inherited.parentFD_ = -1;
	inherited.prefixFD_ = -1;
	inherited.workdirFD_ = -1;
	inherited.leaf_.fill('\0');
	return RuntimePrefixCapability(prefixFD, workdirFD);
}

static std::string runtimePrefixProcPath(int prefixFD)
{
	struct stat status;
	if (prefixFD < 0 || fstat(prefixFD, &status) != 0 ||
		!S_ISDIR(status.st_mode))
		throw RuntimeModeError("runtime prefix fd is not an open directory");
	return "/proc/self/fd/" + std::to_string(prefixFD);
}

std::string RuntimePrefixCapability::prefixProcPath() const
{
	return runtimePrefixProcPath(prefixFD_);
}

std::string RuntimePrefixCapability::workdirProcPath() const
{
	return runtimePrefixProcPath(workdirFD_);
}

}
