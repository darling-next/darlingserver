/*
 * dar-pot RED->GREEN gate: darlingserver<->launchd<->guest LIFETIME BINDING.
 *
 * dar-pot ("orphaned mldr guests leak across sessions and destabilize fresh
 * boots"): after darlingserver dies (crash / SIGKILL / parent-exit), its guest
 * processes (mldr) survive orphaned, and accumulate until a fresh boot wedges
 * ("semaphore_timedwait -111", half-set-up root-owned workdir).
 *
 * The ROOT topology (see darlingserver.cpp ~line 693): darlingserver runs in the
 * HOST pid ns and creates the guest pid namespace ONCE with
 * clone(CLONE_NEWPID | SIGCHLD) -- that clone child is launchd, PID 1 of the new
 * ns. Every guest (mldr) is then a descendant of launchd, living INSIDE that ns.
 *
 * The kernel guarantee we lean on: when the PID-1 of a pid namespace dies, the
 * kernel SIGKILLs every remaining process in that namespace. So killing launchd
 * cascade-kills all guests. BUT: darlingserver is launchd's PARENT (via clone),
 * OUTSIDE the ns. If darlingserver dies while launchd lives, the ns persists and
 * every guest keeps running -- reparented, invisible, orphaned. That is dar-pot.
 *
 * The architecturally-correct fix binds the two lifetimes: the launchd clone
 * child sets prctl(PR_SET_PDEATHSIG, SIGKILL) so that when darlingserver (its
 * parent) dies FOR ANY REASON, launchd receives SIGKILL; being ns-PID-1, its
 * death then cascade-kills every guest. No orphans can survive their server.
 *
 * This gate pins that predicate hermetically -- no boot, no sockets. It models
 * the exact three-level topology:
 *
 *     driver (this process, host ns, plays the role of "the system / init")
 *       └── server  (fork; plays darlingserver -- lives in host ns)
 *             └── launchd (clone CLONE_NEWPID; PID 1 of guest ns)
 *                   └── guest (fork; plays an mldr guest, sleeps "forever")
 *
 * The driver learns the guest's HOST-side pid (published via shared mem), then
 * kills the SERVER and checks, after a grace period, whether the guest is still
 * alive.
 *
 *   RED  arm ("nobind"): launchd does NOT set PDEATHSIG. Killing the server
 *        leaves launchd (and thus the whole ns, incl. the guest) alive. The
 *        guest SURVIVES -- this is the shipped dar-pot bug. The arm asserts the
 *        orphan survives; if it ever DIED, the premise would be wrong and the
 *        gate blind, so "nobind" must report SURVIVED (driver exit 0 for RED
 *        polarity is handled by the harness).
 *
 *   GREEN arm ("bind"): launchd sets prctl(PR_SET_PDEATHSIG, SIGKILL) right
 *        after the clone, BEFORE forking the guest. Killing the server delivers
 *        SIGKILL to launchd; ns-PID-1 death cascade-kills the guest. The guest
 *        is REAPED. The arm asserts the orphan is gone.
 *
 * HOST test (plain glibc; needs pid-ns creation -- run as root or inside a user
 * ns, exactly like spawn_helper_pidns_placement.c). Exit 0 on the arm's
 * expectation being met.
 *
 *   usage: pot_lifetime_binding_test {nobind|bind}
 *     nobind -> expected GUEST-SURVIVED (proves the bug is real / arm not blind)
 *     bind   -> expected GUEST-REAPED   (proves PDEATHSIG closes it)
 */
#define _GNU_SOURCE 1
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <time.h>

/* shared across all three levels: the guest publishes its HOST-visible pid here
 * so the driver (which is NOT in the guest ns) can probe it from outside. */
