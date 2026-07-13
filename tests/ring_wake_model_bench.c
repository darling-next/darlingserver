// perf#18 .31 (dar-dar6x4-perf-5dq.31) P4-spike: isolated host A/B of ring WAKE-MODEL variants.
//
// Committed under tests/ as the data-driven justification for the P4 wake design (.33) and as a
// regression gate on it. Build + run via tests/run-ring-wake-model-bench.sh.
//
// Two pinned processes (server + client) share a real SPSC ring pair in anonymous shared
// memory. NO Mach, NO dispatch, NO darlingserver -- this isolates the ONE variable the live
// A/B proved dominates: the cross-process WAKEUP model. The request "work" is trivial (echo
// seq), so latency == transport + doorbell + scheduler + reply-wake, nothing else.
//
// Variants (select with argv[1]):
//   v0  baseline: client eventfd_write doorbell EVERY request; server blocks in epoll/read
//                 every iteration; server FUTEX_WAKE every reply; client FUTEX_WAIT every reply.
//                 (reproduces the current darlingserver P3 model -> the ~26us control)
//   v1  conditional doorbell + adaptive server poll: server spins the ring (bounded budget)
//                 before arming+sleeping; client writes the doorbell ONLY if server state is
//                 SLEEP_ARMED/SLEEPING. Reply wake still always-futex (isolates the c2s side).
//   v2  guest spin-before-futex on the reply: client spins on the reply seq before FUTEX_WAIT;
//                 server FUTEX_WAKE only if the client waiter-bit is set. c2s still always-doorbell
//                 (isolates the s2c side).
//   v3  v1 + v2 combined (the gist target): hot path 0 syscalls / 0 forced ctx switches.
//
// Measures (client side): p50/p90/p99/max ns, plus voluntary+involuntary ctx switches/call
// (getrusage delta). syscalls/call is measured externally by wrapping in `strace -f -c`.
//
// Cases: --hot (server busy-spins so it's always runnable; pinned) vs default cold-ish
// (server uses its normal sleep path). Pin cores with --cpu A,B.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <stdatomic.h>

// ---- geometry ----
#define SLOT_SIZE   64u
#define SLOT_COUNT  64u            // power of two
#define SLOT_MASK   (SLOT_COUNT - 1u)

// ---- server states (gist model) ----
enum { SRV_ACTIVE=0, SRV_SPINNING=1, SRV_SLEEP_ARMED=2, SRV_SLEEPING=3 };

typedef struct {
    _Alignas(64) atomic_uint c2s_head;   // consumer (server) owns
    _Alignas(64) atomic_uint c2s_tail;   // producer (client) owns
    _Alignas(64) atomic_uint s2c_head;   // consumer (client) owns
    _Alignas(64) atomic_uint s2c_tail;   // producer (server) owns
    _Alignas(64) atomic_int  srv_state;  // SRV_*
    _Alignas(64) atomic_uint reply_seq;  // last published reply seq (client spins on this)
    _Alignas(64) atomic_int  client_waiting; // client waiter-bit (server FUTEX_WAKE only if set)
    _Alignas(64) atomic_uint s2c_futex;  // futex word the client waits on
    // request/reply payload slots
    uint8_t c2s_slots[SLOT_COUNT * SLOT_SIZE];
    uint8_t s2c_slots[SLOT_COUNT * SLOT_SIZE];
} shm_t;

typedef struct { uint32_t seq; uint32_t val; } msg_t;

static int g_variant = 0;
static int g_hot = 0;
static long g_spin_budget = 2000;     // server idle-spin iterations before arming
static long g_client_spin = 2000;     // client reply-spin iterations before futex
static int  g_doorbell_fd = -1;

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}
static long futex(atomic_uint* uaddr, int op, uint32_t val) {
    return syscall(SYS_futex, uaddr, op, val, NULL, NULL, 0);
}
static inline uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

