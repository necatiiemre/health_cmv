#include "Packet.h"
#include "Port.h"
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>

// Global PRBS cache for all ports
struct prbs_cache port_prbs_cache[MAX_PRBS_CACHE_PORTS];

/**
 * PRBS-31 next bit generator
 * Polynomial: x^31 + x^28 + 1
 * Taps at positions 31 and 28
 */
static inline uint32_t prbs31_next(uint32_t *state)
{
    uint32_t output = *state & 0x01;
    // XOR bit 0 and bit 3 (for 31 and 28 positions)
    uint32_t new_bit = ((*state & 0x01) ^ ((*state >> 3) & 0x01)) & 0x01;
    // Shift right and insert new bit at position 30
    *state = (new_bit << 30 | (*state >> 1)) & 0x7FFFFFFF;
    return output;
}

/**
 * Fill buffer with PRBS-31 sequence
 * 
 * @param buffer Destination buffer
 * @param initial_state Initial PRBS-31 state (31-bit)
 */
static void fill_buffer_with_prbs31(uint8_t *buffer, const uint32_t initial_state)
{
    uint32_t state = initial_state;
    
    printf("Generating PRBS-31 sequence (this may take a minute)...\n");
    
    for (size_t i = 0; i < PRBS_CACHE_SIZE; i++) {
        buffer[i] = 0;
        // Generate 8 bits per byte
        for (int j = 0; j < 8; j++) {
            buffer[i] = (buffer[i] << 1) | prbs31_next(&state);
        }
        
        // Progress indicator every 10MB
        if ((i % (10 * 1024 * 1024)) == 0 && i > 0) {
            printf("  Generated %zu MB / %u MB (%.1f%%)\n", 
                   i / (1024 * 1024), 
                   (unsigned)(PRBS_CACHE_SIZE / (1024 * 1024)),
                   (100.0 * i) / PRBS_CACHE_SIZE);
        }
    }
    
    printf("PRBS-31 generation complete!\n");
}

/**
 * Initialize PRBS cache for all ports
 */
void init_prbs_cache_for_all_ports(uint16_t nb_ports, const struct ports_config *ports)
{
    printf("\n=== Initializing PRBS-31 Cache ===\n");
    printf("Cache size per port: %u MB\n", (unsigned)(PRBS_CACHE_SIZE / (1024 * 1024)));
    printf("Extended cache: +%u bytes for wraparound\n", NUM_PRBS_BYTES);
    
    for (uint16_t port = 0; port < nb_ports && port < MAX_PRBS_CACHE_PORTS; port++) {
        printf("\nPort %u:\n", port);
        
        // Get NUMA socket for this port
        int socket_id = 0;
        if (ports) {
            socket_id = ports->ports[port].numa_node;
        }
        
        port_prbs_cache[port].socket_id = socket_id;
        
        // Unique initial state for each port (0x0000000F + port_id)
        port_prbs_cache[port].initial_state = 0x0000000F + port;
        
        printf("  NUMA socket: %d\n", socket_id);
        printf("  Initial PRBS state: 0x%08X\n", port_prbs_cache[port].initial_state);
        
        // Allocate main cache on correct NUMA node
        port_prbs_cache[port].cache = (uint8_t *)rte_malloc_socket(
            NULL, 
            PRBS_CACHE_SIZE, 
            0, 
            socket_id
        );
        
        if (!port_prbs_cache[port].cache) {
            printf("Error: Failed to allocate PRBS cache for port %u\n", port);
            port_prbs_cache[port].initialized = false;
            continue;
        }
        
        // Allocate extended cache (main + extra bytes for wraparound)
        size_t ext_size = (size_t)PRBS_CACHE_SIZE + (size_t)NUM_PRBS_BYTES;
        port_prbs_cache[port].cache_ext = (uint8_t *)rte_malloc_socket(
            NULL,
            ext_size,
            0,
            socket_id
        );
        
        if (!port_prbs_cache[port].cache_ext) {
            printf("Error: Failed to allocate extended PRBS cache for port %u\n", port);
            rte_free(port_prbs_cache[port].cache);
            port_prbs_cache[port].cache = NULL;
            port_prbs_cache[port].initialized = false;
            continue;
        }
        
        // Generate PRBS-31 sequence
        fill_buffer_with_prbs31(port_prbs_cache[port].cache, 
                                port_prbs_cache[port].initial_state);
        
        // Copy to extended cache (main + wraparound bytes)
        rte_memcpy(port_prbs_cache[port].cache_ext, 
                   port_prbs_cache[port].cache, 
                   PRBS_CACHE_SIZE);
        rte_memcpy(port_prbs_cache[port].cache_ext + PRBS_CACHE_SIZE,
                   port_prbs_cache[port].cache,
                   NUM_PRBS_BYTES);
        
        port_prbs_cache[port].initialized = true;

        printf("  Status: PRBS cache initialized successfully\n");

        // Debug: Show first 4 iterations
        {
            uint32_t debug_state = port_prbs_cache[port].initial_state;
            printf("  First 4 iterations:\n");
            for (int iter = 0; iter < 4; iter++) {
                uint32_t state_before = debug_state;
                uint32_t output = debug_state & 0x01;
                uint32_t new_bit = ((debug_state & 0x01) ^ ((debug_state >> 3) & 0x01)) & 0x01;
                debug_state = (new_bit << 30 | (debug_state >> 1)) & 0x7FFFFFFF;
                printf("    [%d] state=0x%08X -> output=%u, new_bit=%u -> state=0x%08X\n",
                       iter, state_before, output, new_bit, debug_state);
            }
        }
    }
    
    printf("\nTotal PRBS cache memory: %.2f GB\n", 
           (nb_ports * PRBS_CACHE_SIZE) / (1024.0 * 1024.0 * 1024.0));
    printf("PRBS cache initialization complete\n\n");
}

