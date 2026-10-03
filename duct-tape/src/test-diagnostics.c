#include <darlingserver/duct-tape/test-diagnostics.h>

#include <stdlib.h>

extern char* getenv(const char* name);
extern int open(const char* pathname, int flags, ...);
extern long write(int fd, const void* buf, unsigned long count);
extern long read(int fd, void* buf, unsigned long count);
extern int close(int fd);
extern int unlink(const char* pathname);
extern int snprintf(char* str, unsigned long size, const char* format, ...);

#define DSERVER_TEST_TRACE_FILE "DSERVER_TEST_TRACE_FILE"
#define DSERVER_TEST_FAULT_FILE "DSERVER_TEST_FAULT_FILE"
#define DSERVER_O_RDONLY 00000000
#define DSERVER_O_WRONLY 00000001
#define DSERVER_O_CREAT 00000100
#define DSERVER_O_APPEND 00002000
#define DSERVER_O_CLOEXEC 02000000

static unsigned long dtape_test_trace_strlen(const char* string) {
	unsigned long result = 0;
	while (string && string[result] != '\0') {
		++result;
	}
	return result;
}

// perf#30 (doc section 241): THE EVENT RING. MEASURED need: the line-per-event trace writes to a file for every
// send, receive and post, and that I/O is enough to move the very race the trace exists to observe (`basic 20` hangs in
// three runs of three without it while `basic 2` passes with it). A race can only be judged from a run that is not
// slowed by its own instrument, so the hot events go into a fixed memory ring -- no syscall, no allocation, no lock --
// and are written out only when something else asks for them (the server's stall watchdog, or the end of a run).
#define DTA_ERING_CAP 2048u
typedef struct {
	unsigned long long seq;
	unsigned long long tag;
	unsigned long long a;
	unsigned long long b;
	unsigned long long c;
	unsigned long long d;
} dta_ering_record;
static dta_ering_record dta_ering[DTA_ERING_CAP];
static unsigned long long dta_ering_reserved;
static int dta_ering_enabled = -1;

static int dta_ering_on(void) {
	if (dta_ering_enabled < 0) {
		const char* p = getenv("DSERVER_TRACE_RING");
		dta_ering_enabled = (p && p[0] == '1') ? 1 : 0;
	}
	return dta_ering_enabled;
}

void dtape_test_ring_record(unsigned long long tag, unsigned long long a, unsigned long long b, unsigned long long c) {
	dtape_test_ring_record4(tag, a, b, c, 0);
}

void dtape_test_ring_record4(unsigned long long tag, unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d) {
	if (!dta_ering_on()) {
		return;
	}
	// One atomic reservation per event and a plain store into the reserved slot: a torn record is possible under
	// concurrency and is accepted, because dropping a diagnostic event must never block the path it observes.
	unsigned long long seq = __atomic_fetch_add(&dta_ering_reserved, 1, __ATOMIC_RELAXED);
	dta_ering_record* r = &dta_ering[seq & (DTA_ERING_CAP - 1)];
	r->seq = seq;
	r->tag = tag;
	r->a = a;
	r->b = b;
	r->c = c;
	r->d = d;
}

// perf#30 (doc section 249): the dump writes to fd 2 itself. MEASURED: it went through `dtape_test_trace_line`, which
// the LINE TRACE controls, so a run with the ring on (`DSERVER_TRACE_RING=1`) and no trace file -- exactly the
// combination the ring exists for, since the line trace perturbs the hot path -- produced a stall dump with NO ring
// records at all. The instrument was on and the output was invisible, which is the same failure as being off.
// The XNU-flavoured translation unit cannot include the host <unistd.h>, so the one call this needs is declared here.
extern long write(int fd, const void* buf, unsigned long count);

static void dta_ering_dump_line(const char* line) {
	unsigned long len = 0;
	while (line[len] != '\0') { len++; }
	(void)!write(2, line, len);
	dtape_test_trace_line(line);
}

void dtape_test_ring_dump(void) {
	unsigned long long reserved = __atomic_load_n(&dta_ering_reserved, __ATOMIC_ACQUIRE);
	unsigned long long count = (reserved < (unsigned long long)DTA_ERING_CAP) ? reserved : (unsigned long long)DTA_ERING_CAP;
	unsigned long long first = reserved - count;
	char line[192];
	snprintf(line, sizeof(line), "dtape.ering dump events=%llu shown=%llu\n", reserved, count);
	dta_ering_dump_line(line);
	for (unsigned long long i = first; i < reserved; ++i) {
		dta_ering_record* r = &dta_ering[i & (DTA_ERING_CAP - 1)];
		if (r->seq != i) {
			continue; // overwritten or partly written while we read: reported by omission, never guessed
		}
		const char* tag = "?";
		switch (r->tag) {
			case 1: tag = "recv_park"; break;
			case 2: tag = "recv_done"; break;
			case 3: tag = "send_dest"; break;
			case 4: tag = "post_wake"; break;
			case 5: tag = "recv_object"; break;
			case 6: tag = "recv_woken"; break;
			// perf#30 (doc section 250): the TIMER path, recorded into the same ring. The question that needed it:
			// a semaphore timed wait with a 30-second deadline began and never received a reply while the server was
			// alive and idle. The deadline is armed by the hook below and fired by the server loop reading the
			// timerfd, so "armed but never fired" is the difference between "the loop never saw the timerfd" and
			// "the timer fired and the wake was lost" -- and nothing could tell them apart.
			case 7: tag = "timer_arm"; break;
			case 8: tag = "timer_fired"; break;
			default: break;
		}
		snprintf(line, sizeof(line), "dtape.ering seq=%llu tag=%s a=0x%llx b=0x%llx c=0x%llx d=0x%llx\n",
			r->seq, tag, r->a, r->b, r->c, r->d);
		dta_ering_dump_line(line);
	}
}

