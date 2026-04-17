/**
 * @file embedded_latency.c
 * @brief Embedded HW Timestamp Latency Test Implementation
 *
 * Performs latency measurement using NIC HW timestamps via
 * raw socket + SO_TIMESTAMPING.
 *
 * Must run before DPDK EAL starts!
 */

#include "EmbeddedLatency.h"
#include "AteCumulusConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <arpa/inet.h>
#include <linux/filter.h>
#include <fcntl.h>

// ============================================
// GLOBAL STATE
// ============================================
struct emb_latency_state g_emb_latency = {0};
bool g_ate_mode = false;
static bool g_using_hw_timestamps = true;  // Track if HW timestamps are actually used

// ============================================
// USER INTERACTION
// ============================================

/**
 * Ask a yes/no question to the user
 * @param question  Question text
 * @return          true = yes, false = no
 */
static bool ask_question(const char *question) {
    char response;
    int c;

    while (1) {
        printf("%s [y/n]: ", question);
        fflush(stdout);

        response = getchar();

        // Clear rest of line
        while ((c = getchar()) != '\n' && c != EOF);

        if (response == 'y' || response == 'Y') {
            return true;
        } else if (response == 'n' || response == 'N') {
            return false;
        }

        printf("Invalid input! Please enter 'y' or 'n'.\n");
    }
}

// ============================================
// CONFIGURATION
// ============================================

// Port info
static const struct {
    uint16_t port_id;
    const char *iface;
} PORT_INFO[] = {
    {0, "ens2f0np0"},
    {1, "ens2f1np1"},
    {2, "ens1f0np0"},
    {3, "ens1f1np1"},
    {4, "ens3f0np0"},
    {5, "ens3f1np1"},
    {6, "ens5f0np0"},
    {7, "ens5f1np1"},
};
#define NUM_PORTS (sizeof(PORT_INFO) / sizeof(PORT_INFO[0]))

// LOOPBACK TEST: Port pairs - TX -> RX mapping (through Mellanox switch)
static const struct {
    uint16_t tx_port;
    const char *tx_iface;
    uint16_t rx_port;
    const char *rx_iface;
    uint16_t vlans[4];
    uint16_t vl_ids[4];
    int vlan_count;
} LOOPBACK_PAIRS[] = {
    {0, "ens2f0np0", 7, "ens5f1np1", {105, 106, 107, 108}, {1027, 1155, 1283, 1411}, 4},
    {1, "ens2f1np1", 6, "ens5f0np0", {109, 110, 111, 112}, {1539, 1667, 1795, 1923}, 4},
    {2, "ens1f0np0", 5, "ens3f1np1", {97,  98,  99,  100}, {3,    131,  259,  387 }, 4},
    {3, "ens1f1np1", 4, "ens3f0np0", {101, 102, 103, 104}, {515,  643,  771,  899 }, 4},
    {4, "ens3f0np0", 3, "ens1f1np1", {113, 114, 115, 116}, {2051, 2179, 2307, 2435}, 4},
    {5, "ens3f1np1", 2, "ens1f0np0", {117, 118, 119, 120}, {2563, 2691, 2819, 2947}, 4},
    {6, "ens5f0np0", 1, "ens2f1np1", {121, 122, 123, 124}, {3075, 3203, 3331, 3459}, 4},
    {7, "ens5f1np1", 0, "ens2f0np0", {125, 126, 127, 128}, {3587, 3715, 3843, 3971}, 4},
};
#define NUM_LOOPBACK_PAIRS (sizeof(LOOPBACK_PAIRS) / sizeof(LOOPBACK_PAIRS[0]))

// UNIT TEST: Port pairs - neighboring ports (0↔1, 2↔3, 4↔5, 6↔7)
static const struct {
    uint16_t tx_port;
    const char *tx_iface;
    uint16_t rx_port;
    const char *rx_iface;
    uint16_t vlans[4];
    uint16_t vl_ids[4];
    int vlan_count;
} UNIT_TEST_PAIRS[] = {
    // Port 0 -> Port 1
    {0, "ens2f0np0", 1, "ens2f1np1", {105, 106, 107, 108}, {1027, 1155, 1283, 1411}, 4},
    // Port 1 -> Port 0
    {1, "ens2f1np1", 0, "ens2f0np0", {109, 110, 111, 112}, {1539, 1667, 1795, 1923}, 4},
    // Port 2 -> Port 3
    {2, "ens1f0np0", 3, "ens1f1np1", {97,  98,  99,  100}, {3,    131,  259,  387 }, 4},
    // Port 3 -> Port 2
    {3, "ens1f1np1", 2, "ens1f0np0", {101, 102, 103, 104}, {515,  643,  771,  899 }, 4},
    // Port 4 -> Port 5
    {4, "ens3f0np0", 5, "ens3f1np1", {113, 114, 115, 116}, {2051, 2179, 2307, 2435}, 4},
    // Port 5 -> Port 4
    {5, "ens3f1np1", 4, "ens3f0np0", {117, 118, 119, 120}, {2563, 2691, 2819, 2947}, 4},
    // Port 6 -> Port 7
    {6, "ens5f0np0", 7, "ens5f1np1", {121, 122, 123, 124}, {3075, 3203, 3331, 3459}, 4},
    // Port 7 -> Port 6
    {7, "ens5f1np1", 6, "ens5f0np0", {125, 126, 127, 128}, {3587, 3715, 3843, 3971}, 4},
};
#define NUM_UNIT_TEST_PAIRS (sizeof(UNIT_TEST_PAIRS) / sizeof(UNIT_TEST_PAIRS[0]))

// COPPER UNIT TEST: Direct connection (no switch, no VLAN)
// Port 12 (1G) ↔ Port 13 (100M) through DUT copper ports
static const struct {
    uint16_t tx_port;
    const char *tx_iface;
    uint16_t rx_port;
    const char *rx_iface;
    uint16_t vl_id;
} COPPER_UNIT_TEST_PAIRS[] = {
    {12, EMB_LAT_COPPER_PORT_12_IFACE, 13, EMB_LAT_COPPER_PORT_13_IFACE, EMB_LAT_COPPER_VLID_12_TO_13},
    {13, EMB_LAT_COPPER_PORT_13_IFACE, 12, EMB_LAT_COPPER_PORT_12_IFACE, EMB_LAT_COPPER_VLID_13_TO_12},
};
#define NUM_COPPER_UNIT_TEST_PAIRS (sizeof(COPPER_UNIT_TEST_PAIRS) / sizeof(COPPER_UNIT_TEST_PAIRS[0]))

// Legacy alias for backward compatibility
#define PORT_PAIRS LOOPBACK_PAIRS
#define NUM_PORT_PAIRS NUM_LOOPBACK_PAIRS

// Packet config
#define PACKET_SIZE_TAGGED   1518    // Max Ethernet frame with VLAN: 14 ETH + 4 VLAN + 1500 IP
#define PACKET_SIZE_UNTAGGED 1514    // Max Ethernet frame without VLAN: 14 ETH + 1500 IP
#define PACKET_SIZE          1518    // Legacy (tagged) - used for buffer allocation
#define ETH_P_8021Q     0x8100
#define ETH_P_IP        0x0800
#define ETH_P_PTP       0x88F7  // PTP over Ethernet (IEEE 1588)

// Source MAC
static const uint8_t SRC_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x20};
// Dest MAC prefix
static const uint8_t DST_MAC_PREFIX[4] = {0x03, 0x00, 0x00, 0x00};

// ============================================
// HELPERS
// ============================================

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static double ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

// Precise busy-wait delay (replaces inaccurate usleep for µs delays)
static void precise_busy_wait_us(uint32_t us) {
    uint64_t start = get_time_ns();
    uint64_t target = (uint64_t)us * 1000ULL;
    while ((get_time_ns() - start) < target) {
        __asm__ volatile("pause" ::: "memory");
    }
}

// Check HW timestamp support via ethtool
static bool check_hw_ts_support(const char *ifname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ethtool_ts_info ts_info;
    memset(&ts_info, 0, sizeof(ts_info));
    ts_info.cmd = ETHTOOL_GET_TS_INFO;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&ts_info;

    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
        close(sock);
        return false;
    }
    close(sock);

    bool has_tx_hw = (ts_info.so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE) != 0;
    bool has_rx_hw = (ts_info.so_timestamping & SOF_TIMESTAMPING_RX_HARDWARE) != 0;
    bool has_raw_hw = (ts_info.so_timestamping & SOF_TIMESTAMPING_RAW_HARDWARE) != 0;

    printf("[HW_TS] %s: TX_HW=%d RX_HW=%d RAW_HW=%d PHC=%d\n",
           ifname, has_tx_hw, has_rx_hw, has_raw_hw, ts_info.phc_index);

    return has_tx_hw && has_rx_hw && has_raw_hw;
}

// Convert FD to clockid_t for clock_gettime (Linux kernel convention)
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int) ~(fd)) << 3) | 3))

// Get PHC index for a given interface
static int get_phc_index(const char *ifname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct ethtool_ts_info ts_info;
    memset(&ts_info, 0, sizeof(ts_info));
    ts_info.cmd = ETHTOOL_GET_TS_INFO;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&ts_info;

    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
        close(sock);
        return -1;
    }
    close(sock);
    return ts_info.phc_index;
}

