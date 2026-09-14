#include <darlingserver/duct-tape/test-diagnostics.h>

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