// ---- SPSC helpers (free-running counters; this harness trusts both ends) ----
static bool ring_push(atomic_uint* head, atomic_uint* tail, uint8_t* slots, const msg_t* m) {
    uint32_t t = atomic_load_explicit(tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(head, memory_order_acquire);
    if ((t - h) >= SLOT_COUNT) return false; // full
    memcpy(slots + (t & SLOT_MASK) * SLOT_SIZE, m, sizeof(*m));
    atomic_store_explicit(tail, t + 1, memory_order_release);
    return true;
}
static bool ring_pop(atomic_uint* head, atomic_uint* tail, uint8_t* slots, msg_t* m) {
    uint32_t h = atomic_load_explicit(head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(tail, memory_order_acquire);
    if (h == t) return false; // empty
    memcpy(m, slots + (h & SLOT_MASK) * SLOT_SIZE, sizeof(*m));
    atomic_store_explicit(head, h + 1, memory_order_release);
    return true;
}

// =========================== SERVER ===========================
static void server_loop(shm_t* s, long total) {
    long served = 0;
    long idle = 0;
    while (served < total) {
        msg_t req;
        if (ring_pop(&s->c2s_head, &s->c2s_tail, s->c2s_slots, &req)) {
            idle = 0;
            atomic_store_explicit(&s->srv_state, SRV_ACTIVE, memory_order_release);
            // "work": echo seq with val+1
            msg_t rep = { req.seq, req.val + 1 };
            while (!ring_push(&s->s2c_head, &s->s2c_tail, s->s2c_slots, &rep)) cpu_relax();
            atomic_store_explicit(&s->reply_seq, req.seq, memory_order_release);
            served++;

            // reply-wake side
            if (g_variant == 0 || g_variant == 1) {
                // always-futex reply (v0, v1)
                atomic_fetch_add_explicit(&s->s2c_futex, 1, memory_order_release);
                futex(&s->s2c_futex, FUTEX_WAKE, 1);
            } else {
                // v2, v3: wake only if client marked itself waiting
                atomic_fetch_add_explicit(&s->s2c_futex, 1, memory_order_release);
                if (atomic_load_explicit(&s->client_waiting, memory_order_acquire))
                    futex(&s->s2c_futex, FUTEX_WAKE, 1);
            }
            continue;
        }

        // c2s empty
        if (g_hot) { cpu_relax(); continue; } // hot: never sleep, just spin

        if (g_variant == 0) {
            // baseline: block on the doorbell every empty turn
            atomic_store_explicit(&s->srv_state, SRV_SLEEPING, memory_order_release);
            uint64_t v; (void)!read(g_doorbell_fd, &v, sizeof(v)); // blocking read = epoll-ish wait
            atomic_store_explicit(&s->srv_state, SRV_ACTIVE, memory_order_release);
            continue;
        }

        // v1/v2/v3: adaptive poll then arm-and-sleep with critical recheck
        if (++idle < g_spin_budget) { atomic_store_explicit(&s->srv_state, SRV_SPINNING, memory_order_relaxed); cpu_relax(); continue; }
        atomic_store_explicit(&s->srv_state, SRV_SLEEP_ARMED, memory_order_release);
        // critical recheck: a request may have been published between the last pop and arming
        if (atomic_load_explicit(&s->c2s_tail, memory_order_acquire) !=
            atomic_load_explicit(&s->c2s_head, memory_order_acquire)) {
            atomic_store_explicit(&s->srv_state, SRV_ACTIVE, memory_order_release);
            idle = 0;
            continue;
        }
        atomic_store_explicit(&s->srv_state, SRV_SLEEPING, memory_order_release);
        uint64_t v; (void)!read(g_doorbell_fd, &v, sizeof(v)); // sleep until client doorbells
        atomic_store_explicit(&s->srv_state, SRV_ACTIVE, memory_order_release);
        idle = 0;
    }
}

// =========================== CLIENT ===========================
static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}
static void client_loop(shm_t* s, long total, uint64_t* lat, long warmup) {
    for (long i = 0; i < total + warmup; i++) {
        uint32_t seq = (uint32_t)(i + 1);
        uint64_t t0 = now_ns();

        // publish request
        msg_t req = { seq, (uint32_t)i };
        while (!ring_push(&s->c2s_head, &s->c2s_tail, s->c2s_slots, &req)) cpu_relax();

        // doorbell the server
        if (g_variant == 0 || g_variant == 2) {
            // always doorbell (v0, v2)
            uint64_t one = 1; (void)!write(g_doorbell_fd, &one, sizeof(one));
        } else {
            // v1, v3: doorbell ONLY if server is asleep or arming
            int st = atomic_load_explicit(&s->srv_state, memory_order_acquire);
            if (st == SRV_SLEEP_ARMED || st == SRV_SLEEPING) {
                uint64_t one = 1; (void)!write(g_doorbell_fd, &one, sizeof(one));
            }
        }

        // wait for the reply with seq
        if (g_variant == 0 || g_variant == 1) {
            // always-futex reply wait (v0, v1)
            for (;;) {
                if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) == seq) break;
                uint32_t f = atomic_load_explicit(&s->s2c_futex, memory_order_acquire);
                if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) == seq) break;
                futex(&s->s2c_futex, FUTEX_WAIT, f);
            }
        } else {
            // v2, v3: spin-before-futex
            long sp = 0;
            for (; sp < g_client_spin; sp++) {
                if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) == seq) break;
                cpu_relax();
            }
            if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) != seq) {
                atomic_store_explicit(&s->client_waiting, 1, memory_order_release);
                for (;;) {
                    if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) == seq) break;
                    uint32_t f = atomic_load_explicit(&s->s2c_futex, memory_order_acquire);
                    if (atomic_load_explicit(&s->reply_seq, memory_order_acquire) == seq) break;
                    futex(&s->s2c_futex, FUTEX_WAIT, f);
                }
                atomic_store_explicit(&s->client_waiting, 0, memory_order_release);
            }
        }
        // drain the reply slot so the ring doesn't fill
        msg_t rep; ring_pop(&s->s2c_head, &s->s2c_tail, s->s2c_slots, &rep);

        uint64_t t1 = now_ns();
        if (i >= warmup) lat[i - warmup] = t1 - t0;
    }
}

