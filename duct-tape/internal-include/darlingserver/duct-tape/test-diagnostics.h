#pragma once

void dtape_test_trace_wait_timer(
	const char* event,
	unsigned long long thread,
	int wait_result,
	int had_timer,
	int active
);
void dtape_test_trace_exception_reply_wait(
	unsigned long long thread,
	int bounded,
	unsigned int timeout
);