// Compare PHC clocks across all NIC pairs used in tests
static void debug_compare_phc_clocks(void) {
    printf("\n[PHC_DEBUG] ========== PHC Clock Comparison ==========\n");

    // Collect unique PHC indices and their interface names
    struct { int phc_index; const char *iface; uint64_t phc_time; } phc_info[NUM_PORTS];
    int phc_count = 0;

    for (size_t i = 0; i < NUM_PORTS; i++) {
        int idx = get_phc_index(PORT_INFO[i].iface);
        printf("[PHC_DEBUG] %s (port %u) → /dev/ptp%d\n", PORT_INFO[i].iface, PORT_INFO[i].port_id, idx);

        // Try to read PHC clock
        char ptp_dev[32];
        snprintf(ptp_dev, sizeof(ptp_dev), "/dev/ptp%d", idx);
        int fd = open(ptp_dev, O_RDONLY);
        if (fd >= 0) {
            clockid_t clk = FD_TO_CLOCKID(fd);
            struct timespec ts;
            if (clock_gettime(clk, &ts) == 0) {
                phc_info[phc_count].phc_index = idx;
                phc_info[phc_count].iface = PORT_INFO[i].iface;
                phc_info[phc_count].phc_time = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                printf("[PHC_DEBUG]   PHC time = %lu ns  (sec=%lu.%09lu)\n",
                       (unsigned long)phc_info[phc_count].phc_time,
                       (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec);
                phc_count++;
            } else {
                printf("[PHC_DEBUG]   clock_gettime failed: %s\n", strerror(errno));
            }
            close(fd);
        } else {
            printf("[PHC_DEBUG]   Cannot open %s: %s\n", ptp_dev, strerror(errno));
        }
    }

    // Also read system clock for reference
    struct timespec sys_ts;
    clock_gettime(CLOCK_REALTIME, &sys_ts);
    uint64_t sys_ns = (uint64_t)sys_ts.tv_sec * 1000000000ULL + sys_ts.tv_nsec;
    printf("[PHC_DEBUG] SYSTEM CLOCK = %lu ns  (sec=%lu.%09lu)\n",
           (unsigned long)sys_ns, (unsigned long)sys_ts.tv_sec, (unsigned long)sys_ts.tv_nsec);

    // Print cross-NIC offsets for loopback pairs
    if (phc_count >= 2) {
        printf("[PHC_DEBUG] --- Cross-NIC PHC offsets (loopback pairs) ---\n");
        for (size_t p = 0; p < NUM_LOOPBACK_PAIRS && p < 4; p++) {
            // Find PHC readings for TX and RX interfaces
            uint64_t tx_phc = 0, rx_phc = 0;
            const char *tx_if = LOOPBACK_PAIRS[p].tx_iface;
            const char *rx_if = LOOPBACK_PAIRS[p].rx_iface;

            for (int j = 0; j < phc_count; j++) {
                if (strcmp(phc_info[j].iface, tx_if) == 0) tx_phc = phc_info[j].phc_time;
                if (strcmp(phc_info[j].iface, rx_if) == 0) rx_phc = phc_info[j].phc_time;
            }

            if (tx_phc && rx_phc) {
                int64_t offset = (int64_t)rx_phc - (int64_t)tx_phc;
                printf("[PHC_DEBUG] Loopback %u→%u (%s→%s): PHC offset = %+.3f us\n",
                       LOOPBACK_PAIRS[p].tx_port, LOOPBACK_PAIRS[p].rx_port,
                       tx_if, rx_if, (double)offset / 1000.0);
            }
        }

        // Also show unit test pair offsets (should be ~0 since same NIC)
        printf("[PHC_DEBUG] --- Same-NIC PHC offsets (unit test pairs) ---\n");
        for (size_t p = 0; p < NUM_UNIT_TEST_PAIRS && p < 4; p++) {
            uint64_t tx_phc = 0, rx_phc = 0;
            const char *tx_if = UNIT_TEST_PAIRS[p].tx_iface;
            const char *rx_if = UNIT_TEST_PAIRS[p].rx_iface;

            for (int j = 0; j < phc_count; j++) {
                if (strcmp(phc_info[j].iface, tx_if) == 0) tx_phc = phc_info[j].phc_time;
                if (strcmp(phc_info[j].iface, rx_if) == 0) rx_phc = phc_info[j].phc_time;
            }

            if (tx_phc && rx_phc) {
                int64_t offset = (int64_t)rx_phc - (int64_t)tx_phc;
                printf("[PHC_DEBUG] Unit %u→%u (%s→%s): PHC offset = %+.3f us\n",
                       UNIT_TEST_PAIRS[p].tx_port, UNIT_TEST_PAIRS[p].rx_port,
                       tx_if, rx_if, (double)offset / 1000.0);
            }
        }
    }

    printf("[PHC_DEBUG] ================================================\n\n");
}

// Drain stale packets from RX socket buffer
static void drain_rx_buffer(int fd) {
    struct pollfd pfd = {fd, POLLIN, 0};
    uint8_t discard[2048];
    int drained = 0;

    while (poll(&pfd, 1, 0) > 0) {  // Non-blocking poll
        if (recv(fd, discard, sizeof(discard), MSG_DONTWAIT) <= 0)
            break;
        drained++;
        if (drained > 1000) break;  // Safety limit
    }
    if (drained > 0) {
        printf("[INFO] Drained %d stale packets from RX buffer\n", drained);
    }
}

// Extract sequence number from test packet
static uint64_t extract_sequence(const uint8_t *pkt, size_t len) {
    // Determine payload offset based on packet type
    uint16_t etype = ((uint16_t)pkt[12] << 8) | pkt[13];
    size_t offset;
    if (etype == ETH_P_8021Q) {
        offset = 14 + 4 + 20 + 8;  // ETH + VLAN + IP + UDP = 46
    } else if (etype == ETH_P_IP) {
        // Check if PTP-over-UDP (UDP dst port 319 = 0x013F)
        // IP proto at offset 23, UDP dst port at offset 36-37
        if (len >= 38 && pkt[23] == 0x11 &&
            pkt[36] == 0x01 && pkt[37] == 0x3F) {
            offset = 14 + 20 + 8 + 44;  // ETH + IP + UDP + PTP Sync = 86
        } else {
            offset = 14 + 20 + 8;       // ETH + IP + UDP = 42
        }
    } else {
        offset = 14 + 20 + 8;      // ETH + IP + UDP = 42 (fallback)
    }
    if (len < offset + 8) return 0;

    uint64_t seq = 0;
    for (int i = 0; i < 8; i++) {
        seq = (seq << 8) | pkt[offset + i];
    }
    return seq;
}

// ============================================
// HW TIMESTAMP SOCKET
// ============================================

typedef enum {
    EMB_SOCK_TX,
    EMB_SOCK_RX
} emb_sock_type_t;

static int create_raw_socket(const char *ifname, int *if_index, emb_sock_type_t type) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Get interface index
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        close(fd);
        return -1;
    }
    *if_index = ifr.ifr_ifindex;

    // Bind to interface
    struct sockaddr_ll sll = {0};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = *if_index;

    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    // Enable promiscuous mode for RX socket (needed for multicast packets)
    if (type == EMB_SOCK_RX) {
        struct packet_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.mr_ifindex = *if_index;
        mreq.mr_type = PACKET_MR_PROMISC;
        setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        // BPF filter: only accept packets with DST MAC prefix 03:00:00:00
        // This filters at kernel level, avoiding unnecessary packet processing
        struct sock_filter bpf_code[] = {
            // Load byte at offset 0 (first byte of DST MAC)
            { BPF_LD | BPF_B | BPF_ABS, 0, 0, 0 },
            // Jump if == 0x03, else reject
            { BPF_JMP | BPF_JEQ | BPF_K, 0, 3, 0x03 },
            // Load halfword at offset 2 (bytes 2-3 of DST MAC: should be 0x0000)
            { BPF_LD | BPF_H | BPF_ABS, 0, 0, 2 },
            // Jump if == 0x0000, else reject
            { BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0x0000 },
            // Accept: return max packet length
            { BPF_RET | BPF_K, 0, 0, 0xFFFFFFFF },
            // Reject: return 0
            { BPF_RET | BPF_K, 0, 0, 0 },
        };

        struct sock_fprog bpf_prog = {
            .len = sizeof(bpf_code) / sizeof(bpf_code[0]),
            .filter = bpf_code,
        };

        if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &bpf_prog, sizeof(bpf_prog)) < 0) {
            fprintf(stderr, "[WARN] BPF filter attach failed for %s: %s (continuing without filter)\n",
                    ifname, strerror(errno));
        }
    }

    // Enable HW timestamping on NIC (SIOCSHWTSTAMP)
    struct hwtstamp_config hwconfig = {0};
    hwconfig.tx_type = (type == EMB_SOCK_TX) ? HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF;
    hwconfig.rx_filter = (type == EMB_SOCK_RX) ? HWTSTAMP_FILTER_ALL : HWTSTAMP_FILTER_NONE;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&hwconfig;

    bool hw_ts_ok = true;
    if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
        // HWTSTAMP_FILTER_ALL not supported (e.g. BCM5720 copper NIC)
        // Retry with PTP L4 event filter (for PTP-over-UDP/IPv4), then L2
        if (type == EMB_SOCK_RX) {
            // Try PTP_V2_L4_EVENT first (matches PTP over UDP port 319)
            hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ifr.ifr_data = (void *)&hwconfig;

            if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
                // Try PTP_V2_L2_EVENT as last resort
                hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
                ifr.ifr_data = (void *)&hwconfig;

                if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
                    fprintf(stderr, "[WARN] SIOCSHWTSTAMP failed for %s (ALL + L4 + L2): %s\n",
                            ifname, strerror(errno));
                    hw_ts_ok = false;
                } else {
                    printf("[HW_TS] %s: using PTP_V2_L2_EVENT filter\n", ifname);
                }
            } else {
                printf("[HW_TS] %s: using PTP_V2_L4_EVENT filter (PTP over UDP)\n", ifname);
            }
        } else {
            // TX socket: BCM5720 may reject rx_filter=NONE if NIC was already
            // configured with a PTP filter. Retry with PTP L4/L2 filters.
            hwconfig.tx_type = HWTSTAMP_TX_ON;
            hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ifr.ifr_data = (void *)&hwconfig;

            if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
                hwconfig.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
                ifr.ifr_data = (void *)&hwconfig;

                if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
                    fprintf(stderr, "[WARN] SIOCSHWTSTAMP TX failed for %s (NONE + L4 + L2): %s\n",
                            ifname, strerror(errno));
                    hw_ts_ok = false;
                } else {
                    printf("[HW_TS] %s TX: using PTP_V2_L2_EVENT rx_filter\n", ifname);
                }
            } else {
                printf("[HW_TS] %s TX: using PTP_V2_L4_EVENT rx_filter\n", ifname);
            }
        }
        if (!hw_ts_ok) {
            fprintf(stderr, "[WARN] HW timestamps not available for %s - using SW fallback\n", ifname);
            g_using_hw_timestamps = false;
        }
    }

    // Request timestamps via socket option
    // Always request both HW and SW so we have fallback
    int flags = SOF_TIMESTAMPING_RAW_HARDWARE |
                SOF_TIMESTAMPING_SOFTWARE;
    if (type == EMB_SOCK_TX) {
        flags |= SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
        flags |= SOF_TIMESTAMPING_OPT_TSONLY;  // Only timestamp, don't echo 1518B packet back
    } else {
        flags |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
        perror("SO_TIMESTAMPING");
        close(fd);
        return -1;
    }

    return fd;
}

// ============================================
// PACKET BUILDING
// ============================================