uint8_t* get_prbs_cache_for_port(uint16_t port_id)
{
    if (port_id >= MAX_PRBS_CACHE_PORTS) {
        printf("Error: Invalid port_id %u for PRBS cache\n", port_id);
        return NULL;
    }
    
    if (!port_prbs_cache[port_id].initialized) {
        printf("Error: PRBS cache not initialized for port %u\n", port_id);
        return NULL;
    }
    
    return port_prbs_cache[port_id].cache;
}

uint8_t* get_prbs_cache_ext_for_port(uint16_t port_id)
{
    if (port_id >= MAX_PRBS_CACHE_PORTS) {
        printf("Error: Invalid port_id %u for PRBS cache\n", port_id);
        return NULL;
    }
    
    if (!port_prbs_cache[port_id].initialized) {
        printf("Error: PRBS cache not initialized for port %u\n", port_id);
        return NULL;
    }
    
    return port_prbs_cache[port_id].cache_ext;
}

void fill_payload_with_prbs31_dynamic(struct rte_mbuf *mbuf, uint16_t port_id,
                                       uint64_t sequence_number, uint16_t l2_len,
                                       uint16_t prbs_len)
{
    // Safety checks
    if (unlikely(!mbuf)) {
        printf("Error: NULL mbuf in fill_payload_with_prbs31_dynamic\n");
        return;
    }

    if (unlikely(port_id >= MAX_PRBS_CACHE_PORTS)) {
        printf("Error: Invalid port_id %u in fill_payload_with_prbs31_dynamic\n", port_id);
        return;
    }

    if (unlikely(!port_prbs_cache[port_id].initialized)) {
        printf("Error: PRBS cache not initialized for port %u\n", port_id);
        return;
    }

    if (unlikely(!port_prbs_cache[port_id].cache_ext)) {
        printf("Error: PRBS cache_ext is NULL for port %u\n", port_id);
        return;
    }

    const uint32_t payload_offset = l2_len + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);

    // Check if offset is valid (dynamic prbs_len validation)
    if (unlikely(payload_offset + SEQ_BYTES + prbs_len > rte_pktmbuf_data_len(mbuf))) {
        printf("Error: Invalid payload offset %u + seq %u + prbs %u > data_len %u\n",
               payload_offset, SEQ_BYTES, prbs_len, rte_pktmbuf_data_len(mbuf));
        return;
    }

    // Write sequence number (first 8 bytes of payload)
    uint64_t *seq_ptr = rte_pktmbuf_mtod_offset(mbuf, uint64_t *, payload_offset);
    *seq_ptr = sequence_number;

    // PRBS data starts after sequence number
    uint8_t *prbs_ptr = rte_pktmbuf_mtod_offset(mbuf, uint8_t *, payload_offset + SEQ_BYTES);

    // IMIX: PRBS offset is ALWAYS calculated with MAX_PRBS_BYTES
    // This way the RX side can calculate the offset from the sequence
    const uint64_t start_offset = (sequence_number * (uint64_t)MAX_PRBS_BYTES) % (uint64_t)PRBS_CACHE_SIZE;

    // Use extended cache - copy only prbs_len bytes
    rte_memcpy(prbs_ptr,
               &port_prbs_cache[port_id].cache_ext[start_offset],
               prbs_len);
}

