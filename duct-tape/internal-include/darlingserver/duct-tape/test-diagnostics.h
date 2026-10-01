#pragma once

int dtape_test_consume_fault(const char* name);
void dtape_test_ring_record(unsigned long long tag, unsigned long long a, unsigned long long b, unsigned long long c);
void dtape_test_ring_record4(unsigned long long tag, unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d);
void dtape_test_ring_dump(void);

void dtape_test_trace_line(const char* line);
void dtape_test_trace_wait_timer(
	const char* event,
	unsigned long long thread,
	int wait_result,
	int had_timer,
	int active
);
void dtape_test_trace_msgq(
	const char* event,
	unsigned long long thread,
	unsigned long long mqueue,
	unsigned long long port_name,
	unsigned int bits,
	int result
);

void dtape_test_trace_exception_reply_wait(
	unsigned long long thread,
	int bounded,
	unsigned int timeout
);