void dtape_test_trace_line(const char* line) {
	const char* path = getenv(DSERVER_TEST_TRACE_FILE);
	if (!path || path[0] == '\0') {
		return;
	}

	int fd = open(path, DSERVER_O_WRONLY | DSERVER_O_CREAT | DSERVER_O_APPEND | DSERVER_O_CLOEXEC, 0666);
	if (fd < 0) {
		return;
	}

	write(fd, line, dtape_test_trace_strlen(line));
	write(fd, "\n", 1);
	close(fd);
}

static int dtape_test_trace_streq(const char* left, const char* right) {
	unsigned long index = 0;
	if (!left || !right) {
		return 0;
	}
	while (left[index] != '\0' && right[index] != '\0') {
		if (left[index] != right[index]) {
			return 0;
		}
		++index;
	}
	return left[index] == '\0' && right[index] == '\0';
}

int dtape_test_consume_fault(const char* name) {
	const char* path = getenv(DSERVER_TEST_FAULT_FILE);
	if (!path || path[0] == '\0' || !name || name[0] == '\0') {
		return 0;
	}

	int fd = open(path, DSERVER_O_RDONLY | DSERVER_O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}

	char buffer[128] = {};
	long count = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (count <= 0) {
		return 0;
	}
	while (count > 0 && (
	    buffer[count - 1] == '\n' ||
	    buffer[count - 1] == '\r' ||
	    buffer[count - 1] == ' ' ||
	    buffer[count - 1] == '\t')) {
		buffer[--count] = '\0';
	}
	if (!dtape_test_trace_streq(buffer, name)) {
		return 0;
	}

	unlink(path);
	char line[160];
	snprintf(line, sizeof(line), "test_fault.consume name=%s", name);
	dtape_test_trace_line(line);
	return 1;
}

void dtape_test_trace_wait_timer(
	const char* event,
	unsigned long long thread,
	int wait_result,
	int had_timer,
	int active
) {
	char line[192];
	snprintf(line, sizeof(line),
		"dtape.wait_timer event=%s thread=0x%llx wait_result=%d had_timer=%d active=%d",
		event,
		thread,
		wait_result,
		had_timer,
		active
	);
	dtape_test_trace_line(line);
}

// perf#30 (doc section 236): the mach_msg send/receive pair, traced at the trap level. MEASURED need: the stall dump
// showed a thread parked in mach_msg_overwrite with a continuation while the only other parked guest thread sat in a
// semaphore wait that times out and retries, and two ring publishes had no matching consume -- i.e. one message
// exchange whose two halves never met. `mqueue` and `port_name` are the two things that decide it: the receive parks on
// ONE mqueue and a send must arrive at THAT one. Addresses of different objects are printed as-is on purpose -- the
// question is equality, and only the run can answer it.
void dtape_test_trace_msgq(
	const char* event,
	unsigned long long thread,
	unsigned long long mqueue,
	unsigned long long port_name,
	unsigned int bits,
	int result
) {
	// The hot path never writes a file when the ring is on: the SAME call sites feed either the ring or the line
	// trace, so the two modes cannot drift and a stalled run can be replayed from order rather than from a summary.
	if (dta_ering_on()) {
		// Explicit string comparison, not character positions: MEASURED in the first version of this classifier, a
		// positional test is one rename away from silently labelling every event with the wrong tag, and a ring whose
		// tags are wrong is worse than no ring. `bits` and `result` are carried in ONE extra field, because the
		// post's "found a waiter" flag is exactly the bit this instrument exists to preserve.
		unsigned long long tag = 0;
		if (dtape_test_trace_streq(event, "trap_recv_park")) {
			tag = 1;
		} else if (dtape_test_trace_streq(event, "trap_recv_done")) {
			tag = 2;
		} else if (dtape_test_trace_streq(event, "kmsg_send_dest")) {
			tag = 3;
		} else if (dtape_test_trace_streq(event, "post_wake")) {
			tag = 4;
		} else if (dtape_test_trace_streq(event, "trap_recv_object")) {
			tag = 5;
		} else if (dtape_test_trace_streq(event, "recv_woken")) {
			tag = 6;
		}
		unsigned long long packed = ((unsigned long long)(unsigned int)bits << 32)
			| (unsigned long long)(unsigned int)result;
		dtape_test_ring_record4(tag, thread, mqueue, port_name, packed);
		return;
	}
	char line[256];
	snprintf(line, sizeof(line),
		// perf#30 (doc section 244): the fourth field is NOT a port name. Two call sites pass a PORT OBJECT pointer
		// (`kmsg_send_dest`) and one passes a RECEIVER THREAD pointer (`post_wake`), which is exactly what the matching
		// comparisons need -- but a field called `port_name` that holds an address is read as a name, and it was, during
		// this very investigation: a 13-digit value looked like a corrupt name instead of the pointer it is. Print it as
		// an address under the name it actually has.
		"dtape.msgq event=%s thread=0x%llx mqueue=0x%llx arg=0x%llx bits=0x%x result=%d",
		event,
		thread,
		mqueue,
		port_name,
		bits,
		result
	);
	dtape_test_trace_line(line);
}

void dtape_test_trace_exception_reply_wait(
	unsigned long long thread,
	int bounded,
	unsigned int timeout
) {
	char line[160];
	snprintf(line, sizeof(line),
		"dtape.exception_reply_wait thread=0x%llx bounded=%d timeout=%u",
		thread,
		bounded,
		timeout
	);
	dtape_test_trace_line(line);
}