void cleanup_prbs_cache(void)
{
    printf("Cleaning up PRBS cache...\n");
    
    for (uint16_t port = 0; port < MAX_PRBS_CACHE_PORTS; port++) {
        if (port_prbs_cache[port].initialized) {
            if (port_prbs_cache[port].cache) {
                rte_free(port_prbs_cache[port].cache);
                port_prbs_cache[port].cache = NULL;
            }
            
            if (port_prbs_cache[port].cache_ext) {
                rte_free(port_prbs_cache[port].cache_ext);
                port_prbs_cache[port].cache_ext = NULL;
            }
            
            port_prbs_cache[port].initialized = false;
        }
    }
    
    printf("PRBS cache cleanup complete\n");
}

// This is continuation of packet_manager.c - append this to the previous part

void init_packet_config(struct packet_config *config)
{
    if (!config) {
        return;
    }
    
    memset(config, 0, sizeof(struct packet_config));
    
#if VLAN_ENABLED
    config->vlan_id = 100;      // VLAN header tag (will be set by caller)
    config->vlan_priority = 0;
#endif
    
    config->vl_id = 0;          // VL ID (will be set by caller)
    
    // Source MAC: 02:00:00:00:00:20 (FIXED)
    config->src_mac.addr_bytes[0] = 0x02;
    config->src_mac.addr_bytes[1] = 0x00;
    config->src_mac.addr_bytes[2] = 0x00;
    config->src_mac.addr_bytes[3] = 0x00;
    config->src_mac.addr_bytes[4] = 0x00;
    config->src_mac.addr_bytes[5] = 0x20;
    
    // Destination MAC: 03:00:00:00:XX:XX (last 2 bytes = VL ID, will be set by caller)
    config->dst_mac.addr_bytes[0] = 0x03;
    config->dst_mac.addr_bytes[1] = 0x00;
    config->dst_mac.addr_bytes[2] = 0x00;
    config->dst_mac.addr_bytes[3] = 0x00;
    config->dst_mac.addr_bytes[4] = 0x00;
    config->dst_mac.addr_bytes[5] = 0x00;
    
    // Source IP: 10.0.0.0 (FIXED)
    config->src_ip = (10 << 24) | (0 << 16) | (0 << 8) | 0;
    
    // Destination IP: 224.224.XX.XX (last 2 bytes = VL ID, will be set by caller)
    config->dst_ip = (224 << 24) | (224 << 16) | (0 << 8) | 0;
    
    config->ttl = 0x01;  // 0x01
    config->tos = 0x00;
    
    config->src_port = 100;  // 100
    config->dst_port = 100;  // 100
    
    config->payload_data = NULL;
    config->payload_size = 0;
}

