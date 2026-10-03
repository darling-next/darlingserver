#ifndef _DARLINGSERVER_RPC_ERROR_REPLY_HPP_
#define _DARLINGSERVER_RPC_ERROR_REPLY_HPP_

namespace DarlingServer {
	template <typename ReplyHeader, typename CallHeader>
	inline ReplyHeader rpcErrorReplyHeaderFromCall(const CallHeader* header, int code) {
		ReplyHeader reply = {};
		reply.number = header->number;
		reply.code = code;
		return reply;
	}
}

#endif
