/*
 * M1 spike gate for dar-dar6x4-perf-5dq.23 (server-side posix_spawn).
 *
 * The .23 perf win requires darlingserver to create a NEW guest process that
 * lands INSIDE the guest PID namespace (so it is born already-registerable and
 * skips the cross-process clone+checkin scheduling chain). But darlingserver
 * runs in the PARENT/host PID namespace -- it created the guest PID ns once at
 * boot via clone(CLONE_NEWPID) and then stayed outside it. So a naive
 * fork()+exec() from the server lands the child in the HOST ns: wrong namespace,
 * wrong getpid(), invisible to the guest.
 *
 * This gate pins the NAMESPACE PRIMITIVE the M1 spike-helper relies on, in
 * isolation (no booted guest needed):
 *
 *   - We create a stand-in "guest PID namespace" with clone(CLONE_NEWPID), just
 *     as darlingserver does for launchd. The clone child is PID 1 in that ns.
 *
 *   - RED arm ("outside"): a process forked from OUTSIDE that ns (a plain child
 *     of the test driver, mirroring a server worker fork) is NOT in the guest ns
 *     -- its /proc/self/ns/pid inode differs from the namespace's, and its
 *     NSpid line has a single entry. This is exactly why the current server
 *     cannot place a spawn into the guest. The RED arm asserts this mismatch;
 *     if it ever matched, the premise would be wrong and the test is blind.
 *
 *   - GREEN arm ("helper"): a process forked by a member of the guest ns (the
 *     spike-helper, which is itself a child of the PID-1 clone) IS in the guest
 *     ns -- same ns/pid inode, and a 2-level NSpid (host pid + ns-local pid).
 *     This is the placement the M1 helper must achieve.
 *
 * HOST test (plain glibc, needs user/PID-ns creation -- run unprivileged via a
 * user namespace, or as root). Exit 0 on PASS.
 *
 *   usage: spawn_helper_pidns_placement {outside|helper}
 *     outside -> expected to report NOT-IN-GUEST-NS  (exit 0 = correctly outside)
 *     helper  -> expected to report IN-GUEST-NS      (exit 0 = correctly inside)
 *   The run-*.sh harness runs both arms and checks the RED/GREEN polarity.
 */
#define _GNU_SOURCE 1
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* read the inode of /proc/<pid>/ns/pid (the PID-namespace identity) */
static unsigned long ns_pid_inode(const char* pid /* "self" or numeric */) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%s/ns/pid", pid);
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	return (unsigned long) st.st_ino;
}

/* count the entries on the NSpid: line of /proc/self/status.
 * 1 entry  => we are in a single (the outermost visible) pid ns.
 * 2 entries => we are nested one level deep (host pid + ns-local pid) = guest. */
static int nspid_depth(void) {
	FILE* f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;
	char line[512];
	int depth = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "NSpid:", 6) == 0) {
			depth = 0;
			for (char* p = line + 6; *p; ++p) {
				if (*p >= '0' && *p <= '9' && (p == line + 6 || p[-1] < '0' || p[-1] > '9'))
					depth++;
			}
			break;
		}
	}
	fclose(f);
	return depth;
}

/* shared between the clone child and us: where the child publishes its ns inode */
struct shared {
	unsigned long guest_ns_inode;
	volatile int  ready;
	volatile int  go;
	volatile int  child_result; /* 0 = in-guest-ns, 1 = not, set by the spawned grandchild via the helper */
	volatile int  child_done;
};

static struct shared* shm_setup(void) {
	struct shared* s = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (s == MAP_FAILED) {
		perror("mmap");
		exit(3);
	}
	memset((void*) s, 0, sizeof(*s));
	return s;
}

/* The PID-1 of the stand-in guest namespace.
 * In the GREEN arm it forks the "helper" grandchild (a member of the guest ns)
 * and records whether that grandchild is correctly in-ns. */
static int guest_init(void* arg) {
	struct shared* s = (struct shared*) arg;
	s->guest_ns_inode = ns_pid_inode("self");
	s->ready = 1;

	/* wait for the driver to tell us which arm */
	while (!s->go)
		usleep(200);

	if (s->go == 2 /* helper arm: fork a grandchild inside this ns */) {
		pid_t g = fork();
		if (g == 0) {
			/* grandchild: a member of the guest ns, like the spawn-helper's child */
			unsigned long mine = ns_pid_inode("self");
			int depth = nspid_depth();
			/* in-guest-ns iff our ns/pid inode matches the guest ns AND we're nested */
			s->child_result = (mine == s->guest_ns_inode && depth >= 2) ? 0 : 1;
			s->child_done = 1;
			_exit(0);
		}
		int st;
		waitpid(g, &st, 0);
	}
	/* keep the namespace alive until released */
	while (s->go != 9)
		usleep(200);
	_exit(0);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s {outside|helper}\n", argv[0]);
		return 2;
	}
	int helper_arm = (strcmp(argv[1], "helper") == 0);

	struct shared* s = shm_setup();

	/* create the stand-in guest PID namespace, exactly like darlingserver does
	 * for launchd: clone(CLONE_NEWPID). The clone child is PID 1 in that ns. */
	size_t stack_size = 256 * 1024;
	char* stack = malloc(stack_size);
	if (!stack) { perror("malloc"); return 3; }

	pid_t init_pid = clone(guest_init, stack + stack_size,
		CLONE_NEWPID | SIGCHLD, s);
	if (init_pid < 0) {
		fprintf(stderr, "clone(CLONE_NEWPID) failed: %s\n", strerror(errno));
		fprintf(stderr, "(this gate needs PID-ns creation: run as root or in a user ns)\n");
		return 3;
	}

	while (!s->ready)
		usleep(200);

	int in_guest_ns;

	if (helper_arm) {
		/* GREEN: the in-ns helper forks a grandchild; that grandchild must be in-ns */
		s->go = 2;
		while (!s->child_done)
			usleep(200);
		in_guest_ns = (s->child_result == 0);
	} else {
		/* RED: a child forked from OUTSIDE the guest ns (a plain child of this
		 * driver -- mirroring a server worker fork) must NOT be in the guest ns */
		s->go = 1;
		pid_t outsider = fork();
		if (outsider == 0) {
			unsigned long mine = ns_pid_inode("self");
			int depth = nspid_depth();
			int inside = (mine == s->guest_ns_inode && depth >= 2);
			_exit(inside ? 0 : 1); /* exit 1 => correctly NOT in guest ns */
		}
		int st;
		waitpid(outsider, &st, 0);
		in_guest_ns = (WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}

	/* release the namespace */
	s->go = 9;
	int st;
	waitpid(init_pid, &st, 0);

	printf("arm=%s guest_ns_inode=%lu result=%s\n",
		helper_arm ? "helper" : "outside",
		s->guest_ns_inode,
		in_guest_ns ? "IN-GUEST-NS" : "NOT-IN-GUEST-NS");

	if (helper_arm) {
		/* GREEN expectation: must be in guest ns */
		return in_guest_ns ? 0 : 1;
	} else {
		/* RED expectation: must NOT be in guest ns (proves the server can't just fork) */
		return in_guest_ns ? 1 : 0;
	}
}