int build_packet(struct packet_template *template, const struct packet_config *config)
{
    if (!template || !config) {
        return -1;
    }
    
    memset(template, 0, sizeof(struct packet_template));
    
    // Build Ethernet header
    memcpy(&template->eth.dst_addr, &config->dst_mac, sizeof(struct rte_ether_addr));
    memcpy(&template->eth.src_addr, &config->src_mac, sizeof(struct rte_ether_addr));
    
#if VLAN_ENABLED
    // Ethernet type for VLAN
    template->eth.ether_type = rte_cpu_to_be_16(ETHER_TYPE_VLAN);
    
    // VLAN header
    uint16_t tci = ((config->vlan_priority & 0x07) << 13) | (config->vlan_id & 0x0FFF);
    template->vlan.tci = rte_cpu_to_be_16(tci);
    template->vlan.eth_proto = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    
    // IP header
    template->ip.version_ihl = 0x45;  // Version 4, IHL 5
    template->ip.type_of_service = config->tos;
    template->ip.total_length = rte_cpu_to_be_16(IP_HDR_SIZE + UDP_HDR_SIZE + PAYLOAD_SIZE_VLAN);
    template->ip.packet_id = 0;
    template->ip.fragment_offset = 0;
    template->ip.time_to_live = config->ttl;
    template->ip.next_proto_id = IPPROTO_UDP;  // 0x11
    template->ip.hdr_checksum = 0;
    template->ip.src_addr = rte_cpu_to_be_32(config->src_ip);
    template->ip.dst_addr = rte_cpu_to_be_32(config->dst_ip);
    
    // Calculate IP checksum
    struct rte_ipv4_hdr ip_copy;
    memcpy(&ip_copy, &template->ip, sizeof(struct rte_ipv4_hdr));
    uint16_t ip_csum = calculate_ip_checksum(&ip_copy);
    template->ip.hdr_checksum = ip_csum;
    
    // UDP header
    template->udp.src_port = rte_cpu_to_be_16(config->src_port);
    template->udp.dst_port = rte_cpu_to_be_16(config->dst_port);
    template->udp.dgram_len = rte_cpu_to_be_16(UDP_HDR_SIZE + PAYLOAD_SIZE_VLAN);
    template->udp.dgram_cksum = 0;
    
    // Payload will be filled by fill_payload_with_prbs31()
    if (config->payload_data && config->payload_size > 0) {
        uint16_t copy_size = (config->payload_size < PAYLOAD_SIZE_VLAN) ? 
                             config->payload_size : PAYLOAD_SIZE_VLAN;
        memcpy(template->payload, config->payload_data, copy_size);
    }
#else
    // Ethernet type for IPv4
    template->eth.ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    
    // IP header
    template->ip.version_ihl = 0x45;  // Version 4, IHL 5
    template->ip.type_of_service = config->tos;
    template->ip.total_length = rte_cpu_to_be_16(IP_HDR_SIZE + UDP_HDR_SIZE + PAYLOAD_SIZE_NO_VLAN);
    template->ip.packet_id = 0;
    template->ip.fragment_offset = 0;
    template->ip.time_to_live = config->ttl;
    template->ip.next_proto_id = IPPROTO_UDP;  // 0x11
    template->ip.hdr_checksum = 0;
    template->ip.src_addr = rte_cpu_to_be_32(config->src_ip);
    template->ip.dst_addr = rte_cpu_to_be_32(config->dst_ip);
    
    // Calculate IP checksum
    struct rte_ipv4_hdr ip_copy;
    memcpy(&ip_copy, &template->ip, sizeof(struct rte_ipv4_hdr));
    uint16_t ip_csum = calculate_ip_checksum(&ip_copy);
    template->ip.hdr_checksum = ip_csum;
    
    // UDP header
    template->udp.src_port = rte_cpu_to_be_16(config->src_port);
    template->udp.dst_port = rte_cpu_to_be_16(config->dst_port);
    template->udp.dgram_len = rte_cpu_to_be_16(UDP_HDR_SIZE + PAYLOAD_SIZE_NO_VLAN);
    template->udp.dgram_cksum = 0;
    
    // Payload will be filled by fill_payload_with_prbs31()
    if (config->payload_data && config->payload_size > 0) {
        uint16_t copy_size = (config->payload_size < PAYLOAD_SIZE_NO_VLAN) ? 
                             config->payload_size : PAYLOAD_SIZE_NO_VLAN;
        memcpy(template->payload, config->payload_data, copy_size);
    }
#endif
    
    return 0;
}

