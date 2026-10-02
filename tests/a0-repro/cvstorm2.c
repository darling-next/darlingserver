// A0 cvstorm variant harness. Compile-time knobs to bisect the hang:
//   -DNCONS=n        number of consumers (default 8)
//   -DNO_STORM       disable the pthread_kill signal storm
//   -DSA_RESTART_ON  set SA_RESTART on the storm signal (waits auto-resume)
//   -DNO_BCAST       producer never broadcasts (signal-only)
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifndef NCONS
#define NCONS 8
#endif

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
static volatile long   work = 0, done_count = 0;
static volatile int    stop = 0;
static pthread_t       cons[NCONS];
static volatile long   cons_wakeups[NCONS];
static volatile int    storm_hits = 0;

static void handler(int sig) { (void)sig; storm_hits++; }

static void* consumer(void* arg) {
	long idx = (long)arg;
	for (;;) {
		pthread_mutex_lock(&mtx);
		while (work == 0 && !stop) pthread_cond_wait(&cv, &mtx);
		if (stop) { pthread_mutex_unlock(&mtx); break; }
		work--; done_count++; cons_wakeups[idx]++;
		pthread_mutex_unlock(&mtx);
	}
	return NULL;
}
static void* producer(void* arg) {
	(void)arg;
	while (!stop) {
		pthread_mutex_lock(&mtx); work++; pthread_mutex_unlock(&mtx);
		pthread_cond_signal(&cv);
#ifndef NO_BCAST
		if ((done_count & 0x3f) == 0) pthread_cond_broadcast(&cv);
#endif
	}
	return NULL;
}
#ifndef NO_STORM
// STORM_THROTTLE_US: microseconds to sleep between signal bursts. 0 = pathological
// unbounded flood (default). A realistic SIGCHLD-like rate is ~hundreds-1000/sec, i.e.
// STORM_THROTTLE_US ~= 1000. Used to test whether the Part 2b residual is a
// pathological-intensity starvation vs a rate-independent lost wakeup.
#ifndef STORM_THROTTLE_US
#define STORM_THROTTLE_US 0
#endif
static void* stormer(void* arg) {
	(void)arg;
	while (!stop) {
		for (int i = 0; i < NCONS; i++) pthread_kill(cons[i], SIGUSR1);
#if STORM_THROTTLE_US > 0
		usleep(STORM_THROTTLE_US);
#endif
	}
	return NULL;
}
#endif

int main(int argc, char** argv) {
	int secs = (argc > 1) ? atoi(argv[1]) : 20;
	struct sigaction sa; memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler; sigemptyset(&sa.sa_mask);
#ifdef SA_RESTART_ON
	sa.sa_flags = SA_RESTART;
#else
	sa.sa_flags = 0;
#endif
	sigaction(SIGUSR1, &sa, NULL);

	pthread_t prod, storm; (void)storm;
	for (long i = 0; i < NCONS; i++) pthread_create(&cons[i], NULL, consumer, (void*)i);
	pthread_create(&prod, NULL, producer, NULL);
#ifndef NO_STORM
	pthread_create(&storm, NULL, stormer, NULL);
#endif
	const int STALL_LIMIT = 5;
	long last = -1; int stall = 0; int hung = 0;
	for (int t = 0; t < secs; t++) {
		sleep(1);
		long cur = done_count;
		stall = (cur == last) ? stall + 1 : 0; last = cur;
		printf("[t=%2ds] done=%ld work=%ld storm_hits=%d stall=%ds\n", t+1, cur, work, storm_hits, stall);
		fflush(stdout);
		if (stall >= STALL_LIMIT) {
			printf("HANG stuck=%ld iters:", cur);
			for (int i = 0; i < NCONS; i++) printf(" c%d=%ld", i, cons_wakeups[i]);
			printf("\n"); fflush(stdout); hung = 1; break;
		}
	}
	stop = 1;
	pthread_mutex_lock(&mtx); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mtx);
	printf(hung ? "RESULT=HANG done=%ld\n" : "RESULT=OK done=%ld\n", done_count);
	return hung;
}
