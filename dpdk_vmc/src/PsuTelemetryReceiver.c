#include "PsuTelemetryReceiver.h"
#include "PsuTelemetry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ====== Internal state ====== */

struct psu_telem_state {
    int                sockfd;
    pthread_t          thread;
    pthread_spinlock_t lock;

    /* Cached latest packet + its local reception monotonic timestamp. */
    psu_telem_pkt_t    cached_pkt;
    bool               has_data;
    uint64_t           last_rx_mono_ns;

    /* Shared stop flags. */
    volatile bool     *force_quit;       /* external (DPDK main) */
    atomic_bool        self_quit;         /* internal stop() request */

    /* Stats. */
    atomic_uint_fast64_t packets_received;
    atomic_uint_fast64_t packets_dropped; /* bad magic / wrong size / version */

    bool               running;
};

static struct psu_telem_state g_state = {
    .sockfd = -1,
    .has_data = false,
    .running = false,
};

/* Staleness threshold in nanoseconds. If we haven't received anything in
 * this window we tag the table as STALE. Publisher sends at 1 Hz; 3 s gives
 * tolerance for occasional UDP drops without looking unhealthy on the first
 * missed packet. */
#define PSU_TELEM_STALE_NS  (3ull * 1000000000ull)

static uint64_t now_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ====== Listener thread ====== */

static void *listener_thread_func(void *arg)
{
    (void)arg;

    psu_telem_pkt_t buf;
    while (!atomic_load_explicit(&g_state.self_quit, memory_order_relaxed) &&
           !(g_state.force_quit && *g_state.force_quit)) {

        ssize_t n = recvfrom(g_state.sockfd, &buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) {
            /* Timeout (SO_RCVTIMEO) or interrupted -> just loop. */
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            /* Real error -> stop listening. */
            fprintf(stderr, "[PSU-TELEM] recvfrom failed: %s\n", strerror(errno));
            break;
        }

        if (n != (ssize_t)sizeof(psu_telem_pkt_t)) {
            atomic_fetch_add_explicit(&g_state.packets_dropped, 1,
                                      memory_order_relaxed);
            continue;
        }
        if (buf.magic != PSU_TELEM_MAGIC) {
            atomic_fetch_add_explicit(&g_state.packets_dropped, 1,
                                      memory_order_relaxed);
            continue;
        }
        if (buf.version != PSU_TELEM_VERSION) {
            /* Wire format mismatch. Drop and log once per incident. */
            static atomic_bool warned = false;
            bool expected = false;
            if (atomic_compare_exchange_strong(&warned, &expected, true)) {
                fprintf(stderr,
                    "[PSU-TELEM] version mismatch: peer=%u, local=%u - "
                    "rebuild both sides.\n",
                    (unsigned)buf.version, (unsigned)PSU_TELEM_VERSION);
            }
            atomic_fetch_add_explicit(&g_state.packets_dropped, 1,
                                      memory_order_relaxed);
            continue;
        }

        pthread_spin_lock(&g_state.lock);
        g_state.cached_pkt = buf;
        g_state.has_data = true;
        g_state.last_rx_mono_ns = now_mono_ns();
        pthread_spin_unlock(&g_state.lock);

        atomic_fetch_add_explicit(&g_state.packets_received, 1,
                                  memory_order_relaxed);
    }

    return NULL;
}

/* ====== Public API ====== */