// IP header checksum calculation
static uint16_t ip_checksum(const void *data, size_t len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

static int build_packet(uint8_t *buf, uint16_t vlan_id, uint16_t vl_id, uint64_t seq) {
    bool has_vlan = (vlan_id != 0);
    int frame_size = has_vlan ? PACKET_SIZE_TAGGED : PACKET_SIZE_UNTAGGED;

    memset(buf, 0, frame_size);
    int offset = 0;

    // Ethernet header
    // Dest MAC: 03:00:00:00:VL_HI:VL_LO
    buf[offset++] = DST_MAC_PREFIX[0];
    buf[offset++] = DST_MAC_PREFIX[1];
    buf[offset++] = DST_MAC_PREFIX[2];
    buf[offset++] = DST_MAC_PREFIX[3];
    buf[offset++] = (vl_id >> 8) & 0xFF;
    buf[offset++] = vl_id & 0xFF;

    // Source MAC
    memcpy(buf + offset, SRC_MAC, 6);
    offset += 6;

    if (has_vlan) {
        // ---- FIBER: VLAN + IP/UDP packet ----
        // VLAN tag (802.1Q)
        buf[offset++] = (ETH_P_8021Q >> 8) & 0xFF;
        buf[offset++] = ETH_P_8021Q & 0xFF;
        buf[offset++] = (vlan_id >> 8) & 0xFF;
        buf[offset++] = vlan_id & 0xFF;

        // EtherType: IP
        buf[offset++] = (ETH_P_IP >> 8) & 0xFF;
        buf[offset++] = ETH_P_IP & 0xFF;

        // IP header
        int ip_hdr_start = offset;
        buf[offset++] = 0x45;  // Version + IHL
        buf[offset++] = 0x00;  // TOS
        uint16_t ip_len = frame_size - 14 - 4;  // 1500
        buf[offset++] = (ip_len >> 8) & 0xFF;
        buf[offset++] = ip_len & 0xFF;
        buf[offset++] = (seq >> 8) & 0xFF;  // ID
        buf[offset++] = seq & 0xFF;
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Flags + Fragment
        buf[offset++] = 0x01;  // TTL
        buf[offset++] = 0x11;  // Protocol: UDP
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Checksum (below)

        // Source IP: 10.0.0.0
        buf[offset++] = 10; buf[offset++] = 0; buf[offset++] = 0; buf[offset++] = 0;
        // Dest IP: 224.224.VL_HI.VL_LO
        buf[offset++] = 224; buf[offset++] = 224;
        buf[offset++] = (vl_id >> 8) & 0xFF;
        buf[offset++] = vl_id & 0xFF;

        uint16_t csum = ip_checksum(buf + ip_hdr_start, 20);
        buf[ip_hdr_start + 10] = (csum >> 8) & 0xFF;
        buf[ip_hdr_start + 11] = csum & 0xFF;

        // UDP header
        buf[offset++] = 0x00; buf[offset++] = 0x64;  // Src port: 100
        buf[offset++] = 0x00; buf[offset++] = 0x64;  // Dst port: 100
        uint16_t udp_len = ip_len - 20;
        buf[offset++] = (udp_len >> 8) & 0xFF;
        buf[offset++] = udp_len & 0xFF;
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Checksum

        // Sequence number (8 bytes)
        for (int i = 7; i >= 0; i--)
            buf[offset++] = (seq >> (i * 8)) & 0xFF;

    } else {
        // ---- COPPER: PTP-over-UDP/IPv4 (for BCM5720 HW timestamp) ----
        // BCM5720 (tg3) only HW-timestamps PTP packets.
        // DUT doesn't forward PTP L2 (ethertype 0x88F7) on copper ports.
        // Solution: IP/UDP packet with dst_port=319 (PTP event port) +
        // PTP Sync as UDP payload. DUT sees IP→forwards. BCM5720 sees
        // PTP-over-UDP→HW timestamps.
        // Frame padded to 1514 bytes for max-size store-and-forward test.

        // EtherType: IP
        buf[offset++] = (ETH_P_IP >> 8) & 0xFF;
        buf[offset++] = ETH_P_IP & 0xFF;

        // IP header (20 bytes)
        int ip_hdr_start = offset;
        buf[offset++] = 0x45;  // Version=4, IHL=5
        buf[offset++] = 0x00;  // TOS
        uint16_t ip_len = frame_size - 14;  // 1500
        buf[offset++] = (ip_len >> 8) & 0xFF;
        buf[offset++] = ip_len & 0xFF;
        buf[offset++] = (seq >> 8) & 0xFF;  // ID
        buf[offset++] = seq & 0xFF;
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Flags + Fragment
        buf[offset++] = 0x01;  // TTL
        buf[offset++] = 0x11;  // Protocol: UDP (17)
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Checksum (calculated below)
        // Source IP: 10.0.0.0
        buf[offset++] = 10; buf[offset++] = 0; buf[offset++] = 0; buf[offset++] = 0;
        // Dest IP: 224.224.VL_HI.VL_LO (multicast for DUT forwarding)
        buf[offset++] = 224; buf[offset++] = 224;
        buf[offset++] = (vl_id >> 8) & 0xFF;
        buf[offset++] = vl_id & 0xFF;

        uint16_t csum = ip_checksum(buf + ip_hdr_start, 20);
        buf[ip_hdr_start + 10] = (csum >> 8) & 0xFF;
        buf[ip_hdr_start + 11] = csum & 0xFF;

        // UDP header (8 bytes) - dst_port=319 triggers PTP HW timestamping
        buf[offset++] = 0x01; buf[offset++] = 0x3F;  // Src port: 319 (PTP event)
        buf[offset++] = 0x01; buf[offset++] = 0x3F;  // Dst port: 319 (PTP event)
        uint16_t udp_len = ip_len - 20;  // 1480
        buf[offset++] = (udp_len >> 8) & 0xFF;
        buf[offset++] = udp_len & 0xFF;
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // Checksum (0 = optional in IPv4)

        // PTP Sync header as UDP payload (44 bytes)
        buf[offset++] = 0x00;  // transportSpecific(4) + messageType(4) = Sync(0x0)
        buf[offset++] = 0x02;  // reserved(4) + versionPTP(4) = PTPv2
        buf[offset++] = 0x00;
        buf[offset++] = 0x2C;  // messageLength = 44
        buf[offset++] = 0x0A;  // domainNumber = 10 (matches DUT PTP config)
        buf[offset++] = 0x00;  // reserved
        buf[offset++] = 0x00; buf[offset++] = 0x00;  // flagField
        offset += 8;           // correctionField (8 bytes) = 0
        offset += 4;           // reserved (4 bytes)
        // sourcePortIdentity (10 bytes) - embed VL-ID in first 2 bytes
        buf[offset++] = (vl_id >> 8) & 0xFF;
        buf[offset++] = vl_id & 0xFF;
        offset += 8;           // rest of sourcePortIdentity
        // sequenceId (2 bytes)
        buf[offset++] = (seq >> 8) & 0xFF;
        buf[offset++] = seq & 0xFF;
        buf[offset++] = 0x00;  // controlField (0 = Sync)
        buf[offset++] = 0x00;  // logMessageInterval
        // originTimestamp (10 bytes) - part of Sync body
        offset += 10;
        // -- end of PTP Sync (44 bytes) at offset 86 --

        // Sequence number (8 bytes) at offset 86
        for (int i = 7; i >= 0; i--)
            buf[offset++] = (seq >> (i * 8)) & 0xFF;

        // Rest of frame (up to 1514) is already zero from memset
    }

    return frame_size;
}

// Check if packet is our test packet (handles VLAN stripped, PTP copper packets)
static bool is_our_test_packet(const uint8_t *pkt, size_t len, uint16_t expected_vlan, uint16_t expected_vlid) {
    if (len < 14) return false;

    // Check DST MAC prefix: 03:00:00:00:XX:XX
    if (pkt[0] != DST_MAC_PREFIX[0] || pkt[1] != DST_MAC_PREFIX[1] ||
        pkt[2] != DST_MAC_PREFIX[2] || pkt[3] != DST_MAC_PREFIX[3]) {
        return false;
    }

    // Check VL-ID from DST MAC
    uint16_t vl_id = ((uint16_t)pkt[4] << 8) | pkt[5];
    if (expected_vlid != 0 && vl_id != expected_vlid) {
        return false;
    }

    // Check EtherType at offset 12-13
    uint16_t ether_type = ((uint16_t)pkt[12] << 8) | pkt[13];
    if (ether_type == ETH_P_8021Q) {
        // VLAN tagged - check VLAN ID at offset 14-15
        uint16_t vlan_id = (((uint16_t)pkt[14] << 8) | pkt[15]) & 0x0FFF;
        if (expected_vlan != 0 && vlan_id != expected_vlan) {
            return false;
        }
    } else if (ether_type == ETH_P_IP && len >= 44) {
        // IP packet - could be PTP-over-UDP (copper) or regular IP (fiber stripped)
        // Check for PTP-over-UDP: IP proto=UDP(17), UDP dst_port=319
        if (pkt[23] == 0x11 && pkt[36] == 0x01 && pkt[37] == 0x3F) {
            // Verify PTP Sync in UDP payload: offset 42 = PTP header
            if (len >= 47 && (pkt[42] & 0x0F) == 0x00 && pkt[43] == 0x02 && pkt[46] == 0x0A) {
                return true;  // PTP-over-UDP Sync, domain=10
            }
            return false;
        }
    }
    // If untagged (VLAN stripped by switch), accept based on VL-ID match

    return true;
}

// ============================================
// TIMESTAMP EXTRACTION
// ============================================

// SCM_TIMESTAMPING is the correct cmsg_type for SO_TIMESTAMPING
#ifndef SCM_TIMESTAMPING
#define SCM_TIMESTAMPING SO_TIMESTAMPING
#endif

// Timestamp source tracking
typedef enum { TS_NONE = 0, TS_HW = 1, TS_SW = 2 } ts_source_t;

static bool extract_timestamp_debug(struct msghdr *msg, uint64_t *ts_ns, ts_source_t *source) {
    struct cmsghdr *cmsg;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
            struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
            // ts[0] = software, ts[1] = deprecated, ts[2] = hardware (raw)

            // Hardware timestamp preferred
            if (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0) {
                *ts_ns = (uint64_t)ts[2].tv_sec * 1000000000ULL + ts[2].tv_nsec;
                if (source) *source = TS_HW;
                return true;
            }
            // Software timestamp fallback - WARN user!
            if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0) {
                *ts_ns = (uint64_t)ts[0].tv_sec * 1000000000ULL + ts[0].tv_nsec;
                fprintf(stderr, "[WARN] Using SOFTWARE timestamp (HW not available!) - latency will be ~10us higher!\n");
                g_using_hw_timestamps = false;
                if (source) *source = TS_SW;
                return true;
            }
        }
    }
    if (source) *source = TS_NONE;
    return false;
}

static bool extract_timestamp(struct msghdr *msg, uint64_t *ts_ns) {
    return extract_timestamp_debug(msg, ts_ns, NULL);
}

// ============================================
// SINGLE TEST
// ============================================

