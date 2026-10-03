#ifndef _DARLINGSERVER_FORK_CHECKIN_HPP_
#define _DARLINGSERVER_FORK_CHECKIN_HPP_

#include <atomic>

#include <darlingserver/duct-tape.h>

namespace DarlingServer {
	enum class ForkCheckinWaitResult {
		Observed,
		Interrupted,
		TimedOut,
		Error,
	};

	constexpr unsigned int ForkCheckinWaitTimeoutSeconds = 30;

	inline ForkCheckinWaitResult waitForForkCheckin(dtape_semaphore_t* semaphore, unsigned int timeoutSeconds = ForkCheckinWaitTimeoutSeconds) {
		switch (dtape_semaphore_down_timeout(semaphore, timeoutSeconds)) {
			case dtape_semaphore_wait_result_ok:
				return ForkCheckinWaitResult::Observed;
			case dtape_semaphore_wait_result_interrupted:
				return ForkCheckinWaitResult::Interrupted;
			case dtape_semaphore_wait_result_timed_out:
				return ForkCheckinWaitResult::TimedOut;
			default:
				return ForkCheckinWaitResult::Error;
		}
	}

	class ForkCheckinState {
	public:
		void reset() {
			_childCheckedIn.store(false);
		}

		void markChildCheckedIn() {
			_childCheckedIn.store(true);
		}

		ForkCheckinWaitResult wait(dtape_semaphore_t* semaphore, unsigned int timeoutSeconds = ForkCheckinWaitTimeoutSeconds) {
			if (consumeSticky()) {
				(void)dtape_semaphore_down_timeout(semaphore, 0);
				return ForkCheckinWaitResult::Observed;
			}

			auto result = waitForForkCheckin(semaphore, timeoutSeconds);
			if (result == ForkCheckinWaitResult::Observed) {
				reset();
				return result;
			}
			if (resultCanRaceWithStickyCheckin(result) && consumeSticky()) {
				return ForkCheckinWaitResult::Observed;
			}
			return result;
		}

	private:
		static bool resultCanRaceWithStickyCheckin(ForkCheckinWaitResult result) {
			return result == ForkCheckinWaitResult::Interrupted || result == ForkCheckinWaitResult::TimedOut;
		}

		bool consumeSticky() {
			return _childCheckedIn.exchange(false);
		}

		std::atomic<bool> _childCheckedIn{false};
	};
}

#endif
