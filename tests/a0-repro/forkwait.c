// A0 fork/wait4 repro — targets the ACTUAL brew hang primitive (NOT cond_wait).
// Brew's hang (captured 2026-07-03): ruby has a ZOMBIE child and is parked forever in
// __skb_wait_for_more_packets waiting a darlingserver child-exit/wait RPC reply that never
// arrives, under the fork/exec/exit + SIGCHLD churn. This reproduces that: a parent rapidly
// forks short-lived children and waitpid()s them, while (optionally) a signal storm and
// concurrent forkers hammer the server's SIGCHLD/wait path.
//
// Knobs:
//   -DNFORKERS=n   number of concurrent forker threads (default 4)
//   -DNO_STORM     disable the SIGUSR1 storm
//   -DEXEC_CHILD   child execs /usr/bin/true instead of _exit (heavier, closer to brew)
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

#ifndef NFORKERS
#define NFORKERS 4
#endif

static volatile int   stop = 0;
static volatile long  reaped = 0;
static volatile int   storm_hits = 0;
static pthread_t      forkers[NFORKERS];

static void handler(int sig) { (void)sig; storm_hits++; }

static void* forker(void* arg) {
	(void)arg;
	while (!stop) {
		pid_t pid = fork();
		if (pid == 0) {
#ifdef EXEC_CHILD
			execl("/usr/bin/true", "true", (char*)NULL);
			_exit(127);
#else
			_exit(0);
#endif
		} else if (pid > 0) {
			int st;
			// waitpid may be interrupted by the storm; retry on EINTR (like libc/brew do)
			while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
			__atomic_fetch_add(&reaped, 1, __ATOMIC_RELAXED);
		} else {
			// fork failed (ENOMEM/EAGAIN under pressure); back off briefly
			usleep(200);
		}
	}
	return NULL;
}

#ifndef NO_STORM
static void* stormer(void* arg) {
	(void)arg;
	while (!stop)
		for (int i = 0; i < NFORKERS; i++)
			pthread_kill(forkers[i], SIGUSR1);
	return NULL;
}
#endif

int main(int argc, char** argv) {
	int secs = (argc > 1) ? atoi(argv[1]) : 20;
	struct sigaction sa; memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler; sigemptyset(&sa.sa_mask); sa.sa_flags = 0; // no SA_RESTART: waits get EINTR
	sigaction(SIGUSR1, &sa, NULL);

	pthread_t storm; (void)storm;
	for (long i = 0; i < NFORKERS; i++) pthread_create(&forkers[i], NULL, forker, (void*)i);
#ifndef NO_STORM
	pthread_create(&storm, NULL, stormer, NULL);
#endif

	const int STALL_LIMIT = 5;
	long last = -1; int stall = 0, hung = 0;
	for (int t = 0; t < secs; t++) {
		sleep(1);
		long cur = __atomic_load_n(&reaped, __ATOMIC_RELAXED);
		stall = (cur == last) ? stall + 1 : 0; last = cur;
		printf("[t=%2ds] reaped=%ld storm_hits=%d stall=%ds\n", t+1, cur, storm_hits, stall);
		fflush(stdout);
		if (stall >= STALL_LIMIT) { printf("HANG reaped=%ld\n", cur); fflush(stdout); hung = 1; break; }
	}
	stop = 1;
	// let forkers drain
	for (int t = 0; t < 3; t++) { usleep(200000); }
	printf(hung ? "RESULT=HANG reaped=%ld\n" : "RESULT=OK reaped=%ld\n", reaped);
	return hung;
}