static int run_single_test(int tx_fd, int rx_fd, int tx_ifindex,
                           uint16_t tx_port, uint16_t rx_port,
                           uint16_t vlan_id, uint16_t vl_id,
                           int packet_count, int timeout_ms,
                           uint64_t max_latency_ns,
                           struct emb_latency_result *result) {

    memset(result, 0, sizeof(*result));
    result->tx_port = tx_port;
    result->rx_port = rx_port;
    result->vlan_id = vlan_id;
    result->vl_id = vl_id;
    result->min_latency_ns = UINT64_MAX;

    uint8_t tx_buf[2048];
    uint8_t rx_buf[2048];
    // Separate control buffers for TX and RX (prevents data corruption)
    char tx_ctrl_buf[1024];
    char rx_ctrl_buf[1024];
    // Separate dummy buffer for TX timestamp retrieval (OPT_TSONLY gives minimal data)
    uint8_t tx_ts_buf[64];

    uint64_t total_latency = 0;

    // --- WARM-UP: Send 2 packets to prime NIC TX/RX pipeline ---
    #define WARMUP_COUNT 2
    for (int w = 0; w < WARMUP_COUNT; w++) {
        uint64_t warmup_seq = ((uint64_t)vlan_id << 32) | 0xFFFF0000UL | w;
        int pkt_len = build_packet(tx_buf, vlan_id, vl_id, warmup_seq);

        struct sockaddr_ll sll = {0};
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = tx_ifindex;
        sll.sll_halen = 6;
        memcpy(sll.sll_addr, tx_buf, 6);

        sendto(tx_fd, tx_buf, pkt_len, 0, (struct sockaddr *)&sll, sizeof(sll));

        // Consume TX timestamp from error queue
        struct pollfd wpfd = {tx_fd, POLLERR, 0};
        if (poll(&wpfd, 1, 50) > 0) {
            struct msghdr wmsg = {0};
            struct iovec wiov = {tx_ts_buf, sizeof(tx_ts_buf)};
            wmsg.msg_iov = &wiov;
            wmsg.msg_iovlen = 1;
            wmsg.msg_control = tx_ctrl_buf;
            wmsg.msg_controllen = sizeof(tx_ctrl_buf);
            recvmsg(tx_fd, &wmsg, MSG_ERRQUEUE);
        }

        // Wait and discard RX warm-up packet
        struct pollfd wrpfd = {rx_fd, POLLIN, 0};
        if (poll(&wrpfd, 1, 50) > 0) {
            recv(rx_fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT);
        }

        precise_busy_wait_us(10);  // Small delay between warm-ups
    }
    // Drain any remaining warm-up packets from RX buffer
    drain_rx_buffer(rx_fd);

    // --- MEASUREMENT PACKETS ---
    for (int pkt = 0; pkt < packet_count; pkt++) {
        uint64_t seq = ((uint64_t)vlan_id << 32) | pkt;

        // Build packet
        int pkt_len = build_packet(tx_buf, vlan_id, vl_id, seq);

        // Send
        struct sockaddr_ll sll = {0};
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = tx_ifindex;
        sll.sll_halen = 6;
        memcpy(sll.sll_addr, tx_buf, 6);

        ssize_t sent = sendto(tx_fd, tx_buf, pkt_len, 0,
                              (struct sockaddr *)&sll, sizeof(sll));
        if (sent < 0) {
            snprintf(result->error_msg, sizeof(result->error_msg), "send failed: %s", strerror(errno));
            continue;
        }
        result->tx_count++;

        // Get TX timestamp from error queue (separate buffer from RX!)
        uint64_t tx_ts = 0;
        ts_source_t tx_src = TS_NONE;
        struct pollfd pfd = {tx_fd, POLLERR, 0};
        if (poll(&pfd, 1, 100) > 0) {
            struct msghdr msg = {0};
            struct iovec iov = {tx_ts_buf, sizeof(tx_ts_buf)};  // Separate small buffer
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = tx_ctrl_buf;  // Separate control buffer
            msg.msg_controllen = sizeof(tx_ctrl_buf);

            if (recvmsg(tx_fd, &msg, MSG_ERRQUEUE) < 0) {
                fprintf(stderr, "[WARN] TX timestamp recvmsg failed: %s\n", strerror(errno));
            } else {
                if (!extract_timestamp_debug(&msg, &tx_ts, &tx_src)) {
                    fprintf(stderr, "[WARN] No TX timestamp in error queue message\n");
                }
            }
        } else {
            fprintf(stderr, "[WARN] TX timestamp poll timeout for VLAN %u pkt %d\n", vlan_id, pkt);
        }

        // Wait for RX - use real time-based timeout
        uint64_t rx_start = get_time_ns();
        uint64_t rx_deadline = rx_start + (uint64_t)timeout_ms * 1000000ULL;
        bool received = false;

        while (!received) {
            uint64_t now = get_time_ns();
            if (now >= rx_deadline) break;

            int remaining_ms = (int)((rx_deadline - now) / 1000000ULL);
            if (remaining_ms <= 0) break;
            int poll_timeout = remaining_ms < 100 ? remaining_ms : 100;

            struct pollfd rx_pfd = {rx_fd, POLLIN, 0};
            int ret = poll(&rx_pfd, 1, poll_timeout);
            if (ret > 0) {
                struct msghdr msg = {0};
                struct iovec iov = {rx_buf, sizeof(rx_buf)};  // Separate RX buffer
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = rx_ctrl_buf;  // Separate control buffer
                msg.msg_controllen = sizeof(rx_ctrl_buf);

                ssize_t len = recvmsg(rx_fd, &msg, 0);
                if (len > 0) {
                    // Check if this is our test packet (handles VLAN stripped case)
                    if (is_our_test_packet(rx_buf, len, vlan_id, vl_id)) {
                        // Verify sequence number match
                        uint64_t rx_seq = extract_sequence(rx_buf, len);
                        if (rx_seq != seq) {
                            // Wrong sequence - stale or out-of-order packet, skip
                            continue;
                        }

                        uint64_t rx_ts = 0;
                        ts_source_t rx_src = TS_NONE;
                        extract_timestamp_debug(&msg, &rx_ts, &rx_src);

                        if (rx_ts > 0 && tx_ts > 0 && rx_ts > tx_ts) {
                            uint64_t latency = rx_ts - tx_ts;
                            total_latency += latency;

                            // Debug: print raw timestamps for first packet of each VLAN
                            if (pkt == 0) {
                                const char *tx_label = (tx_src == TS_HW) ? "HW" : (tx_src == TS_SW) ? "SW" : "??";
                                const char *rx_label = (rx_src == TS_HW) ? "HW" : (rx_src == TS_SW) ? "SW" : "??";
                                printf("  [DEBUG] VLAN %u Port %u→%u pkt#0:\n",
                                       vlan_id, tx_port, rx_port);
                                printf("    TX_raw = %lu ns [%s]  (sec=%lu)\n",
                                       (unsigned long)tx_ts, tx_label, (unsigned long)(tx_ts / 1000000000ULL));
                                printf("    RX_raw = %lu ns [%s]  (sec=%lu)\n",
                                       (unsigned long)rx_ts, rx_label, (unsigned long)(rx_ts / 1000000000ULL));
                                printf("    diff   = %lu ns = %.2f us\n",
                                       (unsigned long)latency, (double)latency / 1000.0);
                                // Compare with system clock for reference
                                struct timespec sys_ts;
                                clock_gettime(CLOCK_REALTIME, &sys_ts);
                                uint64_t sys_ns = (uint64_t)sys_ts.tv_sec * 1000000000ULL + sys_ts.tv_nsec;
                                printf("    SYS_clock = %lu ns  (sec=%lu)\n",
                                       (unsigned long)sys_ns, (unsigned long)sys_ts.tv_sec);
                                printf("    TX vs SYS offset = %.3f ms\n",
                                       (double)((int64_t)tx_ts - (int64_t)sys_ns) / 1000000.0);
                            }

                            if (latency < result->min_latency_ns)
                                result->min_latency_ns = latency;
                            if (latency > result->max_latency_ns)
                                result->max_latency_ns = latency;

                            result->rx_count++;
                            received = true;
                        } else if (tx_ts == 0) {
                            fprintf(stderr, "[WARN] VLAN %u pkt %d: TX timestamp missing\n",
                                    vlan_id, pkt);
                        }
                    }
                }
            }
        }
    }

    // Finalize
    if (result->rx_count > 0) {
        result->valid = true;
        result->avg_latency_ns = total_latency / result->rx_count;
        result->passed = (result->max_latency_ns <= max_latency_ns);
        if (result->min_latency_ns == UINT64_MAX)
            result->min_latency_ns = 0;
    } else {
        result->valid = false;
        result->passed = false;
        if (result->error_msg[0] == '\0')
            snprintf(result->error_msg, sizeof(result->error_msg), "No packets received");
    }

    return result->passed ? 0 : 1;
}

// ============================================
// MAIN TEST FUNCTION
// ============================================

int emb_latency_run(int packet_count, int timeout_ms, int max_latency_us) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         EMBEDDED HW TIMESTAMP LATENCY TEST                       ║\n");
    printf("║  Packets per VLAN: %-3d | Timeout: %dms | Max: %dus             ║\n",
           packet_count, timeout_ms, max_latency_us);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Reset state
    memset(&g_emb_latency, 0, sizeof(g_emb_latency));

    uint64_t max_latency_ns = (uint64_t)max_latency_us * 1000;
    uint64_t start_time = get_time_ns();
    int result_idx = 0;

    // Test each port pair
    for (size_t p = 0; p < NUM_PORT_PAIRS; p++) {
        printf("Testing port pair: Port %d (%s) -> Port %d (%s)\n",
               PORT_PAIRS[p].tx_port, PORT_PAIRS[p].tx_iface,
               PORT_PAIRS[p].rx_port, PORT_PAIRS[p].rx_iface);

        // Create sockets (TX and RX separately)
        int tx_ifindex, rx_ifindex;
        int tx_fd = create_raw_socket(PORT_PAIRS[p].tx_iface, &tx_ifindex, EMB_SOCK_TX);
        int rx_fd = create_raw_socket(PORT_PAIRS[p].rx_iface, &rx_ifindex, EMB_SOCK_RX);

        if (tx_fd < 0 || rx_fd < 0) {
            fprintf(stderr, "ERROR: Cannot create sockets for %s/%s\n",
                    PORT_PAIRS[p].tx_iface, PORT_PAIRS[p].rx_iface);
            if (tx_fd >= 0) close(tx_fd);
            if (rx_fd >= 0) close(rx_fd);
            continue;
        }

        // Wait for sockets to initialize, then drain stale packets
        usleep(10000);  // 10ms
        drain_rx_buffer(rx_fd);

        // Test each VLAN
        for (int v = 0; v < PORT_PAIRS[p].vlan_count; v++) {
            struct emb_latency_result *r = &g_emb_latency.results[result_idx];

            run_single_test(tx_fd, rx_fd, tx_ifindex,
                           PORT_PAIRS[p].tx_port, PORT_PAIRS[p].rx_port,
                           PORT_PAIRS[p].vlans[v], PORT_PAIRS[p].vl_ids[v],
                           packet_count, timeout_ms, max_latency_ns, r);

            if (r->passed) {
                g_emb_latency.passed_count++;
            } else {
                g_emb_latency.failed_count++;
            }
            result_idx++;

            // Inter-VLAN delay (precise busy-wait instead of inaccurate usleep)
            precise_busy_wait_us(32);
        }

        close(tx_fd);
        close(rx_fd);
    }

    // Finalize
    g_emb_latency.result_count = result_idx;
    g_emb_latency.test_completed = true;
    g_emb_latency.test_passed = (g_emb_latency.failed_count == 0);
    g_emb_latency.test_duration_ns = get_time_ns() - start_time;

    // Calculate overall stats
    uint64_t min = UINT64_MAX, max = 0, sum = 0;
    int valid_count = 0;
    for (int i = 0; i < result_idx; i++) {
        struct emb_latency_result *r = &g_emb_latency.results[i];
        if (r->valid && r->rx_count > 0) {
            if (r->min_latency_ns < min) min = r->min_latency_ns;
            if (r->max_latency_ns > max) max = r->max_latency_ns;
            sum += r->avg_latency_ns;
            valid_count++;
        }
    }
    g_emb_latency.overall_min_ns = (min == UINT64_MAX) ? 0 : min;
    g_emb_latency.overall_max_ns = max;
    g_emb_latency.overall_avg_ns = valid_count > 0 ? sum / valid_count : 0;

    // Print results
    emb_latency_print();

    return g_emb_latency.failed_count;
}

int emb_latency_run_default(void) {
    // Run unit test (neighboring ports: 0↔1, 2↔3, 4↔5, 6↔7)
    // This matches normal DPDK TX/RX port configuration
    return emb_latency_run_unit_test(1, 100, 100);  // 1 packet, 100ms timeout, 100us max
}

/**
 * Interactive latency test with user prompts
 * Follows the same pattern as Dtn.cpp latencyTestSequence()
 *
 * Tests neighboring ports: 0↔1, 2↔3, 4↔5, 6↔7
 * (matches normal DPDK TX/RX port configuration)
 *
 * @return  0 = all passed/skipped, >0 = fail count, <0 = error
 */