int build_packet_mbuf(struct rte_mbuf *mbuf, const struct packet_config *config)
{
    if (!mbuf || !config) {
        return -1;
    }

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct packet_template *pkt = (struct packet_template *)pkt_data;

    int ret = build_packet(pkt, config);
    if (ret < 0) {
        return ret;
    }

    mbuf->data_len = PACKET_SIZE;
    mbuf->pkt_len = PACKET_SIZE;

    return 0;
}

int build_packet_dynamic(struct rte_mbuf *mbuf, const struct packet_config *config,
                          uint16_t packet_size)
{
    if (!mbuf || !config) {
        return -1;
    }

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);

    // Calculate payload size
#if VLAN_ENABLED
    const uint16_t l2_len = ETH_HDR_SIZE + VLAN_HDR_SIZE;
#else
    const uint16_t l2_len = ETH_HDR_SIZE;
#endif
    const uint16_t payload_size = packet_size - l2_len - IP_HDR_SIZE - UDP_HDR_SIZE;

    // ==========================================
    // ETHERNET HEADER
    // ==========================================
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt_data;
    memcpy(&eth->dst_addr, &config->dst_mac, sizeof(struct rte_ether_addr));
    memcpy(&eth->src_addr, &config->src_mac, sizeof(struct rte_ether_addr));

#if VLAN_ENABLED
    eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_VLAN);

    // VLAN header
    struct vlan_hdr *vlan = (struct vlan_hdr *)(pkt_data + ETH_HDR_SIZE);
    uint16_t tci = ((config->vlan_priority & 0x07) << 13) | (config->vlan_id & 0x0FFF);
    vlan->tci = rte_cpu_to_be_16(tci);
    vlan->eth_proto = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    // IP header offset
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(pkt_data + ETH_HDR_SIZE + VLAN_HDR_SIZE);
#else
    eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(pkt_data + ETH_HDR_SIZE);
#endif

    // ==========================================
    // IP HEADER (dinamik total_length)
    // ==========================================
    ip->version_ihl = 0x45;
    ip->type_of_service = config->tos;
    ip->total_length = rte_cpu_to_be_16(IP_HDR_SIZE + UDP_HDR_SIZE + payload_size);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = config->ttl;
    ip->next_proto_id = IPPROTO_UDP;
    ip->hdr_checksum = 0;
    ip->src_addr = rte_cpu_to_be_32(config->src_ip);
    ip->dst_addr = rte_cpu_to_be_32(config->dst_ip);

    // Calculate IP checksum
    struct rte_ipv4_hdr ip_copy;
    memcpy(&ip_copy, ip, sizeof(struct rte_ipv4_hdr));
    ip->hdr_checksum = calculate_ip_checksum(&ip_copy);

    // ==========================================
    // UDP HEADER (dinamik dgram_len)
    // ==========================================
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + IP_HDR_SIZE);
    udp->src_port = rte_cpu_to_be_16(config->src_port);
    udp->dst_port = rte_cpu_to_be_16(config->dst_port);
    udp->dgram_len = rte_cpu_to_be_16(UDP_HDR_SIZE + payload_size);
    udp->dgram_cksum = 0;  // UDP checksum disabled

    // ==========================================
    // SET MBUF LENGTHS (dinamik)
    // ==========================================
    mbuf->data_len = packet_size;
    mbuf->pkt_len = packet_size;

    return 0;
}

uint16_t calculate_ip_checksum(struct rte_ipv4_hdr *ip)
{
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)ip;
    int len = sizeof(struct rte_ipv4_hdr) / 2;
    
    ip->hdr_checksum = 0;
    
    while (len > 0) {
        sum += *ptr++;
        len--;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)~sum;
}