int main(int argc, char** argv) {
    long N = 100000, warmup = 1000;
    int cpu_srv = -1, cpu_cli = -1;
    const char* vname = (argc > 1) ? argv[1] : "v0";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--hot")) g_hot = 1;
        else if (!strcmp(argv[i], "--n") && i+1 < argc) N = atol(argv[++i]);
        else if (!strcmp(argv[i], "--spin") && i+1 < argc) g_spin_budget = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cspin") && i+1 < argc) g_client_spin = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cpu") && i+1 < argc) { sscanf(argv[++i], "%d,%d", &cpu_srv, &cpu_cli); }
    }
    if      (!strcmp(vname, "v0")) g_variant = 0;
    else if (!strcmp(vname, "v1")) g_variant = 1;
    else if (!strcmp(vname, "v2")) g_variant = 2;
    else if (!strcmp(vname, "v3")) g_variant = 3;
    else { fprintf(stderr, "unknown variant %s\n", vname); return 2; }

    shm_t* s = mmap(NULL, sizeof(shm_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) { perror("mmap"); return 2; }
    memset(s, 0, sizeof(*s));
    uint64_t* lat = mmap(NULL, sizeof(uint64_t) * N, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (lat == MAP_FAILED) { perror("mmap lat"); return 2; }

    g_doorbell_fd = eventfd(0, 0); // blocking; semaphore-less counter
    if (g_doorbell_fd < 0) { perror("eventfd"); return 2; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        pin(cpu_srv);
        server_loop(s, N + warmup);
        _exit(0);
    }
    pin(cpu_cli);
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    client_loop(s, N, lat, warmup);
    getrusage(RUSAGE_SELF, &ru1);

    // make sure the server drains/exits
    int st; waitpid(pid, &st, 0);

    qsort(lat, N, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0; for (long i = 0; i < N; i++) sum += lat[i];
    double mean = (double)sum / N;
    long nvcsw = ru1.ru_nvcsw - ru0.ru_nvcsw;
    long nivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;
    printf("variant=%s hot=%d N=%ld spin=%ld cspin=%ld  "
           "p50=%lu p90=%lu p99=%lu max=%lu mean=%.0f ns  "
           "vctxsw/call=%.4f ivctxsw/call=%.4f\n",
           vname, g_hot, N, g_spin_budget, g_client_spin,
           lat[N/2], lat[(long)(N*0.90)], lat[(long)(N*0.99)], lat[N-1], mean,
           (double)nvcsw / N, (double)nivcsw / N);
    return 0;
}