struct shared {
	volatile pid_t guest_host_pid; /* guest's pid as seen from the host ns */
	volatile int   guest_ready;    /* guest has published its pid + is sleeping */
	volatile int   bind_pdeathsig; /* 1 => launchd sets PR_SET_PDEATHSIG(SIGKILL) */
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

/* Is `pid` (a HOST-ns pid) still a live, non-reaped process? We can't waitpid a
 * grandchild we don't own, so probe with kill(pid, 0): 0 or EPERM => alive,
 * ESRCH => gone. */
static int host_pid_alive(pid_t pid) {
	if (pid <= 0)
		return 0;
	if (kill(pid, 0) == 0)
		return 1;
	return (errno == EPERM) ? 1 : 0;
}

static void sleep_ms(long ms) {
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* trivial clone child for the pid-ns preflight probe */
static int preflight_child(void* arg) {
	(void) arg;
	_exit(0);
}

/* Detach std{in,out,err} to /dev/null. The server/launchd/guest subtree must not
 * keep the driver's stdout pipe open: in the RED arm those processes survive the
 * server's death, and if they held our stdout, a caller's command-substitution
 * ($(...)) would block on the pipe forever even after the driver exits. mldr
 * guests likewise do not share the launcher's stdio, so this is faithful. */
static void detach_stdio(void) {
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) {
		dup2(nul, 0);
		dup2(nul, 1);
		dup2(nul, 2);
		if (nul > 2)
			close(nul);
	}
}

/* PID-1 of the stand-in guest namespace: plays launchd. */
static int launchd_init(void* arg) {
	struct shared* s = (struct shared*) arg;

	/* THE FIX under test: bind our lifetime to the server (our parent). When the
	 * server dies for any reason, we (ns PID 1) get SIGKILL; our death then
	 * cascade-kills the whole guest ns. Set it BEFORE spawning the guest so there
	 * is no window in which a guest exists but the binding does not. */
	if (s->bind_pdeathsig) {
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
		/* NB: getppid() cannot detect a clone->here parent-death race here --
		 * across the PID-ns boundary ns-PID-1 always sees getppid()==0. The real
		 * darlingserver relies on its childWaitFDs handshake (armed after this
		 * prctl) to cover that window; this model arms pdeathsig and proceeds. */
	}

	/* fork the guest (an mldr stand-in): a member of the guest ns that publishes
	 * its host-side pid and then sleeps "forever" (until cascade-killed). */
	pid_t g = fork();
	if (g == 0) {
		/* guest: sleep effectively forever; the ONLY ways out are (a) cascade
		 * kill when launchd/ns-pid-1 dies [GREEN], or (b) never [RED]. */
		for (;;)
			pause();
		_exit(0); /* unreachable */
	}
	if (g < 0)
		_exit(4);

	/* publish the guest's pid as the HOST sees it. Inside the new pid ns the
	 * guest's local pid differs from its host pid; the driver needs the HOST pid.
	 * We (launchd) are ourselves inside the ns, so `g` here is the ns-local pid,
	 * NOT the host pid. Read the host pid off the guest's NStgid line, whose LAST
	 * field is host-visible... but we can't see the host ns either. Instead the
	 * guest itself cannot know its host pid. So the DRIVER discovers it: launchd
	 * is the server's only child; the guest is launchd's only child in the host's
	 * /proc. We publish a readiness flag and let the driver walk the tree. */
	s->guest_ready = 1;

	/* stay alive (keep the ns open) until we are killed. As ns-PID-1, reap the
	 * guest if it ever exits so we model launchd faithfully. */
	for (;;) {
		int st;
		pid_t r = waitpid(-1, &st, WNOHANG);
		(void) r;
		pause();
	}
	_exit(0);
}

/* From the host ns, find the guest: it is the sole grandchild under `server_pid`
 * -> launchd -> guest, discovered via /proc/<pid>/task/<pid>/children. Returns
 * the guest's HOST pid, or 0 if not found. */
static pid_t discover_guest_host_pid(pid_t server_pid) {
	char path[128];
	FILE* f;
	pid_t launchd_pid = 0, guest_pid = 0;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", server_pid, server_pid);
	f = fopen(path, "r");
	if (!f || fscanf(f, "%d", &launchd_pid) != 1) {
		if (f) fclose(f);
		return 0;
	}
	fclose(f);
	if (launchd_pid <= 0)
		return 0;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", launchd_pid, launchd_pid);
	f = fopen(path, "r");
	if (!f || fscanf(f, "%d", &guest_pid) != 1) {
		if (f) fclose(f);
		return 0;
	}
	fclose(f);
	return guest_pid;
}

/* the "server" (darlingserver stand-in): clones launchd into a new pid ns, then
 * just waits to be killed by the driver. Runs in the host ns. */
static void run_server(struct shared* s) {
	/* the server and its whole subtree (launchd, guest) must not hold the
	 * driver's stdout pipe -- see detach_stdio(). */
	detach_stdio();

	size_t stack_size = 256 * 1024;
	char* stack = malloc(stack_size);
	if (!stack)
		_exit(5);

	pid_t launchd = clone(launchd_init, stack + stack_size,
		CLONE_NEWPID | SIGCHLD, s);
	if (launchd < 0)
		_exit(6);

	/* server just idles; the driver will SIGKILL the whole server subtree by
	 * killing THIS process. (In production darlingserver dying is the trigger.) */
	for (;;)
		pause();
	_exit(0);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s {nobind|bind}\n", argv[0]);
		return 2;
	}
	int bind = (strcmp(argv[1], "bind") == 0);
	int nobind = (strcmp(argv[1], "nobind") == 0);
	if (!bind && !nobind) {
		fprintf(stderr, "usage: %s {nobind|bind}\n", argv[0]);
		return 2;
	}

