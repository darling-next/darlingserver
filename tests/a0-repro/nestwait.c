// A0 nested fork+exec+wait+pipe repro — mirrors brew's real churn shape (sh->make->cc|...,
// ruby->curl) that produces the captured hang: a parent parked in __skb_wait_for_more_packets
// with a ZOMBIE child whose exit/wait4 reply was lost.
//
// Structure per "job": the worker forks a CHILD shell-like process; that child sets up a
// 2-stage PIPELINE (fork two grandchildren connected by a pipe, both exec /bin/echo|/usr/bin/true),
// waits for both grandchildren, then _exits; the worker wait4()s the child. Many worker threads
// run jobs concurrently. This nests fork+exec+wait 2 deep with pipes, like make->sh->(cmd|cmd).
//
// Knobs:
//   -DNWORKERS=n   concurrent worker threads (default 6)
//   -DNO_STORM     disable SIGCHLD-ish SIGUSR1 storm on the workers
//   -DDEPTH1       only 1-level nesting (worker->child->exec, no pipeline) for A/B
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>

#ifndef NWORKERS
#define NWORKERS 6
#endif

static volatile int  stop = 0;
static volatile long jobs = 0;
static volatile int  storm_hits = 0;
static pthread_t     workers[NWORKERS];

static void handler(int sig) { (void)sig; storm_hits++; }

static int reap(pid_t pid) {
	int st;
	while (waitpid(pid, &st, 0) < 0) {
		if (errno == EINTR) continue;   // storm-interrupted wait: retry (like libc/sh)
		return -1;
	}
	return 0;
}

// child process: a mini shell that runs `true | true` via a pipe (or just execs, if DEPTH1)
static void child_body(void) {
#ifdef DEPTH1
	execl("/usr/bin/true", "true", (char*)NULL);
	_exit(127);
#else
	int pfd[2];
	if (pipe(pfd) < 0) _exit(70);
	pid_t a = fork();
	if (a == 0) {
		// left of pipe: stdout -> pipe
		dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
		execl("/bin/echo", "echo", "x", (char*)NULL);
		_exit(127);
	}
	pid_t b = fork();
	if (b == 0) {
		// right of pipe: stdin <- pipe
		dup2(pfd[0], 0); close(pfd[0]); close(pfd[1]);
		execl("/usr/bin/true", "true", (char*)NULL);
		_exit(127);
	}
	close(pfd[0]); close(pfd[1]);
	if (a > 0) reap(a);
	if (b > 0) reap(b);
	_exit(0);
#endif
}

static void* worker(void* arg) {
	(void)arg;
	while (!stop) {
		pid_t pid = fork();
		if (pid == 0) {
			child_body();
			_exit(0);
		} else if (pid > 0) {
			if (reap(pid) == 0) __atomic_fetch_add(&jobs, 1, __ATOMIC_RELAXED);
		} else {
			usleep(300);
		}
	}
	return NULL;
}

#ifndef NO_STORM
static void* stormer(void* arg) {
	(void)arg;
	while (!stop)
		for (int i = 0; i < NWORKERS; i++)
			pthread_kill(workers[i], SIGUSR1);
	return NULL;
}
#endif

int main(int argc, char** argv) {
	int secs = (argc > 1) ? atoi(argv[1]) : 20;
	struct sigaction sa; memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	pthread_t storm; (void)storm;
	for (long i = 0; i < NWORKERS; i++) pthread_create(&workers[i], NULL, worker, (void*)i);
#ifndef NO_STORM
	pthread_create(&storm, NULL, stormer, NULL);
#endif

	const int STALL_LIMIT = 5;
	long last = -1; int stall = 0, hung = 0;
	for (int t = 0; t < secs; t++) {
		sleep(1);
		long cur = __atomic_load_n(&jobs, __ATOMIC_RELAXED);
		stall = (cur == last) ? stall + 1 : 0; last = cur;
		printf("[t=%2ds] jobs=%ld storm_hits=%d stall=%ds\n", t+1, cur, storm_hits, stall);
		fflush(stdout);
		if (stall >= STALL_LIMIT) {
			printf("HANG jobs=%ld\n", cur); fflush(stdout); hung = 1;
#ifdef HOLD_ON_HANG
			// keep the hung state alive for live capture (host inspects /proc + server auxlog)
			for (;;) sleep(60);
#endif
			break;
		}
	}
	stop = 1;
	for (int t = 0; t < 3; t++) usleep(200000);
	printf(hung ? "RESULT=HANG jobs=%ld\n" : "RESULT=OK jobs=%ld\n", jobs);
	return hung;
}
