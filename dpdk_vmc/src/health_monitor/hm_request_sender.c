#include "hm_request_sender.h"
#include <health_monitor.h>
#include <vmc_message_types.h>
#include "Packet.h"
#include "Config.h"
#include "Common.h"
#include "Port.h"

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
// Request hedefleri
//   VS CPU  : VLAN 98  + VL-ID 15 (0x000f) -> response VLAN 226 / VL-ID 13
//   FLCS CPU: VLAN 100 + VL-ID 12 (0x000c) -> response VLAN 228 / VL-ID 10
// Response'lar zaten rx_worker fast-path'inde hm_handle_packet()'e düşüyor.
// ----------------------------------------------------------------------------
struct hm_request_target {
    uint16_t    vlan_id;
    uint16_t    vl_id;
    const char *label;
};

static const struct hm_request_target g_targets[] = {
    { .vlan_id = 98,  .vl_id = HEALTH_MONITOR_VS_PBIT_REQUEST_VLID,   .label = "VS"   },
    { .vlan_id = 100, .vl_id = HEALTH_MONITOR_FLCS_PBIT_REQUEST_VLID, .label = "FLCS" },
};
#define HM_REQUEST_TARGET_COUNT (sizeof(g_targets) / sizeof(g_targets[0]))

// Paket ölçüleri: 14 ETH + 4 VLAN + 20 IP + 8 UDP + 11 payload = 57 B.
// Ethernet min frame 60 B olduğundan NIC HW padding uygular; IP total_length
// alanı gerçek payload uzunluğunu (39) yansıtır — alıcıyı kirletmez.
#define HM_REQ_PAYLOAD_LEN  ((uint16_t)sizeof(pbit_result_req_mes_t))   // 11
#define HM_REQ_L2_LEN       (ETH_HDR_SIZE + VLAN_HDR_SIZE)              // 18
#define HM_REQ_PACKET_SIZE  (HM_REQ_L2_LEN + IP_HDR_SIZE + UDP_HDR_SIZE + HM_REQ_PAYLOAD_LEN)

static struct {
    pthread_t             thread;
    struct rte_mempool   *mbuf_pool;
    volatile bool        *force_quit;
    atomic_bool           self_quit;
    atomic_uint_fast64_t  tx_count;
    atomic_uint_fast64_t  tx_drops;
    bool                  running;
    bool                  initialized;
} g_st;

static uint64_t now_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int build_hm_request(struct rte_mbuf *mbuf,
                            const struct hm_request_target *t)
{
    struct packet_config cfg;
    init_packet_config(&cfg);
#if VLAN_ENABLED
    cfg.vlan_id       = t->vlan_id;
    cfg.vlan_priority = 0;
#endif
    cfg.vl_id = t->vl_id;

    // DST MAC son 2 byte = VL-ID (03:00:00:00:VV:VV)
    cfg.dst_mac.addr_bytes[4] = (uint8_t)((t->vl_id >> 8) & 0xFF);
    cfg.dst_mac.addr_bytes[5] = (uint8_t)(t->vl_id & 0xFF);

    // DST IP son 2 byte = VL-ID (224.224.VV.VV)
    cfg.dst_ip = (224U << 24) | (224U << 16) | ((uint32_t)t->vl_id & 0xFFFFU);

    int ret = build_packet_dynamic(mbuf, &cfg, HM_REQ_PACKET_SIZE);
    if (ret < 0) {
        return ret;
    }

    // pbit_result_req_mes_t (11 B) payload'ı büyük-endian wire format ile yaz.
    // message_identifier 1 byte olduğu için swap gerekmez.
    pbit_result_req_mes_t req;
    req.header_st.message_identifier = HM_REQUEST_MSG_IDENTIFIER;
    req.header_st.message_len        = rte_cpu_to_be_16(HM_REQ_PAYLOAD_LEN);
    req.header_st.timestamp          = rte_cpu_to_be_64(now_mono_ns());

    uint8_t *payload = rte_pktmbuf_mtod_offset(
        mbuf, uint8_t *, HM_REQ_L2_LEN + IP_HDR_SIZE + UDP_HDR_SIZE);
    memcpy(payload, &req, sizeof(req));

    return 0;
}

static bool stop_requested(void)
{
    if (atomic_load_explicit(&g_st.self_quit, memory_order_relaxed)) return true;
    if (g_st.force_quit && *g_st.force_quit) return true;
    return false;
}