	struct shared* s = shm_setup();
	s->bind_pdeathsig = bind ? 1 : 0;

	/* Preflight: pid-ns creation must be available, else SKIP (exit 77) so the
	 * harness can tell "environment can't run this" from a real failure. Use a
	 * REAL trivial child fn -- clone() with a NULL fn returns EINVAL and would
	 * mask the true permission errno. */
	{
		size_t ss = 64 * 1024;
		char* st = malloc(ss);
		errno = 0;
		pid_t probe = clone(preflight_child, st + ss, CLONE_NEWPID | SIGCHLD, NULL);
		int e = errno;
		if (probe < 0) {
			fprintf(stderr, "SKIP: cannot create pid namespace (errno=%d %s); run as root or in a user ns\n",
				e, strerror(e));
			free(st);
			return 77;
		}
		{ int w; waitpid(probe, &w, 0); }
		free(st);
	}

	pid_t server = fork();
	if (server == 0) {
		run_server(s);
		_exit(0); /* unreachable */
	}
	if (server < 0) {
		perror("fork server");
		return 3;
	}

	/* wait for the guest to come up inside the ns */
	for (int i = 0; i < 500 && !s->guest_ready; ++i)
		sleep_ms(10);
	if (!s->guest_ready) {
		fprintf(stderr, "guest never became ready\n");
		kill(server, SIGKILL);
		return 4;
	}

	/* discover the guest's HOST pid by walking server -> launchd -> guest */
	pid_t guest_host_pid = 0;
	for (int i = 0; i < 200 && guest_host_pid == 0; ++i) {
		guest_host_pid = discover_guest_host_pid(server);
		if (!guest_host_pid) sleep_ms(10);
	}
	if (!guest_host_pid) {
		fprintf(stderr, "could not discover guest host pid\n");
		kill(server, SIGKILL);
		return 4;
	}

	/* sanity: the guest is alive right now (before we kill the server) */
	if (!host_pid_alive(guest_host_pid)) {
		fprintf(stderr, "guest not alive pre-kill -- setup failed\n");
		kill(server, SIGKILL);
		return 4;
	}

	/* THE EVENT: darlingserver dies. We SIGKILL the server (models a crash /
	 * external kill / the launcher exiting). launchd is NOT killed by us -- its
	 * fate depends ENTIRELY on whether it bound itself via PDEATHSIG. */
	kill(server, SIGKILL);
	{ int w; waitpid(server, &w, 0); } /* reap the server we own */

	/* grace period for PDEATHSIG delivery + ns cascade-kill to complete */
	int guest_alive = 1;
	for (int i = 0; i < 200; ++i) { /* up to ~2s */
		if (!host_pid_alive(guest_host_pid)) { guest_alive = 0; break; }
		sleep_ms(10);
	}

	printf("arm=%s guest_host_pid=%d result=%s\n",
		bind ? "bind" : "nobind",
		guest_host_pid,
		guest_alive ? "GUEST-SURVIVED" : "GUEST-REAPED");

	/* cleanup: in the RED arm the orphaned launchd (ns PID 1) + its guest survive
	 * by design -- but the TEST must not leak them. Killing this driver's launchd
	 * (which reparented to us when the server died) cascade-kills the whole guest
	 * ns. Sweep every child of ours that is a lingering launchd, then belt-and-
	 * suspenders kill the guest by host pid too. */
	if (guest_alive) {
		/* launchd (ns PID 1) is the guest's parent; when the server died launchd
		 * reparented to init, NOT to us, so we can't find it via our own children.
		 * Read it off the guest's /proc/<pid>/stat (field 4 = ppid) and kill it --
		 * ns-PID-1 death cascade-kills the guest. Belt-and-suspenders: kill the
		 * guest directly too. */
		char path[128];
		snprintf(path, sizeof(path), "/proc/%d/stat", (int) guest_host_pid);
		FILE* sf = fopen(path, "r");
		if (sf) {
			int gpid; char comm[256]; char st_c; int launchd_pid = 0;
			/* pid (comm) state ppid ...  -- comm can contain spaces/parens, so read
			 * up to the closing paren then the state char + ppid. */
			if (fscanf(sf, "%d %255s %c %d", &gpid, comm, &st_c, &launchd_pid) == 4
			    && launchd_pid > 1) {
				kill(launchd_pid, SIGKILL);
			}
			fclose(sf);
		}
		kill(guest_host_pid, SIGKILL);
	}

	if (bind) {
		/* GREEN expectation: PDEATHSIG bound the lifetimes -> guest reaped */
		return guest_alive ? 1 : 0;
	} else {
		/* RED expectation: no binding -> guest SURVIVES (bug reproduces). If it
		 * died, the model is wrong / the arm is blind -> fail. */
		return guest_alive ? 0 : 1;
	}
}