int emb_latency_run_interactive(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         HW TIMESTAMP LATENCY TEST (INTERACTIVE MODE)             ║\n");
    printf("║  Port pairs: 0↔1, 2↔3, 4↔5, 6↔7 (neighboring ports)             ║\n");
    printf("║  Max threshold: 100us                                            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Ask if user wants to run the test
    if (!ask_question("Do you want to run HW Timestamp Latency Test?")) {
        printf("Latency test skipped by user.\n\n");
        return 0;  // Skipped = success
    }

    // Confirm before starting
    if (ask_question("Ready to start latency test on neighboring ports (0↔1, 2↔3, 4↔5, 6↔7)?")) {
        printf("\nStarting latency test...\n");
        return emb_latency_run_default();
    } else {
        printf("Latency test skipped by user.\n\n");
        return 0;  // Skipped = success
    }
}

// ============================================
// LOOPBACK TEST (Mellanox Switch Latency)
// ============================================

int emb_latency_run_loopback(int packet_count, int timeout_ms, int max_latency_us) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         LOOPBACK TEST (Mellanox Switch Latency)                  ║\n");
    printf("║  Packets: %-3d | Timeout: %dms | Max: %dus                      ║\n",
           packet_count, timeout_ms, max_latency_us);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Verify HW timestamp support before starting
    g_using_hw_timestamps = true;
    printf("[HW_TS] Checking HW timestamp support...\n");
    if (!check_hw_ts_support(LOOPBACK_PAIRS[0].tx_iface)) {
        fprintf(stderr, "[WARN] HW timestamp not supported on %s - results may be inaccurate!\n",
                LOOPBACK_PAIRS[0].tx_iface);
        g_using_hw_timestamps = false;
    }

    uint64_t max_latency_ns = (uint64_t)max_latency_us * 1000;
    (void)get_time_ns();  // Could be used for duration tracking in the future
    int result_idx = 0;
    int failed_count = 0;
    int passed_count = 0;

    // Test each loopback port pair
    for (size_t p = 0; p < NUM_LOOPBACK_PAIRS; p++) {
        printf("Testing port pair: Port %d (%s) -> Port %d (%s)\n",
               LOOPBACK_PAIRS[p].tx_port, LOOPBACK_PAIRS[p].tx_iface,
               LOOPBACK_PAIRS[p].rx_port, LOOPBACK_PAIRS[p].rx_iface);

        int tx_ifindex, rx_ifindex;
        int tx_fd = create_raw_socket(LOOPBACK_PAIRS[p].tx_iface, &tx_ifindex, EMB_SOCK_TX);
        int rx_fd = create_raw_socket(LOOPBACK_PAIRS[p].rx_iface, &rx_ifindex, EMB_SOCK_RX);

        if (tx_fd < 0 || rx_fd < 0) {
            fprintf(stderr, "ERROR: Cannot create sockets\n");
            if (tx_fd >= 0) close(tx_fd);
            if (rx_fd >= 0) close(rx_fd);
            continue;
        }

        // Wait for sockets to initialize, then drain stale packets
        usleep(10000);  // 10ms
        drain_rx_buffer(rx_fd);

        for (int v = 0; v < LOOPBACK_PAIRS[p].vlan_count; v++) {
            struct emb_latency_result *r = &g_emb_latency.loopback_results[result_idx];

            run_single_test(tx_fd, rx_fd, tx_ifindex,
                           LOOPBACK_PAIRS[p].tx_port, LOOPBACK_PAIRS[p].rx_port,
                           LOOPBACK_PAIRS[p].vlans[v], LOOPBACK_PAIRS[p].vl_ids[v],
                           packet_count, timeout_ms, max_latency_ns, r);

            if (r->passed) {
                passed_count++;
            } else {
                failed_count++;
            }
            result_idx++;
            precise_busy_wait_us(32);
        }

        close(tx_fd);
        close(rx_fd);
    }

    // Update loopback state
    g_emb_latency.loopback_result_count = result_idx;
    g_emb_latency.loopback_completed = true;
    g_emb_latency.loopback_passed = (failed_count == 0);
    g_emb_latency.loopback_skipped = false;

    // Print results table
    emb_latency_print_loopback();

    printf("Loopback test complete: %d/%d passed\n\n", passed_count, result_idx);

    return failed_count;
}

// ============================================
// DTNIRSW PORT MAPPING & UNIT LATENCY HELPERS
// ============================================

/**
 * Map VLAN/port to dtnirsw TX/RX port numbers
 * Fiber: VLAN 97-128 maps to dtnirsw ports 0-31
 * Copper: Port 12<->13 maps to dtnirsw 32<->33
 */
static void get_dtnirsw_ports(uint16_t tx_port, uint16_t rx_port, uint16_t vlan_id,
                               uint16_t *dtnirsw_tx, uint16_t *dtnirsw_rx) {
    if (vlan_id >= 97 && vlan_id <= 128) {
        uint16_t idx = vlan_id - 97;
        *dtnirsw_tx = idx;
        *dtnirsw_rx = (idx + 16) % 32;
    } else if (tx_port == 12 && rx_port == 13) {
        *dtnirsw_tx = 32;
        *dtnirsw_rx = 33;
    } else if (tx_port == 13 && rx_port == 12) {
        *dtnirsw_tx = 33;
        *dtnirsw_rx = 32;
    } else {
        *dtnirsw_tx = tx_port;
        *dtnirsw_rx = rx_port;
    }
}

/**
 * Get store-and-forward delay for a given dtnirsw direction
 * 32->33 (Port 12->13): 1G S&F
 * 33->32 (Port 13->12): 100M S&F
 * Fiber: 1G S&F
 */
static double get_sf_delay_us(uint16_t dtnirsw_tx, uint16_t dtnirsw_rx) {
    if (dtnirsw_tx == 33 && dtnirsw_rx == 32) {
        return EMB_LAT_STORE_FWD_DELAY_100M_US;
    }
    return EMB_LAT_STORE_FWD_DELAY_1G_US;
}

/**
 * Get loopback (switch) latency for a given original TX port
 * Copper ports have no switch, returns 0
 */
static double get_loopback_latency_us(uint16_t orig_tx_port) {
    if (orig_tx_port == 12 || orig_tx_port == 13) {
        return 0.0;
    }

    if (g_emb_latency.loopback_completed && !g_emb_latency.loopback_skipped) {
        double sum = 0;
        int count = 0;
        for (uint32_t j = 0; j < g_emb_latency.loopback_result_count; j++) {
            struct emb_latency_result *r = &g_emb_latency.loopback_results[j];
            if (r->valid && r->tx_port == orig_tx_port) {
                sum += ns_to_us(r->avg_latency_ns);
                count++;
            }
        }
        if (count > 0) return sum / count;
    }

    return EMB_LAT_DEFAULT_SWITCH_US;
}

// ============================================
// UNIT TEST (Device Latency)
// ============================================

int emb_latency_run_unit_test(int packet_count, int timeout_ms, int max_latency_us) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         UNIT TEST (Device Latency)                               ║\n");
    printf("║  Port pairs: 0↔1, 2↔3, 4↔5, 6↔7                                  ║\n");
    printf("║  Packets: %-3d | Timeout: %dms | Max: %dus                      ║\n",
           packet_count, timeout_ms, max_latency_us);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Verify HW timestamp support before starting
    g_using_hw_timestamps = true;
    printf("[HW_TS] Checking HW timestamp support...\n");
    if (!check_hw_ts_support(UNIT_TEST_PAIRS[0].tx_iface)) {
        fprintf(stderr, "[WARN] HW timestamp not supported on %s - results may be inaccurate!\n",
                UNIT_TEST_PAIRS[0].tx_iface);
        g_using_hw_timestamps = false;
    }

    uint64_t max_latency_ns = (uint64_t)max_latency_us * 1000;
    (void)get_time_ns();  // Could be used for duration tracking in the future
    int result_idx = 0;
    int failed_count = 0;
    int passed_count = 0;

    // Test each unit test port pair
    for (size_t p = 0; p < NUM_UNIT_TEST_PAIRS; p++) {
        printf("Testing port pair: Port %d (%s) -> Port %d (%s)\n",
               UNIT_TEST_PAIRS[p].tx_port, UNIT_TEST_PAIRS[p].tx_iface,
               UNIT_TEST_PAIRS[p].rx_port, UNIT_TEST_PAIRS[p].rx_iface);

        int tx_ifindex, rx_ifindex;
        int tx_fd = create_raw_socket(UNIT_TEST_PAIRS[p].tx_iface, &tx_ifindex, EMB_SOCK_TX);
        int rx_fd = create_raw_socket(UNIT_TEST_PAIRS[p].rx_iface, &rx_ifindex, EMB_SOCK_RX);

        if (tx_fd < 0 || rx_fd < 0) {
            fprintf(stderr, "ERROR: Cannot create sockets\n");
            if (tx_fd >= 0) close(tx_fd);
            if (rx_fd >= 0) close(rx_fd);
            continue;
        }

        // Wait for sockets to initialize, then drain stale packets
        usleep(10000);  // 10ms
        drain_rx_buffer(rx_fd);

        for (int v = 0; v < UNIT_TEST_PAIRS[p].vlan_count; v++) {
            struct emb_latency_result *r = &g_emb_latency.unit_results[result_idx];

            run_single_test(tx_fd, rx_fd, tx_ifindex,
                           UNIT_TEST_PAIRS[p].tx_port, UNIT_TEST_PAIRS[p].rx_port,
                           UNIT_TEST_PAIRS[p].vlans[v], UNIT_TEST_PAIRS[p].vl_ids[v],
                           packet_count, timeout_ms, max_latency_ns, r);

            if (r->passed) {
                passed_count++;
            } else {
                failed_count++;
            }
            result_idx++;
            precise_busy_wait_us(32);
        }

        close(tx_fd);
        close(rx_fd);
    }

    // ==========================================
    // COPPER UNIT TEST PAIRS (no VLAN, direct connection)
    // ==========================================
    for (size_t p = 0; p < NUM_COPPER_UNIT_TEST_PAIRS; p++) {
        printf("Testing copper pair: Port %d (%s) -> Port %d (%s) [VL-ID: %d]\n",
               COPPER_UNIT_TEST_PAIRS[p].tx_port, COPPER_UNIT_TEST_PAIRS[p].tx_iface,
               COPPER_UNIT_TEST_PAIRS[p].rx_port, COPPER_UNIT_TEST_PAIRS[p].rx_iface,
               COPPER_UNIT_TEST_PAIRS[p].vl_id);

        int tx_ifindex, rx_ifindex;
        int tx_fd = create_raw_socket(COPPER_UNIT_TEST_PAIRS[p].tx_iface, &tx_ifindex, EMB_SOCK_TX);
        int rx_fd = create_raw_socket(COPPER_UNIT_TEST_PAIRS[p].rx_iface, &rx_ifindex, EMB_SOCK_RX);

        if (tx_fd < 0 || rx_fd < 0) {
            fprintf(stderr, "ERROR: Cannot create copper sockets for port %d -> %d\n",
                    COPPER_UNIT_TEST_PAIRS[p].tx_port, COPPER_UNIT_TEST_PAIRS[p].rx_port);
            if (tx_fd >= 0) close(tx_fd);
            if (rx_fd >= 0) close(rx_fd);
            continue;
        }

        usleep(10000);
        drain_rx_buffer(rx_fd);

        // Single test per copper pair (no VLAN, vlan_id = 0)
        struct emb_latency_result *r = &g_emb_latency.unit_results[result_idx];

        run_single_test(tx_fd, rx_fd, tx_ifindex,
                       COPPER_UNIT_TEST_PAIRS[p].tx_port, COPPER_UNIT_TEST_PAIRS[p].rx_port,
                       0, COPPER_UNIT_TEST_PAIRS[p].vl_id,
                       packet_count, timeout_ms, max_latency_ns, r);

        if (r->passed) {
            passed_count++;
        } else {
            failed_count++;
        }
        result_idx++;

        close(tx_fd);
        close(rx_fd);
    }

    // Update unit test state
    g_emb_latency.unit_result_count = result_idx;
    g_emb_latency.unit_completed = true;

    // Recalculate pass/fail based on unit latency (total - loopback - S&F)
    passed_count = 0;
    failed_count = 0;
    for (int i = 0; i < result_idx; i++) {
        struct emb_latency_result *r = &g_emb_latency.unit_results[i];
        if (r->rx_count > 0 && r->valid) {
            uint16_t dtnirsw_tx, dtnirsw_rx;
            get_dtnirsw_ports(r->tx_port, r->rx_port, r->vlan_id, &dtnirsw_tx, &dtnirsw_rx);
            double avg_us = ns_to_us(r->avg_latency_ns);
            double loopback_us = get_loopback_latency_us(r->tx_port);
            double sf_us = get_sf_delay_us(dtnirsw_tx, dtnirsw_rx);
            double unit_us = avg_us - loopback_us - sf_us;
            if (unit_us < 0) unit_us = 0;
            r->passed = (unit_us <= EMB_LAT_UNIT_THRESHOLD_US);
        } else {
            r->passed = false;
        }
        if (r->passed) passed_count++;
        else failed_count++;
    }
    g_emb_latency.unit_passed = (failed_count == 0);

    // Print results table
    emb_latency_print_unit();

    printf("Unit test complete: %d/%d passed (Timestamp: %s)\n\n",
           passed_count, result_idx, g_using_hw_timestamps ? "HARDWARE" : "SOFTWARE");

    return failed_count;
}