uint16_t calculate_udp_checksum(const struct rte_ipv4_hdr *ip,
                                const struct rte_udp_hdr *udp,
                                const uint8_t *payload,
                                uint16_t payload_len)
{
    uint32_t sum = 0;
    uint16_t udp_len = UDP_HDR_SIZE + payload_len;
    
    // Pseudo-header
    sum += (ip->src_addr >> 16) & 0xFFFF;
    sum += (ip->src_addr) & 0xFFFF;
    sum += (ip->dst_addr >> 16) & 0xFFFF;
    sum += (ip->dst_addr) & 0xFFFF;
    sum += rte_cpu_to_be_16(IPPROTO_UDP);
    sum += rte_cpu_to_be_16(udp_len);
    
    // UDP header (checksum field assumed zero here)
    sum += udp->src_port;
    sum += udp->dst_port;
    sum += udp->dgram_len;
    
    // Payload
    const uint16_t *data = (const uint16_t *)payload;
    int len = payload_len;
    
    while (len > 1) {
        sum += *data++;
        len -= 2;
    }
    
    if (len > 0) {
        sum += *(const uint8_t *)data;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)~sum;
}

int set_mac_from_string(struct rte_ether_addr *mac, const char *mac_str)
{
    if (!mac || !mac_str) {
        return -1;
    }
    
    unsigned int values[6];
    int ret = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                     &values[0], &values[1], &values[2],
                     &values[3], &values[4], &values[5]);
    
    if (ret != 6) {
        return -1;
    }
    
    for (int i = 0; i < 6; i++) {
        mac->addr_bytes[i] = (uint8_t)values[i];
    }
    
    return 0;
}

int set_ip_from_string(uint32_t *ip, const char *ip_str)
{
    if (!ip || !ip_str) {
        return -1;
    }
    
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return -1;
    }
    
    *ip = rte_be_to_cpu_32(addr.s_addr);
    return 0;
}

void print_packet_info(const struct packet_config *config)
{
    if (!config) {
        return;
    }
    
    printf("\n=== Packet Configuration ===\n");
    
#if VLAN_ENABLED
    printf("VLAN: Enabled\n");
    printf("VLAN ID: %u\n", config->vlan_id);
    printf("VL ID: %u\n", config->vl_id);
    printf("VLAN Priority: %u\n", config->vlan_priority);
    printf("Packet Size: %u bytes\n", PACKET_SIZE_VLAN);
    printf("Payload Size: %u bytes (SEQ: %u + PRBS: %u)\n", 
           PAYLOAD_SIZE_VLAN, SEQ_BYTES, NUM_PRBS_BYTES);
#else
    printf("VLAN: Disabled\n");
    printf("VL ID: %u\n", config->vl_id);
    printf("Packet Size: %u bytes\n", PACKET_SIZE_NO_VLAN);
    printf("Payload Size: %u bytes (SEQ: %u + PRBS: %u)\n", 
           PAYLOAD_SIZE_NO_VLAN, SEQ_BYTES, NUM_PRBS_BYTES);
#endif
    
    printf("\nEthernet Layer:\n");
    printf("  Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           config->src_mac.addr_bytes[0], config->src_mac.addr_bytes[1],
           config->src_mac.addr_bytes[2], config->src_mac.addr_bytes[3],
           config->src_mac.addr_bytes[4], config->src_mac.addr_bytes[5]);
    printf("  Dest MAC: %02x:%02x:%02x:%02x:%02x:%02x (VL ID: %u)\n",
           config->dst_mac.addr_bytes[0], config->dst_mac.addr_bytes[1],
           config->dst_mac.addr_bytes[2], config->dst_mac.addr_bytes[3],
           config->dst_mac.addr_bytes[4], config->dst_mac.addr_bytes[5],
           config->vl_id);
    
    printf("\nIP Layer:\n");
    printf("  Source IP: %u.%u.%u.%u\n",
           (config->src_ip >> 24) & 0xFF, (config->src_ip >> 16) & 0xFF,
           (config->src_ip >> 8) & 0xFF, config->src_ip & 0xFF);
    printf("  Dest IP: %u.%u.%u.%u (VL ID: %u)\n",
           (config->dst_ip >> 24) & 0xFF, (config->dst_ip >> 16) & 0xFF,
           (config->dst_ip >> 8) & 0xFF, config->dst_ip & 0xFF,
           config->vl_id);
    printf("  TTL: %u\n", config->ttl);
    printf("  TOS: 0x%02x\n", config->tos);
    
    printf("\nUDP Layer:\n");
    printf("  Source Port: %u\n", config->src_port);
    printf("  Dest Port: %u\n", config->dst_port);
    printf("\n");
}