static void *hm_request_thread(void *arg)
{
    (void)arg;

    while (!stop_requested()) {
        for (size_t i = 0; i < HM_REQUEST_TARGET_COUNT; i++) {
            struct rte_mbuf *mbuf = rte_pktmbuf_alloc(g_st.mbuf_pool);
            if (!mbuf) {
                atomic_fetch_add_explicit(&g_st.tx_drops, 1, memory_order_relaxed);
                continue;
            }

            if (build_hm_request(mbuf, &g_targets[i]) < 0) {
                rte_pktmbuf_free(mbuf);
                atomic_fetch_add_explicit(&g_st.tx_drops, 1, memory_order_relaxed);
                continue;
            }

            uint16_t sent = rte_eth_tx_burst(HM_REQUEST_TX_PORT_ID,
                                             HM_REQUEST_TX_QUEUE_ID,
                                             &mbuf, 1);
            if (sent != 1) {
                rte_pktmbuf_free(mbuf);
                atomic_fetch_add_explicit(&g_st.tx_drops, 1, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&g_st.tx_count, 1, memory_order_relaxed);
            }
        }

        // ~1 s'i 100 ms'lik parçalara bölerek bekle; stop sinyali responsive kalır.
        for (int i = 0; i < (HM_REQUEST_PERIOD_MS / 100) && !stop_requested(); i++) {
            usleep(100 * 1000);
        }
    }

    return NULL;
}

int hm_request_sender_init(void)
{
    if (g_st.initialized) return 0;

    // Port 2'nin NUMA node'unu ports_config'ten al, doğru pool'u bul.
    int numa = 0;
    bool found = false;
    for (uint16_t i = 0; i < ports_config.nb_ports; i++) {
        if (ports_config.ports[i].port_id == HM_REQUEST_TX_PORT_ID) {
            numa = ports_config.ports[i].numa_node;
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "[HM-REQ] Port %u not present in ports_config\n",
                HM_REQUEST_TX_PORT_ID);
        return -1;
    }

    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "mbuf_pool_%u_%u",
             (unsigned)numa, (unsigned)HM_REQUEST_TX_PORT_ID);
    g_st.mbuf_pool = rte_mempool_lookup(pool_name);
    if (!g_st.mbuf_pool) {
        fprintf(stderr, "[HM-REQ] mbuf pool '%s' not found\n", pool_name);
        return -1;
    }

    atomic_store(&g_st.self_quit, false);
    atomic_store(&g_st.tx_count, 0);
    atomic_store(&g_st.tx_drops, 0);
    g_st.initialized = true;

    printf("[HM-REQ] init ok: port=%u queue=%u pool='%s' pkt_size=%u B\n",
           HM_REQUEST_TX_PORT_ID, HM_REQUEST_TX_QUEUE_ID, pool_name,
           (unsigned)HM_REQ_PACKET_SIZE);
    return 0;
}

int hm_request_sender_start(volatile bool *force_quit)
{
    if (!g_st.initialized) return -1;
    if (g_st.running)      return 0;

    g_st.force_quit = force_quit;
    atomic_store(&g_st.self_quit, false);

    if (pthread_create(&g_st.thread, NULL, hm_request_thread, NULL) != 0) {
        fprintf(stderr, "[HM-REQ] pthread_create failed: %s\n", strerror(errno));
        return -1;
    }
    g_st.running = true;
    printf("[HM-REQ] sender started: %u targets @ %u Hz\n",
           (unsigned)HM_REQUEST_TARGET_COUNT,
           (unsigned)(1000 / HM_REQUEST_PERIOD_MS));
    return 0;
}

void hm_request_sender_stop(void)
{
    if (!g_st.running) return;

    atomic_store(&g_st.self_quit, true);
    pthread_join(g_st.thread, NULL);
    g_st.running = false;

    printf("[HM-REQ] sender stopped. tx=%lu drops=%lu\n",
           (unsigned long)atomic_load(&g_st.tx_count),
           (unsigned long)atomic_load(&g_st.tx_drops));
}

uint64_t hm_request_sender_tx_count(void)
{
    return (uint64_t)atomic_load(&g_st.tx_count);
}

uint64_t hm_request_sender_tx_drops(void)
{
    return (uint64_t)atomic_load(&g_st.tx_drops);
}