// ============================================
// COMBINED LATENCY CALCULATION
// ============================================

void emb_latency_calculate_combined(void) {
    // Fiber directions: 0→1, 1→0, 2→3, 3→2, 4→5, 5→4, 6→7, 7→6
    const uint16_t fiber_directions[][2] = {
        {0, 1}, {1, 0},
        {2, 3}, {3, 2},
        {4, 5}, {5, 4},
        {6, 7}, {7, 6}
    };
    // Copper directions: 12→13, 13→12
    const uint16_t copper_directions[][2] = {
        {12, 13}, {13, 12}
    };

    int idx = 0;

    // ---- FIBER DIRECTIONS ----
    for (int i = 0; i < 8; i++) {
        struct emb_combined_latency *c = &g_emb_latency.combined[idx];
        memset(c, 0, sizeof(*c));

        c->tx_port = fiber_directions[i][0];
        c->rx_port = fiber_directions[i][1];
        c->is_copper = false;

        // Get switch latency (from loopback or default)
        if (g_emb_latency.loopback_completed && !g_emb_latency.loopback_skipped) {
            double sum = 0;
            int count = 0;

            for (uint32_t j = 0; j < g_emb_latency.loopback_result_count; j++) {
                struct emb_latency_result *r = &g_emb_latency.loopback_results[j];
                if (r->valid && r->tx_port == c->tx_port) {
                    sum += ns_to_us(r->avg_latency_ns);
                    count++;
                }
            }

            if (count > 0) {
                c->switch_latency_us = sum / count;
                c->switch_measured = true;
            } else {
                c->switch_latency_us = EMB_LAT_DEFAULT_SWITCH_US;
                c->switch_measured = false;
            }
        } else {
            c->switch_latency_us = EMB_LAT_DEFAULT_SWITCH_US;
            c->switch_measured = false;
        }

        // Get total latency (from unit test)
        if (g_emb_latency.unit_completed) {
            double sum = 0;
            int count = 0;

            for (uint32_t j = 0; j < g_emb_latency.unit_result_count; j++) {
                struct emb_latency_result *r = &g_emb_latency.unit_results[j];
                if (r->valid &&
                    r->tx_port == c->tx_port && r->rx_port == c->rx_port) {
                    sum += ns_to_us(r->avg_latency_ns);
                    count++;
                }
            }

            if (count > 0) {
                c->total_latency_us = sum / count;
                c->total_measured = true;
            }
        }

        // Fiber formula: unit = total - loopback - S&F(1G)
        if (c->total_measured) {
            c->unit_latency_us = c->total_latency_us - c->switch_latency_us
                                 - EMB_LAT_STORE_FWD_DELAY_NIC_US;
            if (c->unit_latency_us < 0) c->unit_latency_us = 0;
            c->unit_valid = true;
            c->passed = (c->unit_latency_us <= EMB_LAT_UNIT_THRESHOLD_US);
        }
        idx++;
    }

    // ---- COPPER DIRECTIONS ----
    for (int i = 0; i < 2; i++) {
        struct emb_combined_latency *c = &g_emb_latency.combined[idx];
        memset(c, 0, sizeof(*c));

        c->tx_port = copper_directions[i][0];
        c->rx_port = copper_directions[i][1];
        c->is_copper = true;

        // Copper: no switch (direct connection)
        c->switch_latency_us = 0;
        c->switch_measured = true;  // Not applicable, but mark as known

        // Get total latency from unit test results
        if (g_emb_latency.unit_completed) {
            double sum = 0;
            int count = 0;

            for (uint32_t j = 0; j < g_emb_latency.unit_result_count; j++) {
                struct emb_latency_result *r = &g_emb_latency.unit_results[j];
                if (r->valid &&
                    r->tx_port == c->tx_port && r->rx_port == c->rx_port) {
                    sum += ns_to_us(r->avg_latency_ns);
                    count++;
                }
            }

            if (count > 0) {
                c->total_latency_us = sum / count;
                c->total_measured = true;
            }
        }

        // Copper formula: unit = total - S&F (no switch to subtract)
        // DUT copper ports are 100M, so both directions use 100M S&F
        // (server port 12 is 1G capable but autonegotiates to 100M with DUT)
        if (c->total_measured) {
            double sf_delay = EMB_LAT_STORE_FWD_DELAY_100M_US;
            c->unit_latency_us = c->total_latency_us - sf_delay;
            if (c->unit_latency_us < 0) c->unit_latency_us = 0;
            c->unit_valid = true;
            c->passed = (c->unit_latency_us <= EMB_LAT_UNIT_THRESHOLD_US);
        }
        idx++;
    }

    g_emb_latency.combined_count = idx;
}

// ============================================
// FULL INTERACTIVE SEQUENCE
// ============================================

