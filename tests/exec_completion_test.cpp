#include <darlingserver/exec-completion.hpp>
#include <darlingserver/utility.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <unistd.h>
#include <utility>

using namespace DarlingServer;
using Result = ExecCompletion::Result;

static void require(bool condition, const char* message)
{
	if (!condition) {
		std::cerr << "FAIL " << message << '\n';
		std::exit(1);
	}
}

struct ExecPipe {
	std::shared_ptr<FD> listener;
	std::unique_ptr<FD> writer;

	ExecPipe()
	{
		int descriptors[2];
		require(pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0,
			"create nonblocking exec pipe");
		listener = std::make_shared<FD>(descriptors[0]);
		writer = std::make_unique<FD>(descriptors[1]);
	}
};

static void checkinBeforeMonitor()
{
	ExecCompletion completion;
	ExecPipe pipe;
	completion.begin(pipe.listener, true);
	pipe.writer.reset();

	// Model only the caller's pending replacement and the thread consuming it.
	// Calls are serialized, as they are under Process::_rwlock.
	bool replacementPending = false;
	int retaskedThread = 0;
	auto publishCompletion = [&](Result result) {
		if (result == Result::Darling) {
			replacementPending = true;
		}
	};
	auto checkin = [&](int threadID) {
		publishCompletion(completion.poll());
		if (std::exchange(replacementPending, false)) {
			retaskedThread = threadID;
		}
	};

	checkin(101);
	require(retaskedThread == 101,
		"exec thread Checkin must consume completion before the monitor runs");
	publishCompletion(completion.poll(pipe.listener));
	checkin(202);
	require(retaskedThread == 101,
		"delayed monitor must not publish another replacement for a later thread");
}

static void monitorBeforeCheckin()
{
	ExecCompletion completion;
	ExecPipe pipe;
	completion.begin(pipe.listener, true);
	pipe.writer.reset();

	require(completion.poll(pipe.listener) == Result::Darling,
		"monitor must report successful Darling exec");
	require(completion.poll() == Result::None,
		"Checkin must not rediscover completion already consumed by monitor");
	require(completion.poll(pipe.listener) == Result::None,
		"repeated monitor callback must not report completion twice");
}

static void failureBeforeEOF()
{
	ExecCompletion completion;
	ExecPipe pipe;
	completion.begin(pipe.listener, true);
	const char failed = 1;
	require(write(pipe.writer->fd(), &failed, sizeof(failed)) == sizeof(failed),
		"write exec failure byte");

	require(completion.poll() == Result::Failed,
		"failure byte must report failed exec without waiting for EOF");
	pipe.writer.reset();
	require(completion.poll(pipe.listener) == Result::None,
		"EOF after failure must not publish successful exec from monitor");
	require(completion.poll() == Result::None,
		"EOF after failure must not publish a replacement at Checkin");
}

static void oldImageBeforeCompletion()
{
	ExecCompletion completion;
	ExecPipe pipe;
	completion.begin(pipe.listener, true);

	require(completion.poll() == Result::None,
		"old-image Checkin while writer is open must not consume exec attempt");
	require(completion.poll(pipe.listener) == Result::None,
		"monitor without data or EOF must leave exec attempt pending");
	pipe.writer.reset();
	require(completion.poll() == Result::Darling,
		"actual EOF must remain observable after old-image polling");
	require(completion.poll(pipe.listener) == Result::None,
		"monitor after actual completion must not duplicate replacement");
}

static void staleMonitorDuringNextExec()
{
	ExecCompletion completion;
	ExecPipe previous;
	completion.begin(previous.listener, true);
	previous.writer.reset();
	require(completion.poll() == Result::Darling,
		"first exec must complete before the next attempt");

	ExecPipe next;
	completion.begin(next.listener, false);
	require(completion.poll(previous.listener) == Result::None,
		"stale callback must not complete the next attempt while writer is open");
	require(completion.poll() == Result::None,
		"next attempt must remain pending after stale callback");
	next.writer.reset();
	require(completion.poll(previous.listener) == Result::None,
		"stale callback must not consume next attempt's ready EOF");
	require(completion.poll(next.listener) == Result::Native,
		"next monitor must retain Native completion after stale callback");
	require(completion.poll() == Result::None,
		"Native completion must also be consumed exactly once");
}

static void nativeCheckinBeforeMonitor()
{
	ExecCompletion completion;
	ExecPipe pipe;
	completion.begin(pipe.listener, false);
	pipe.writer.reset();

	require(completion.poll() == Result::Native,
		"Checkin must distinguish Native exec from a Darling replacement");
	require(completion.poll(pipe.listener) == Result::None,
		"delayed monitor must not duplicate Native completion");
}

int main()
{
	checkinBeforeMonitor();
	monitorBeforeCheckin();
	failureBeforeEOF();
	oldImageBeforeCompletion();
	staleMonitorDuringNextExec();
	nativeCheckinBeforeMonitor();
	std::cout << "DSERVER_EXEC_COMPLETION_CONTRACT_OK\n";
	return 0;
}
