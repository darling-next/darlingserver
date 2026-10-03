#pragma once

#include <array>
#include <limits.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace DarlingServer {

enum class RuntimeMode {
	PrivilegedOverlay,
	PrivilegedCopy,
	PrivilegedEunion,
	RootlessEunion,
};

class RuntimeModeError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class RuntimePrefixCapability;

class InheritedRuntimePrefix final {
public:
	InheritedRuntimePrefix(
		int prefixFD,
		int parentFD,
		const char* leaf,
		int workdirFD
	);
	~InheritedRuntimePrefix();
	InheritedRuntimePrefix(const InheritedRuntimePrefix&) = delete;
	InheritedRuntimePrefix& operator=(const InheritedRuntimePrefix&) = delete;
	InheritedRuntimePrefix(InheritedRuntimePrefix&& other) noexcept;
	InheritedRuntimePrefix& operator=(InheritedRuntimePrefix&& other) noexcept;

private:
	friend class RuntimePrefixCapability;
	friend RuntimePrefixCapability anchorRuntimeModePrefix(
		InheritedRuntimePrefix&&,
		RuntimeMode,
		uid_t,
		gid_t
	);

	int prefixFD_;
	int parentFD_;
	std::array<char, NAME_MAX + 1> leaf_;
	int workdirFD_;
};

class RuntimePrefixCapability final {
public:
	~RuntimePrefixCapability();
	RuntimePrefixCapability(const RuntimePrefixCapability&) = delete;
	RuntimePrefixCapability& operator=(const RuntimePrefixCapability&) = delete;
	RuntimePrefixCapability(RuntimePrefixCapability&& other) noexcept;
	RuntimePrefixCapability& operator=(RuntimePrefixCapability&& other) noexcept;

	int prefixFD() const noexcept;
	int workdirFD() const noexcept;
	std::string prefixProcPath() const;
	std::string workdirProcPath() const;

private:
	friend RuntimePrefixCapability anchorRuntimeModePrefix(
		InheritedRuntimePrefix&&,
		RuntimeMode,
		uid_t,
		gid_t
	);
	RuntimePrefixCapability(int prefixFD, int workdirFD) noexcept;

	int prefixFD_;
	int workdirFD_;
};

RuntimeMode requireRuntimeModeFromEnvironment(bool eunionCapable);
std::string_view runtimeModeName(RuntimeMode mode);
bool runtimeModeIsRootless(RuntimeMode mode);
bool runtimeModeUsesOverlay(RuntimeMode mode);
bool runtimeModeUsesEunion(RuntimeMode mode);
RuntimePrefixCapability anchorRuntimeModePrefix(
	InheritedRuntimePrefix&& inherited,
	RuntimeMode mode,
	uid_t ownerUID,
	gid_t ownerGID
);
void validateRootlessProcessCredentials(uid_t expectedUID, gid_t expectedGID);

}