int emb_latency_full_sequence(void) {
    int total_fails = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         LATENCY TEST SEQUENCE                                    ║\n");
    printf("║  1. Loopback Test (Mellanox switch latency measurement)          ║\n");
    printf("║  2. Unit Test (Fiber: 0↔1,2↔3,4↔5,6↔7 + Copper: 12↔13)         ║\n");
    printf("║     (Unit latency = Avg - Loopback - S&F, shown in table)        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Reset state
    memset(&g_emb_latency, 0, sizeof(g_emb_latency));

    // Debug: Compare PHC clocks across NICs before starting
    debug_compare_phc_clocks();

    // ==========================================
    // STEP 1: Loopback Test (OPTIONAL)
    // ==========================================
    printf("=== STEP 1: Loopback Test (Mellanox Switch Latency) ===\n\n");

    // Outer loop: Ask if user wants loopback test
    while (1) {
        if (ask_question("Do you want to run the Loopback test to measure Mellanox switch latency?")) {
            // User wants loopback test - ask about cables
            if (ask_question("Are the loopback cables installed?")) {
                // Cables installed - run loopback test
                int fails = emb_latency_run_loopback(1, 100, 30);
                total_fails += fails;
                break;  // Done with loopback
            } else {
                // Cables not installed - go back to outer question
                printf("\nPlease install the loopback cables first.\n\n");
                // Loop continues - will ask "Do you want loopback test?" again
            }
        } else {
            // User doesn't want loopback test - use default
            printf("Using default Mellanox switch latency: %.1f us\n\n",
                   EMB_LAT_DEFAULT_SWITCH_US);
            g_emb_latency.loopback_skipped = true;
            break;  // Done with loopback
        }
    }

    // ==========================================
    // ATE TEST MODE QUESTION
    // ==========================================
    printf("=== ATE Test Mode Selection ===\n\n");

    if (ask_question("Do you want to continue in ATE test mode?")) {
        printf("\n[ATE] ATE test mode selected.\n");
        printf("[ATE] Sending Cumulus switch ATE configuration...\n\n");

        int ate_result = ate_configure_cumulus();
        if (ate_result != 0) {
            printf("\n[ATE] ERROR: Cumulus ATE configuration failed!\n");
            printf("[ATE] Program continues but ATE config could not be applied.\n\n");
        } else {
            printf("\n[ATE] Cumulus ATE configuration applied successfully.\n\n");
        }

        // Ask for ATE test cables after config is sent
        while (!ask_question("Are the ATE test mode cables installed?")) {
            printf("\nPlease install the ATE test mode cables and try again.\n\n");
        }

        g_ate_mode = true;

        // Unit test is skipped in ATE mode
        printf("[ATE] Skipping unit test - continuing in ATE test mode.\n\n");

        g_emb_latency.test_completed = true;
        g_emb_latency.test_passed = (total_fails == 0);

        return total_fails;
    }

    printf("Continuing in normal test mode.\n\n");

    // ==========================================
    // STEP 2: Unit Test (MANDATORY - only in normal mode)
    // ==========================================
    printf("=== STEP 2: Unit Test (Device Latency) ===\n\n");
    printf("This test measures total latency through the device.\n");
    printf("Fiber pairs: 0→1, 1→0, 2→3, 3→2, 4→5, 5→4, 6→7, 7→6\n");
    printf("Copper pairs: 12→13 (1G), 13→12 (100M) [no VLAN, direct]\n\n");

    // Unit test is mandatory - wait for cables to be installed
    while (!ask_question("Are the unit test cables installed (neighboring ports connected)?")) {
        printf("\nPlease install the unit test cables and try again.\n\n");
    }

    // Run unit test (unit latency calculation is now done inside print_unit)
    int unit_fails = emb_latency_run_unit_test(1, 100, 100);  // 1 packet, 100ms timeout, 100us max
    total_fails += unit_fails;

    // Update legacy state
    g_emb_latency.test_completed = true;
    g_emb_latency.test_passed = (total_fails == 0);

    return total_fails;
}

// ============================================
// ACCESSOR FUNCTIONS
// ============================================

bool ate_mode_enabled(void) {
    return g_ate_mode;
}

bool emb_latency_completed(void) {
    return g_emb_latency.test_completed;
}

bool emb_latency_all_passed(void) {
    return g_emb_latency.test_completed && g_emb_latency.test_passed;
}

int emb_latency_get_count(void) {
    return g_emb_latency.result_count;
}

const struct emb_latency_result* emb_latency_get(int index) {
    if (index < 0 || (uint32_t)index >= g_emb_latency.result_count)
        return NULL;
    return &g_emb_latency.results[index];
}

const struct emb_latency_result* emb_latency_get_by_vlan(uint16_t vlan_id) {
    for (uint32_t i = 0; i < g_emb_latency.result_count; i++) {
        if (g_emb_latency.results[i].vlan_id == vlan_id)
            return &g_emb_latency.results[i];
    }
    return NULL;
}

bool emb_latency_get_us(uint16_t vlan_id, double *min, double *avg, double *max) {
    const struct emb_latency_result *r = emb_latency_get_by_vlan(vlan_id);
    if (!r || !r->valid) return false;

    *min = ns_to_us(r->min_latency_ns);
    *avg = ns_to_us(r->avg_latency_ns);
    *max = ns_to_us(r->max_latency_ns);
    return true;
}

// ============================================
// PRINT FUNCTIONS
// ============================================

// Table column widths (matching standalone latency_test)
#define COL_PORT    8
#define COL_VLAN    10
#define COL_VLID    10
#define COL_LAT     11
#define COL_RXTX    10
#define COL_RESULT  8

static void print_table_line(const char *left, const char *mid, const char *right, const char *fill) {
    printf("%s", left);
    for (int i = 0; i < COL_PORT; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_PORT; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_VLAN; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_VLID; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_LAT; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_LAT; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_LAT; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_RXTX; i++) printf("%s", fill);
    printf("%s", mid);
    for (int i = 0; i < COL_RESULT; i++) printf("%s", fill);
    printf("%s\n", right);
}

#define TABLE_WIDTH (COL_PORT + COL_PORT + COL_VLAN + COL_VLID + COL_LAT + COL_LAT + COL_LAT + COL_RXTX + COL_RESULT + 8)

static void print_table_title(const char *title) {
    int title_len = strlen(title);
    int padding = (TABLE_WIDTH - title_len) / 2;

    printf("║");
    for (int i = 0; i < padding; i++) printf(" ");
    printf("%s", title);
    for (int i = 0; i < TABLE_WIDTH - padding - title_len; i++) printf(" ");
    printf("║\n");
}

// Generic function to print results table
static void print_results_table(const char *title, struct emb_latency_result *results, int count) {
    // Calculate statistics
    int successful = 0;
    int passed_count = 0;
    double total_avg_latency = 0.0;
    double min_of_mins = 1e9;
    double max_of_maxs = 0.0;

    for (int i = 0; i < count; i++) {
        struct emb_latency_result *r = &results[i];
        if (r->rx_count > 0) {
            successful++;
            double avg = ns_to_us(r->avg_latency_ns);
            total_avg_latency += avg;

            double min_lat = ns_to_us(r->min_latency_ns);
            double max_lat = ns_to_us(r->max_latency_ns);
            if (min_lat < min_of_mins) min_of_mins = min_lat;
            if (max_lat > max_of_maxs) max_of_maxs = max_lat;
        }
        if (r->passed) passed_count++;
    }

    printf("\n");
    fflush(stdout);

    // Top border
    print_table_line("╔", "╦", "╗", "═");

    // Title
    print_table_title(title);

    // Header separator
    print_table_line("╠", "╬", "╣", "═");

    // Header row
    printf("║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║\n",
           COL_PORT, "TX Port",
           COL_PORT, "RX Port",
           COL_VLAN, "VLAN",
           COL_VLID, "VL-ID",
           COL_LAT, "Min (us)",
           COL_LAT, "Avg (us)",
           COL_LAT, "Max (us)",
           COL_RXTX, "RX/TX",
           COL_RESULT, "Result");

    // Header bottom separator
    print_table_line("╠", "╬", "╣", "═");

    // Data rows
    for (int i = 0; i < count; i++) {
        struct emb_latency_result *r = &results[i];

        char min_str[16], avg_str[16], max_str[16], rxtx_str[16];
        const char *result_str = r->passed ? "PASS" : "FAIL";

        if (r->rx_count > 0) {
            snprintf(min_str, sizeof(min_str), "%9.2f", ns_to_us(r->min_latency_ns));
            snprintf(avg_str, sizeof(avg_str), "%9.2f", ns_to_us(r->avg_latency_ns));
            snprintf(max_str, sizeof(max_str), "%9.2f", ns_to_us(r->max_latency_ns));
        } else {
            snprintf(min_str, sizeof(min_str), "%9s", "-");
            snprintf(avg_str, sizeof(avg_str), "%9s", "-");
            snprintf(max_str, sizeof(max_str), "%9s", "-");
        }
        snprintf(rxtx_str, sizeof(rxtx_str), "%4u/%-4u", r->rx_count, r->tx_count);

        printf("║%*u║%*u║%*u║%*u║%*s║%*s║%*s║%*s║%*s║\n",
               COL_PORT, r->tx_port,
               COL_PORT, r->rx_port,
               COL_VLAN, r->vlan_id,
               COL_VLID, r->vl_id,
               COL_LAT, min_str,
               COL_LAT, avg_str,
               COL_LAT, max_str,
               COL_RXTX, rxtx_str,
               COL_RESULT, result_str);
    }

    // Summary separator
    print_table_line("╠", "╩", "╣", "═");

    // Summary line
    char summary[128];
    if (successful > 0) {
        snprintf(summary, sizeof(summary),
                "SUMMARY: PASS %d/%d | Avg: %.2f us | Max: %.2f us | Packets/VLAN: 1",
                passed_count, count,
                total_avg_latency / successful,
                max_of_maxs);
    } else {
        snprintf(summary, sizeof(summary),
                "SUMMARY: PASS %d/%d | Packets/VLAN: 1",
                passed_count, count);
    }
    print_table_title(summary);

    // Bottom border
    print_table_line("╚", "╩", "╝", "═");

    printf("\n");
    fflush(stdout);
}

void emb_latency_print(void) {
    char title[128];
    snprintf(title, sizeof(title), "LATENCY TEST RESULTS (Timestamp: %s)",
             g_using_hw_timestamps ? "HARDWARE NIC" : "SOFTWARE - INACCURATE!");
    print_results_table(title, g_emb_latency.results, g_emb_latency.result_count);
}

void emb_latency_print_loopback(void) {
    struct emb_latency_result *results = g_emb_latency.loopback_results;
    int count = g_emb_latency.loopback_result_count;

    // Calculate statistics
    int successful = 0;
    int passed_count = 0;
    double total_avg_latency = 0.0;
    double max_of_maxs = 0.0;

    for (int i = 0; i < count; i++) {
        struct emb_latency_result *r = &results[i];
        if (r->rx_count > 0) {
            successful++;
            double avg = ns_to_us(r->avg_latency_ns);
            total_avg_latency += avg;
            double max_lat = ns_to_us(r->max_latency_ns);
            if (max_lat > max_of_maxs) max_of_maxs = max_lat;
        }
        if (r->passed) passed_count++;
    }

    printf("\n");
    fflush(stdout);

    char title[128];
    snprintf(title, sizeof(title), "LOOPBACK TEST RESULTS (Switch Latency) [TS: %s]",
             g_using_hw_timestamps ? "HW" : "SW!");

    // Top border
    print_table_line("╔", "╦", "╗", "═");
    print_table_title(title);
    print_table_line("╠", "╬", "╣", "═");

    // Header row
    printf("║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║\n",
           COL_PORT, "TX Port",
           COL_PORT, "RX Port",
           COL_VLAN, "VLAN",
           COL_VLID, "VL-ID",
           COL_LAT, "Min (us)",
           COL_LAT, "Avg (us)",
           COL_LAT, "Max (us)",
           COL_RXTX, "RX/TX",
           COL_RESULT, "Result");

    print_table_line("╠", "╬", "╣", "═");

    // Build sorted index array by dtnirsw TX port (ascending)
    int sorted_idx[count];
    uint16_t sorted_tx[count];
    for (int i = 0; i < count; i++) {
        sorted_idx[i] = i;
        uint16_t dtx, drx;
        get_dtnirsw_ports(results[i].tx_port, results[i].rx_port, results[i].vlan_id, &dtx, &drx);
        sorted_tx[i] = dtx;
    }
    for (int i = 1; i < count; i++) {
        int key_idx = sorted_idx[i];
        uint16_t key_tx = sorted_tx[i];
        int j = i - 1;
        while (j >= 0 && sorted_tx[j] > key_tx) {
            sorted_idx[j + 1] = sorted_idx[j];
            sorted_tx[j + 1] = sorted_tx[j];
            j--;
        }
        sorted_idx[j + 1] = key_idx;
        sorted_tx[j + 1] = key_tx;
    }

    // Data rows (sorted by dtnirsw TX port)
    for (int si = 0; si < count; si++) {
        struct emb_latency_result *r = &results[sorted_idx[si]];

        uint16_t dtnirsw_tx, dtnirsw_rx;
        get_dtnirsw_ports(r->tx_port, r->rx_port, r->vlan_id, &dtnirsw_tx, &dtnirsw_rx);

        char min_str[16], avg_str[16], max_str[16], rxtx_str[16];
        const char *result_str = r->passed ? "PASS" : "FAIL";

        if (r->rx_count > 0) {
            snprintf(min_str, sizeof(min_str), "%9.2f", ns_to_us(r->min_latency_ns));
            snprintf(avg_str, sizeof(avg_str), "%9.2f", ns_to_us(r->avg_latency_ns));
            snprintf(max_str, sizeof(max_str), "%9.2f", ns_to_us(r->max_latency_ns));
        } else {
            snprintf(min_str, sizeof(min_str), "%9s", "-");
            snprintf(avg_str, sizeof(avg_str), "%9s", "-");
            snprintf(max_str, sizeof(max_str), "%9s", "-");
        }
        snprintf(rxtx_str, sizeof(rxtx_str), "%4u/%-4u", r->rx_count, r->tx_count);

        printf("║%*u║%*u║%*u║%*u║%*s║%*s║%*s║%*s║%*s║\n",
               COL_PORT, dtnirsw_tx,
               COL_PORT, dtnirsw_rx,
               COL_VLAN, r->vlan_id,
               COL_VLID, r->vl_id,
               COL_LAT, min_str,
               COL_LAT, avg_str,
               COL_LAT, max_str,
               COL_RXTX, rxtx_str,
               COL_RESULT, result_str);
    }

    // Summary
    print_table_line("╠", "╩", "╣", "═");
    char summary[128];
    if (successful > 0) {
        snprintf(summary, sizeof(summary),
                "SUMMARY: PASS %d/%d | Avg: %.2f us | Max: %.2f us | Packets/VLAN: 1",
                passed_count, count,
                total_avg_latency / successful,
                max_of_maxs);
    } else {
        snprintf(summary, sizeof(summary),
                "SUMMARY: PASS %d/%d | Packets/VLAN: 1",
                passed_count, count);
    }
    print_table_title(summary);
    print_table_line("╚", "╩", "╝", "═");

    printf("\n");
    fflush(stdout);
}

void emb_latency_print_unit(void) {
    struct emb_latency_result *results = g_emb_latency.unit_results;
    int count = g_emb_latency.unit_result_count;

    // Column widths for unit test table (10 columns)
    #define UT_COL_PORT    8
    #define UT_COL_VLAN    10
    #define UT_COL_VLID    10
    #define UT_COL_LAT     11
    #define UT_COL_RXTX    10
    #define UT_COL_RESULT  8

    // 10 columns: TX Port, RX Port, VLAN, VL-ID, Min, Avg, Max, Unit, RX/TX, Result
    #define UT_TABLE_WIDTH (UT_COL_PORT + UT_COL_PORT + UT_COL_VLAN + UT_COL_VLID + \
                            UT_COL_LAT + UT_COL_LAT + UT_COL_LAT + UT_COL_LAT + \
                            UT_COL_RXTX + UT_COL_RESULT + 9)

    // Line drawing helper (10 columns)
    #define UT_PRINT_LINE(L, M, R, F) do { \
        printf("%s", L); \
        for (int _i = 0; _i < UT_COL_PORT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_PORT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_VLAN; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_VLID; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_LAT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_LAT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_LAT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_LAT; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_RXTX; _i++) printf("%s", F); printf("%s", M); \
        for (int _i = 0; _i < UT_COL_RESULT; _i++) printf("%s", F); printf("%s\n", R); \
    } while(0)

    // Calculate statistics
    int successful = 0;
    int passed_count = 0;
    double total_avg_latency = 0.0;
    double max_of_maxs = 0.0;

    for (int i = 0; i < count; i++) {
        struct emb_latency_result *r = &results[i];
        if (r->rx_count > 0) {
            successful++;
            double avg = ns_to_us(r->avg_latency_ns);
            total_avg_latency += avg;
            double max_lat = ns_to_us(r->max_latency_ns);
            if (max_lat > max_of_maxs) max_of_maxs = max_lat;
        }
        if (r->passed) passed_count++;
    }

    printf("\n");
    fflush(stdout);

    // Title
    char title[128];
    snprintf(title, sizeof(title), "UNIT TEST RESULTS (Device Latency) [TS: %s]",
             g_using_hw_timestamps ? "HW" : "SW!");

    // Top border
    UT_PRINT_LINE("╔", "╦", "╗", "═");

    // Title line
    {
        int title_len = strlen(title);
        int padding = (UT_TABLE_WIDTH - title_len) / 2;
        printf("║");
        for (int i = 0; i < padding; i++) printf(" ");
        printf("%s", title);
        for (int i = 0; i < UT_TABLE_WIDTH - padding - title_len; i++) printf(" ");
        printf("║\n");
    }

    // Header separator
    UT_PRINT_LINE("╠", "╬", "╣", "═");

    // Header row
    printf("║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║%*s║\n",
           UT_COL_PORT, "TX Port",
           UT_COL_PORT, "RX Port",
           UT_COL_VLAN, "VLAN",
           UT_COL_VLID, "VL-ID",
           UT_COL_LAT, "Min (us)",
           UT_COL_LAT, "Avg (us)",
           UT_COL_LAT, "Max (us)",
           UT_COL_LAT, "Unit (us)",
           UT_COL_RXTX, "RX/TX",
           UT_COL_RESULT, "Result");

    // Header bottom separator
    UT_PRINT_LINE("╠", "╬", "╣", "═");

    // Build sorted index array by dtnirsw TX port (ascending)
    int sorted_idx[count];
    uint16_t sorted_tx[count];
    for (int i = 0; i < count; i++) {
        sorted_idx[i] = i;
        uint16_t dtx, drx;
        get_dtnirsw_ports(results[i].tx_port, results[i].rx_port, results[i].vlan_id, &dtx, &drx);
        sorted_tx[i] = dtx;
    }
    // Simple insertion sort (count is small, ~34)
    for (int i = 1; i < count; i++) {
        int key_idx = sorted_idx[i];
        uint16_t key_tx = sorted_tx[i];
        int j = i - 1;
        while (j >= 0 && sorted_tx[j] > key_tx) {
            sorted_idx[j + 1] = sorted_idx[j];
            sorted_tx[j + 1] = sorted_tx[j];
            j--;
        }
        sorted_idx[j + 1] = key_idx;
        sorted_tx[j + 1] = key_tx;
    }

    // Data rows (sorted by dtnirsw TX port)
    for (int si = 0; si < count; si++) {
        struct emb_latency_result *r = &results[sorted_idx[si]];

        // Get dtnirsw port numbers
        uint16_t dtnirsw_tx, dtnirsw_rx;
        get_dtnirsw_ports(r->tx_port, r->rx_port, r->vlan_id, &dtnirsw_tx, &dtnirsw_rx);

        // Get loopback and S&F for unit latency calculation
        double loopback_us = get_loopback_latency_us(r->tx_port);
        double sf_us = get_sf_delay_us(dtnirsw_tx, dtnirsw_rx);

        char min_str[16], avg_str[16], max_str[16], unit_str[16], rxtx_str[16];

        if (r->rx_count > 0) {
            double avg_us = ns_to_us(r->avg_latency_ns);
            double unit_us = avg_us - loopback_us - sf_us;
            if (unit_us < 0) unit_us = 0;

            snprintf(min_str, sizeof(min_str), "%9.2f", ns_to_us(r->min_latency_ns));
            snprintf(avg_str, sizeof(avg_str), "%9.2f", avg_us);
            snprintf(max_str, sizeof(max_str), "%9.2f", ns_to_us(r->max_latency_ns));
            snprintf(unit_str, sizeof(unit_str), "%9.2f", unit_us);
        } else {
            snprintf(min_str, sizeof(min_str), "%9s", "-");
            snprintf(avg_str, sizeof(avg_str), "%9s", "-");
            snprintf(max_str, sizeof(max_str), "%9s", "-");
            snprintf(unit_str, sizeof(unit_str), "%9s", "-");
        }
        snprintf(rxtx_str, sizeof(rxtx_str), "%4u/%-4u", r->rx_count, r->tx_count);

        const char *result_str = r->passed ? "PASS" : "FAIL";

        printf("║%*u║%*u║%*u║%*u║%*s║%*s║%*s║%*s║%*s║%*s║\n",
               UT_COL_PORT, dtnirsw_tx,
               UT_COL_PORT, dtnirsw_rx,
               UT_COL_VLAN, r->vlan_id,
               UT_COL_VLID, r->vl_id,
               UT_COL_LAT, min_str,
               UT_COL_LAT, avg_str,
               UT_COL_LAT, max_str,
               UT_COL_LAT, unit_str,
               UT_COL_RXTX, rxtx_str,
               UT_COL_RESULT, result_str);
    }

    // Summary separator
    UT_PRINT_LINE("╠", "╩", "╣", "═");

    // Summary line
    {
        char summary[256];
        if (successful > 0) {
            snprintf(summary, sizeof(summary),
                    "SUMMARY: PASS %d/%d | Avg: %.2f us | Max: %.2f us | Packets/VLAN: 1",
                    passed_count, count,
                    total_avg_latency / successful,
                    max_of_maxs);
        } else {
            snprintf(summary, sizeof(summary),
                    "SUMMARY: PASS %d/%d | Packets/VLAN: 1",
                    passed_count, count);
        }
        int slen = strlen(summary);
        int spad = (UT_TABLE_WIDTH - slen) / 2;
        printf("║");
        for (int i = 0; i < spad; i++) printf(" ");
        printf("%s", summary);
        for (int i = 0; i < UT_TABLE_WIDTH - spad - slen; i++) printf(" ");
        printf("║\n");
    }

    // Formula info line
    {
        const char *lb_source = (g_emb_latency.loopback_completed && !g_emb_latency.loopback_skipped)
                                 ? "Measured" : "Default (14us)";
        char info[256];
        snprintf(info, sizeof(info),
                "Unit = Avg - Loopback(%s) - S&F | Threshold: %.1f us",
                lb_source, EMB_LAT_UNIT_THRESHOLD_US);
        int ilen = strlen(info);
        int ipad = (UT_TABLE_WIDTH - ilen) / 2;
        printf("║");
        for (int i = 0; i < ipad; i++) printf(" ");
        printf("%s", info);
        for (int i = 0; i < UT_TABLE_WIDTH - ipad - ilen; i++) printf(" ");
        printf("║\n");
    }

    // Bottom border
    UT_PRINT_LINE("╚", "╩", "╝", "═");

    printf("\n");
    fflush(stdout);

    #undef UT_COL_PORT
    #undef UT_COL_VLAN
    #undef UT_COL_VLID
    #undef UT_COL_LAT
    #undef UT_COL_RXTX
    #undef UT_COL_RESULT
    #undef UT_TABLE_WIDTH
    #undef UT_PRINT_LINE
}

