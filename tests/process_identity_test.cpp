#include <darlingserver/process-identity.hpp>

#include <cassert>

int main() {
	using DarlingServer::ProcessIdentity::initNamespaceID;
	using DarlingServer::ProcessIdentity::isMainThread;
	using DarlingServer::ProcessIdentity::namespaceIDForPeer;

	assert(namespaceIDForPeer(0, 4100, 4100) == 4100);
	assert(namespaceIDForPeer(4100, 4101, 4101) == 4101);
	assert(namespaceIDForPeer(4100, 4100, 4100) == initNamespaceID);
	assert(namespaceIDForPeer(4100, 4101, initNamespaceID) == 4101);

	assert(isMainThread(73, 8100, 73, 8100));
	assert(isMainThread(8100, 8100, initNamespaceID, 8100));
	assert(!isMainThread(8101, 8101, initNamespaceID, 8100));
	assert(!isMainThread(8100, 8101, initNamespaceID, 8100));

	return 0;
}