int psu_telem_init(uint16_t port)
{
    if (g_state.sockfd >= 0) {
        return 0;  /* already initialized */
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[PSU-TELEM] socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Short recv timeout so stop() is responsive without a fancy wake-up. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };  /* 200 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[PSU-TELEM] bind(port=%u) failed: %s\n",
                (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }

    if (pthread_spin_init(&g_state.lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        fprintf(stderr, "[PSU-TELEM] pthread_spin_init failed\n");
        close(fd);
        return -1;
    }

    g_state.sockfd = fd;
    g_state.has_data = false;
    g_state.last_rx_mono_ns = 0;
    atomic_store(&g_state.self_quit, false);
    atomic_store(&g_state.packets_received, 0);
    atomic_store(&g_state.packets_dropped, 0);

    printf("[PSU-TELEM] listener bound on UDP :%u\n", (unsigned)port);
    return 0;
}

int psu_telem_start(volatile bool *force_quit)
{
    if (g_state.running) return 0;
    if (g_state.sockfd < 0) return -1;

    g_state.force_quit = force_quit;
    atomic_store(&g_state.self_quit, false);

    if (pthread_create(&g_state.thread, NULL, listener_thread_func, NULL) != 0) {
        fprintf(stderr, "[PSU-TELEM] pthread_create failed: %s\n", strerror(errno));
        return -1;
    }
    g_state.running = true;
    printf("[PSU-TELEM] listener thread started\n");
    return 0;
}

void psu_telem_stop(void)
{
    if (!g_state.running && g_state.sockfd < 0) return;

    atomic_store(&g_state.self_quit, true);

    if (g_state.running) {
        pthread_join(g_state.thread, NULL);
        g_state.running = false;
    }

    if (g_state.sockfd >= 0) {
        close(g_state.sockfd);
        g_state.sockfd = -1;
    }

    pthread_spin_destroy(&g_state.lock);

    printf("[PSU-TELEM] listener stopped. received=%lu dropped=%lu\n",
           (unsigned long)atomic_load(&g_state.packets_received),
           (unsigned long)atomic_load(&g_state.packets_dropped));
}

uint64_t psu_telem_packets_received(void)
{
    return (uint64_t)atomic_load(&g_state.packets_received);
}

uint64_t psu_telem_packets_dropped(void)
{
    return (uint64_t)atomic_load(&g_state.packets_dropped);
}

bool psu_telem_is_running(void)
{
    return g_state.running;
}

/* ====== Printing ====== */

static const char *model_name(uint8_t model)
{
    switch (model) {
    case PSU_TELEM_MODEL_PSU30:  return "PSU30 (30V/56A)";
    case PSU_TELEM_MODEL_PSU300: return "PSU300 (300V/5.6A)";
    default: return "Unknown";
    }
}

void psu_telem_print_table(void)
{
    psu_telem_pkt_t snap;
    bool has_data;
    uint64_t last_rx;

    pthread_spin_lock(&g_state.lock);
    snap     = g_state.cached_pkt;
    has_data = g_state.has_data;
    last_rx  = g_state.last_rx_mono_ns;
    pthread_spin_unlock(&g_state.lock);

    printf("[HEALTH] ============ Power Supply (TDK Lambda) ============\n");

    if (!has_data) {
        printf("[HEALTH] PSU telemetry: NO DATA (no packet received yet)\n");
        printf("[HEALTH] ================================================\n\n");
        return;
    }

    uint64_t now = now_mono_ns();
    uint64_t age_ns = (now > last_rx) ? (now - last_rx) : 0;
    double   age_s  = age_ns / 1e9;

    if (age_ns > PSU_TELEM_STALE_NS) {
        printf("[HEALTH] PSU telemetry: STALE (last packet %.1fs ago, seq=%u)\n",
               age_s, (unsigned)snap.seq);
        printf("[HEALTH] ================================================\n\n");
        return;
    }

    const char *status =
        (snap.flags & PSU_TELEM_FLAG_PSU_ERROR) ? "ERROR (SCPI query failed)"
      : (snap.flags & PSU_TELEM_FLAG_OUTPUT_ON) ? "OUTPUT ON"
                                                : "OUTPUT OFF";

    printf("[HEALTH] Model       : %s\n", model_name(snap.model));
    printf("[HEALTH] Status      : %s  (seq=%u, age=%.1fs)\n",
           status, (unsigned)snap.seq, age_s);
    printf("[HEALTH] Setpoints   : V=%7.3f V  |  I=%7.3f A\n",
           (double)snap.set_voltage_v, (double)snap.set_current_a);

    if (snap.flags & PSU_TELEM_FLAG_PSU_ERROR) {
        printf("[HEALTH] Measurements: (unavailable - PSU query failed)\n");
    } else {
        printf("[HEALTH] Measured    : V=%7.3f V  |  I=%7.3f A  |  P=%8.3f W\n",
               (double)snap.voltage_v,
               (double)snap.current_a,
               (double)snap.power_w);
    }

    if (snap.flags & PSU_TELEM_FLAG_RECONNECTED) {
        printf("[HEALTH] Note        : PSU connection recovered this tick\n");
    }

    printf("[HEALTH] ================================================\n\n");
}