void emb_latency_print_summary(void) {
    printf("║  SUMMARY: %u/%u PASSED | Min: %.2f us | Avg: %.2f us | Max: %.2f us | Duration: %.1f ms  ║\n",
           g_emb_latency.passed_count,
           g_emb_latency.result_count,
           ns_to_us(g_emb_latency.overall_min_ns),
           ns_to_us(g_emb_latency.overall_avg_ns),
           ns_to_us(g_emb_latency.overall_max_ns),
           g_emb_latency.test_duration_ns / 1000000.0);
}

void emb_latency_print_combined(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                              COMBINED LATENCY RESULTS (Per Direction)                                    ║\n");
    printf("╠═══════════╦═══════════╦══════════════╦══════════════╦══════════════╦══════════════╦═══════════════════════╣\n");
    printf("║ Direction ║  Source   ║ Loopback(µs) ║  Total (µs)  ║ S&F Corr(µs) ║  Unit (µs)  ║       Status        ║\n");
    printf("╠═══════════╬═══════════╬══════════════╬══════════════╬══════════════╬══════════════╬═══════════════════════╣\n");

    int pass_count = 0;
    int fail_count = 0;

    for (uint32_t i = 0; i < g_emb_latency.combined_count; i++) {
        struct emb_combined_latency *c = &g_emb_latency.combined[i];

        const char *status;
        if (!c->unit_valid) {
            status = "  N/A  ";
        } else if (c->passed) {
            status = " PASS (<30us)";
            pass_count++;
        } else {
            status = " FAIL (>30us)";
            fail_count++;
        }

        // Determine S&F correction for this row
        double sf_delay;
        if (c->is_copper) {
            sf_delay = EMB_LAT_STORE_FWD_DELAY_100M_US;  // DUT copper ports are 100M
        } else {
            sf_delay = EMB_LAT_STORE_FWD_DELAY_NIC_US;
        }

        const char *source;
        if (c->is_copper) {
            source = "copper";
        } else {
            source = c->switch_measured ? "measured" : "default";
        }

        printf("║  %2u → %2u  ║ %-9s ║    %8.2f  ║    %8.2f  ║    %8.2f  ║    %8.2f  ║ %s ║\n",
               c->tx_port, c->rx_port,
               source,
               c->switch_latency_us,
               c->total_measured ? c->total_latency_us : 0.0,
               sf_delay,
               c->unit_valid ? c->unit_latency_us : 0.0,
               status);
    }

    printf("╠═══════════╩═══════════╩══════════════╩══════════════╩══════════════╩══════════════╩═══════════════════════╣\n");
    printf("║  SUMMARY: %d PASS / %d FAIL  |  Threshold: %.1f µs                                                      ║\n",
           pass_count, fail_count, EMB_LAT_UNIT_THRESHOLD_US);
    printf("║  Fiber S&F: %.3f µs (1G)  |  Copper S&F: %.3f µs (1G) / %.2f µs (100M)                          ║\n",
           EMB_LAT_STORE_FWD_DELAY_NIC_US, EMB_LAT_STORE_FWD_DELAY_1G_US, EMB_LAT_STORE_FWD_DELAY_100M_US);
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("Formula:\n");
    printf("  Fiber:  Unit = Total - Loopback - S&F(1G)\n");
    printf("  Copper: Unit = Total - S&F (no switch, direct connection)\n");
    printf("  DUT is store-and-forward (buffers entire frame before forwarding)\n");
    printf("Switch latency source: %s\n",
           g_emb_latency.loopback_skipped ? "Default (14 µs)" : "Measured (Loopback test)");
    printf("Timestamp source: %s\n\n",
           g_using_hw_timestamps ? "HARDWARE (NIC PTP clock)" : "SOFTWARE (kernel) - results may be ~10us higher!");
}

// ============================================
// COMBINED LATENCY ACCESSORS
// ============================================

const struct emb_combined_latency* emb_latency_get_combined(uint16_t tx_port) {
    for (uint32_t i = 0; i < g_emb_latency.combined_count; i++) {
        if (g_emb_latency.combined[i].tx_port == tx_port)
            return &g_emb_latency.combined[i];
    }
    return NULL;
}

/**
 * Get combined latency for a specific direction (tx_port → rx_port)
 */
const struct emb_combined_latency* emb_latency_get_combined_direction(uint16_t tx_port, uint16_t rx_port) {
    for (uint32_t i = 0; i < g_emb_latency.combined_count; i++) {
        if (g_emb_latency.combined[i].tx_port == tx_port &&
            g_emb_latency.combined[i].rx_port == rx_port)
            return &g_emb_latency.combined[i];
    }
    return NULL;
}

bool emb_latency_get_unit_us(uint16_t tx_port, double *unit_latency_us) {
    const struct emb_combined_latency *c = emb_latency_get_combined(tx_port);
    if (!c || !c->unit_valid) return false;

    *unit_latency_us = c->unit_latency_us;
    return true;
}

bool emb_latency_get_all_us(uint16_t tx_port, double *switch_us, double *total_us, double *unit_us) {
    const struct emb_combined_latency *c = emb_latency_get_combined(tx_port);
    if (!c || !c->unit_valid) return false;

    *switch_us = c->switch_latency_us;
    *total_us = c->total_latency_us;
    *unit_us = c->unit_latency_us;
    return true;
}