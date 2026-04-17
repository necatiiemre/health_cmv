#define _GNU_SOURCE
#include "HealthMonitor.h"
#include "PsuTelemetryReceiver.h"   // psu_telem_print_table() - appended to health block
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>
#include <poll.h>

// ==========================================
// GLOBAL STATE
// ==========================================

static struct health_monitor_state g_health_monitor;
static volatile bool *g_stop_flag = NULL;

// ==========================================
// QUERY PACKET TEMPLATE (64 bytes, no VLAN)
// ==========================================

static const uint8_t health_query_template[HEALTH_MONITOR_QUERY_SIZE] = {
    // Ethernet Header (14 bytes)
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00,  // DST MAC (multicast)
    0x02, 0x00, 0x00, 0x00, 0x00, 0x20,  // SRC MAC
    0x08, 0x00,                          // EtherType (IPv4)

    // IP Header (20 bytes)
    0x45, 0x00, 0x00, 0x32,              // Version, IHL, TOS, Total Length (50)
    0xd4, 0x3b, 0x00, 0x00,              // ID, Flags, Fragment Offset
    0x01, 0x11,                          // TTL=1, Protocol=UDP
    0xd9, 0x9d,                          // Header Checksum
    0x0a, 0x01, 0x21, 0x01,              // SRC IP: 10.1.33.1
    0xe0, 0xe0, 0x00, 0x00,              // DST IP: 224.224.0.0

    // UDP Header (8 bytes)
    0x00, 0x64, 0x00, 0x64,              // SRC Port: 100, DST Port: 100
    0x00, 0x1e, 0x00, 0x00,              // Length: 30, Checksum: 0

    // Payload (22 bytes)
    0x26, 0x00, 0x52, 0x00, 0x00, 0x00,
    0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Sequence Number (1 byte) - offset 63
    0x2f
};

// ==========================================
// UTILITY FUNCTIONS
// ==========================================

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// ==========================================
// BYTE PARSING FUNCTIONS (Big-Endian)
// ==========================================

static inline uint16_t parse_2byte_be(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

static inline uint32_t parse_4byte_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8)  | (uint32_t)data[3];
}

static inline uint64_t parse_5byte_be(const uint8_t *data)
{
    return ((uint64_t)data[0] << 32) | ((uint64_t)data[1] << 24) |
           ((uint64_t)data[2] << 16) | ((uint64_t)data[3] << 8) |
           (uint64_t)data[4];
}

static inline uint64_t parse_6byte_be(const uint8_t *data)
{
    return ((uint64_t)data[0] << 40) | ((uint64_t)data[1] << 32) |
           ((uint64_t)data[2] << 24) | ((uint64_t)data[3] << 16) |
           ((uint64_t)data[4] << 8)  | (uint64_t)data[5];
}

// ==========================================
// DEVICE HEADER PARSING
// ==========================================

static void parse_device_header(const uint8_t *udp_payload, struct health_device_info *dev)
{
    dev->device_id          = parse_2byte_be(udp_payload + DEV_OFF_DEVICE_ID);
    dev->operation_type     = udp_payload[DEV_OFF_OPERATION_TYPE];
    dev->config_type        = udp_payload[DEV_OFF_CONFIG_TYPE];
    dev->frame_length       = parse_2byte_be(udp_payload + DEV_OFF_FRAME_LENGTH);
    dev->status_enable      = udp_payload[DEV_OFF_STATUS_ENABLE];
    dev->status_addr        = parse_2byte_be(udp_payload + DEV_OFF_STATUS_ADDR);
    dev->tx_total_count     = parse_6byte_be(udp_payload + DEV_OFF_TX_TOTAL_COUNT);
    dev->rx_total_count     = parse_6byte_be(udp_payload + DEV_OFF_RX_TOTAL_COUNT);
    dev->tx_err_total_count = parse_6byte_be(udp_payload + DEV_OFF_TX_ERR_TOTAL_COUNT);
    dev->rx_err_total_count = parse_6byte_be(udp_payload + DEV_OFF_RX_ERR_TOTAL_COUNT);
    dev->heartbeat          = udp_payload[DEV_OFF_HEARTBEAT];
    dev->device_id2         = parse_2byte_be(udp_payload + DEV_OFF_DEV_ID2);
    dev->port_count         = udp_payload[DEV_OFF_PORT_COUNT];
    dev->token_bucket_status = udp_payload[DEV_OFF_TOKEN_BUCKET];
    dev->sw_mode            = udp_payload[DEV_OFF_SW_MODE];
    dev->vendor_id          = udp_payload[DEV_OFF_VENDOR_ID];
    dev->auto_mac_update    = udp_payload[DEV_OFF_AUTO_MAC_UPDATE];
    dev->upstream_mode      = udp_payload[DEV_OFF_UPSTREAM_MODE];

    // SW IP core version (3x uint16_t big-endian in 6-byte field)
    dev->sw_ip_major = (uint8_t)parse_2byte_be(udp_payload + DEV_OFF_SW_IP_CORE_VER);
    dev->sw_ip_minor = (uint8_t)parse_2byte_be(udp_payload + DEV_OFF_SW_IP_CORE_VER + 2);
    dev->sw_ip_patch = (uint8_t)parse_2byte_be(udp_payload + DEV_OFF_SW_IP_CORE_VER + 4);

    // ES IP core version (3 single bytes in last 3 bytes of 6-byte field)
    dev->es_ip_major = udp_payload[DEV_OFF_ES_IP_CORE_VER + 3];
    dev->es_ip_minor = udp_payload[DEV_OFF_ES_IP_CORE_VER + 4];
    dev->es_ip_patch = udp_payload[DEV_OFF_ES_IP_CORE_VER + 5];

    dev->sw_input_fifo_size  = parse_2byte_be(udp_payload + DEV_OFF_SW_INPUT_FIFO);
    dev->pkt_pro_fifo_size   = parse_2byte_be(udp_payload + DEV_OFF_PKT_PRO_FIFO);
    dev->sw_output_fifo_size = parse_2byte_be(udp_payload + DEV_OFF_SW_OUTPUT_FIFO);
    dev->hp_fifo_size        = parse_2byte_be(udp_payload + DEV_OFF_HP_FIFO_SIZE);
    dev->lp_fifo_size        = parse_2byte_be(udp_payload + DEV_OFF_LP_FIFO_SIZE);
    dev->be_fifo_size        = parse_2byte_be(udp_payload + DEV_OFF_BE_FIFO_SIZE);

    dev->tod_ns              = parse_5byte_be(udp_payload + DEV_OFF_TOD_NS);
    dev->tod_sec             = parse_5byte_be(udp_payload + DEV_OFF_TOD_SEC);

    dev->eth_wrong_dev_cnt   = parse_6byte_be(udp_payload + DEV_OFF_ETH_WRONG_DEV_CNT);
    dev->eth_wrong_op_cnt    = parse_6byte_be(udp_payload + DEV_OFF_ETH_WRONG_OP_CNT);
    dev->eth_wrong_type_cnt  = parse_6byte_be(udp_payload + DEV_OFF_ETH_WRONG_TYPE_CNT);

    dev->fpga_voltage        = parse_2byte_be(udp_payload + DEV_OFF_FPGA_VOLTAGE);
    dev->fpga_temp           = (int16_t)parse_2byte_be(udp_payload + DEV_OFF_FPGA_TEMP);
    dev->config_id           = parse_2byte_be(udp_payload + DEV_OFF_CONFIG_ID);
}

// ==========================================
// PORT DATA PARSING
// ==========================================

static void parse_port_data(const uint8_t *port_data, struct health_port_info *port)
{
    port->port_number        = parse_2byte_be(port_data + PORT_OFF_PORT_NUMBER);
    port->bit_status         = port_data[PORT_OFF_BIT_STATUS];
    port->crc_err_count      = parse_6byte_be(port_data + PORT_OFF_CRC_ERR_CNT);
    port->ali_err_count      = parse_6byte_be(port_data + PORT_OFF_ALI_ERR_CNT);
    port->len_exc_64         = parse_6byte_be(port_data + PORT_OFF_LEN_EXC_64);
    port->len_exc_1518       = parse_6byte_be(port_data + PORT_OFF_LEN_EXC_1518);
    port->min_vl_frame_err   = parse_6byte_be(port_data + PORT_OFF_MIN_VL_FRAME_ERR);
    port->max_vl_frame_err   = parse_6byte_be(port_data + PORT_OFF_MAX_VL_FRAME_ERR);
    port->inp_port_terr_cnt  = parse_6byte_be(port_data + PORT_OFF_INP_PORT_TERR_CNT);
    port->traffic_policy_drop = parse_6byte_be(port_data + PORT_OFF_TRAFFIC_POLICY_DROP);
    port->be_count           = parse_6byte_be(port_data + PORT_OFF_BE_COUNT);
    port->tx_count           = parse_6byte_be(port_data + PORT_OFF_TX_COUNT);
    port->rx_count           = parse_6byte_be(port_data + PORT_OFF_RX_COUNT);
    port->vl_source_err      = parse_6byte_be(port_data + PORT_OFF_VL_SOURCE_ERR);
    port->max_delay_err      = parse_6byte_be(port_data + PORT_OFF_MAX_DELAY_ERR);
    port->queue_overflow     = parse_6byte_be(port_data + PORT_OFF_QUEUE_OVERFLOW);
    port->vlid_drop_count    = parse_6byte_be(port_data + PORT_OFF_VLID_DROP);
    port->undef_mac_count    = parse_6byte_be(port_data + PORT_OFF_UNDEF_MAC);
    port->hp_queue_overflow  = parse_6byte_be(port_data + PORT_OFF_HP_QUEUE_OVERFLOW);
    port->lp_queue_overflow  = parse_6byte_be(port_data + PORT_OFF_LP_QUEUE_OVERFLOW);
    port->be_queue_overflow  = parse_6byte_be(port_data + PORT_OFF_BE_QUEUE_OVERFLOW);
    port->max_delay_param    = parse_6byte_be(port_data + PORT_OFF_MAX_DELAY_PARAM);
    port->port_speed         = parse_6byte_be(port_data + PORT_OFF_PORT_SPEED);
    port->valid = true;
}

// ==========================================
// MCU DATA PARSING
// ==========================================

static void parse_mcu_data(const uint8_t *udp_payload, struct health_mcu_info *mcu)
{
    // Header
    mcu->device_id          = parse_2byte_be(udp_payload + MCU_OFF_DEVICE_ID);
    mcu->operation_type     = udp_payload[MCU_OFF_OPERATION_TYPE];
    mcu->config_type        = udp_payload[MCU_OFF_CONFIG_TYPE];
    mcu->frame_length       = parse_2byte_be(udp_payload + MCU_OFF_FRAME_LENGTH);
    mcu->status_enable      = udp_payload[MCU_OFF_STATUS_ENABLE];
    mcu->fw_major           = (udp_payload[MCU_OFF_FW_VERSION] >> 4) & 0x0F;
    mcu->fw_minor           = udp_payload[MCU_OFF_FW_VERSION] & 0x0F;
    mcu->fw_patch           = udp_payload[MCU_OFF_FW_VERSION + 1];
    mcu->input_power_status = udp_payload[MCU_OFF_INPUT_POWER_STATUS];
    mcu->pbit               = udp_payload[MCU_OFF_PBIT];
    mcu->cbit               = udp_payload[MCU_OFF_CBIT];

    // Current data (readValue/1000)
    mcu->curr_12v      = parse_2byte_be(udp_payload + MCU_OFF_CURR_12V);
    mcu->curr_3v3      = parse_2byte_be(udp_payload + MCU_OFF_CURR_3V3);
    mcu->curr_1v8      = parse_2byte_be(udp_payload + MCU_OFF_CURR_1V8);
    mcu->curr_3v3_fo   = parse_2byte_be(udp_payload + MCU_OFF_CURR_3V3_FO);
    mcu->curr_1v3      = parse_2byte_be(udp_payload + MCU_OFF_CURR_1V3);
    mcu->curr_1v0_mgr  = parse_2byte_be(udp_payload + MCU_OFF_CURR_1V0_MGR);
    mcu->curr_1v0_ast  = parse_2byte_be(udp_payload + MCU_OFF_CURR_1V0_AST);

    // Voltage data (readValue/1000)
    mcu->volt_3v3      = parse_2byte_be(udp_payload + MCU_OFF_VOLT_3V3);
    mcu->volt_3v3_fo   = parse_2byte_be(udp_payload + MCU_OFF_VOLT_3V3_FO);
    mcu->volt_12v      = parse_2byte_be(udp_payload + MCU_OFF_VOLT_12V);
    mcu->volt_1v8      = parse_2byte_be(udp_payload + MCU_OFF_VOLT_1V8);
    mcu->volt_1v3      = parse_2byte_be(udp_payload + MCU_OFF_VOLT_1V3);
    mcu->volt_1v0_mgr  = parse_2byte_be(udp_payload + MCU_OFF_VOLT_1V0_MGR);
    mcu->volt_1v0_ast  = parse_2byte_be(udp_payload + MCU_OFF_VOLT_1V0_AST);

    // Temperature data
    mcu->board_temp        = (int16_t)parse_2byte_be(udp_payload + MCU_OFF_BOARD_TEMP);
    mcu->fo_trans1_temp    = (int16_t)parse_2byte_be(udp_payload + MCU_OFF_FO_TRANS1_TEMP);
    mcu->eth_phy_1g_temp   = (int8_t)udp_payload[MCU_OFF_ETH_PHY_1G_TEMP];
    mcu->eth_phy_100m_temp = (int8_t)udp_payload[MCU_OFF_ETH_PHY_100M_TEMP];
}

// ==========================================
// RESPONSE PARSING (Assistant / Manager / MCU)
// ==========================================

static void health_parse_response(const uint8_t *packet, size_t len, struct health_cycle_data *cycle)
{
    // Get UDP payload pointer
    const uint8_t *udp_payload = packet + HEALTH_UDP_PAYLOAD_OFFSET;

    // Determine packet type by size
    bool has_device_header = false;
    int port_count_in_packet = 0;
    int port_data_offset = 0;

    if (len == HEALTH_PKT_SIZE_WITH_HEADER) {
        // 1187 bytes: Device header + 8 ports
        has_device_header = true;
        port_count_in_packet = 8;
        port_data_offset = HEALTH_DEVICE_HEADER_SIZE;
    } else if (len == HEALTH_PKT_SIZE_8_PORTS) {
        // 1083 bytes: Mini header + 8 ports
        port_count_in_packet = 8;
        port_data_offset = HEALTH_MINI_HEADER_SIZE;
    } else if (len == HEALTH_PKT_SIZE_3_PORTS) {
        // 438 bytes: Mini header + 3 ports
        port_count_in_packet = 3;
        port_data_offset = HEALTH_MINI_HEADER_SIZE;
    } else {
        // Not an FPGA packet - treat as MCU data
        // MCU expected ~84 bytes but accept any non-FPGA health response
        if (len >= (size_t)(HEALTH_UDP_PAYLOAD_OFFSET + MCU_OFF_ETH_PHY_100M_TEMP + 1)) {
            parse_mcu_data(udp_payload, &cycle->mcu);
            cycle->mcu.valid = true;
            cycle->total_responses_received++;
        } else {
            printf("[HEALTH] Unexpected small packet: %zu bytes (need >= %d for MCU)\n",
                   len, (int)(HEALTH_UDP_PAYLOAD_OFFSET + MCU_OFF_ETH_PHY_100M_TEMP + 1));
        }
        return;
    }

    // Determine which FPGA this packet belongs to:
    //   1187-byte packets have full device header with valid status_enable at byte 6:
    //     status_enable = 0x03 -> Assistant FPGA (16 ports: 0-15)
    //     status_enable = 0x01 -> Manager FPGA  (19 ports: 16-34)
    //   1083/438-byte packets have mini header where byte 6 is reserved (always 0x00).
    //   For mini header packets, use the last FPGA identified from a 1187-byte packet.

    struct health_fpga_data *target_fpga = NULL;

    if (has_device_header) {
        // 1187-byte packet: identify FPGA from status_enable field
        uint8_t status_enable = udp_payload[DEV_OFF_STATUS_ENABLE];
        if (status_enable == STATUS_ENABLE_ASSISTANT) {
            target_fpga = &cycle->assistant;
            cycle->last_fpga_type = STATUS_ENABLE_ASSISTANT;
        } else if (status_enable == STATUS_ENABLE_MANAGER) {
            target_fpga = &cycle->manager;
            cycle->last_fpga_type = STATUS_ENABLE_MANAGER;
        } else {
            printf("[HEALTH] Unknown status_enable=0x%02X in %zu-byte packet, ignoring\n",
                   status_enable, len);
            return;
        }
    } else {
        // 1083/438-byte packet (mini header): use last identified FPGA
        if (cycle->last_fpga_type == STATUS_ENABLE_ASSISTANT) {
            target_fpga = &cycle->assistant;
        } else if (cycle->last_fpga_type == STATUS_ENABLE_MANAGER) {
            target_fpga = &cycle->manager;
        } else {
            printf("[HEALTH] Mini header packet (%zu bytes) received before any 1187-byte packet, ignoring\n", len);
            return;
        }
    }

    // Parse device header if present
    if (has_device_header && !target_fpga->device_info_valid) {
        parse_device_header(udp_payload, &target_fpga->device);
        target_fpga->device_info_valid = true;
    }

    // Parse port data
    const uint8_t *port_ptr = udp_payload + port_data_offset;
    for (int i = 0; i < port_count_in_packet; i++) {
        struct health_port_info temp_port;
        memset(&temp_port, 0, sizeof(temp_port));
        parse_port_data(port_ptr, &temp_port);

        // Store in correct slot by port number
        uint16_t pnum = temp_port.port_number;
        if (pnum < HEALTH_MAX_PORTS) {
            target_fpga->ports[pnum] = temp_port;
            target_fpga->port_count_received++;
        }

        port_ptr += HEALTH_PORT_DATA_SIZE;
    }

    target_fpga->packets_received++;
    cycle->total_responses_received++;
}

// ==========================================
// CONVERSION FUNCTIONS
// ==========================================

static double convert_fpga_voltage(uint16_t raw)
{
    uint16_t integer_part = (raw & 0x7FF8) >> 3;
    uint16_t fractional_part = raw & 0x7;
    double milli_volt = (double)integer_part + (double)fractional_part / 10.0;
    return milli_volt / 1000.0;
}

static double convert_fpga_temperature(int16_t raw)
{
    uint16_t uraw = (uint16_t)raw;
    uint16_t integer_part = (uraw & 0x7FF0) >> 4;
    uint16_t fractional_part = uraw & 0xF;
    double divisor = (fractional_part >= 10) ? 100.0 : 10.0;
    double kelvin = (double)integer_part + (double)fractional_part / divisor;
    return kelvin - 273.15;
}

static const char *port_speed_str(uint64_t speed)
{
    switch (speed) {
    case 0:  return "1000M";
    case 1:  return "10M";
    case 2:  return "100M";
    default: return "???";
    }
}

// ==========================================
// TABLE PRINTING
// ==========================================

static void health_print_fpga_table(const char *fpga_name, const struct health_fpga_data *fpga)
{
    const struct health_device_info *dev = &fpga->device;

    // FPGA Device Status Header
    printf("[HEALTH] ============ %s FPGA - Device Status ============\n", fpga_name);

    if (fpga->device_info_valid) {
        printf("[HEALTH] DevID=0x%04X | OpType=0x%02X | CfgType=0x%02X | StatusEnable=0x%02X\n",
               dev->device_id, dev->operation_type, dev->config_type, dev->status_enable);
        printf("[HEALTH] Mode=0x%02X | Ports=%d | ConfigID=%d\n",
               dev->sw_mode, dev->port_count, dev->config_id);
        printf("[HEALTH] SW_FIRMW=%d.%d.%d | ES_FIRMW=%d.%d.%d | VendorID=%d\n",
               dev->sw_ip_major, dev->sw_ip_minor, dev->sw_ip_patch,
               dev->es_ip_major, dev->es_ip_minor, dev->es_ip_patch,
               dev->vendor_id);
        printf("[HEALTH] Temp=%.2f°C | Volt=%.4fV\n",
               convert_fpga_temperature(dev->fpga_temp), convert_fpga_voltage(dev->fpga_voltage));
        printf("[HEALTH] TxTotal=%lu | RxTotal=%lu | TxErrTotal=%lu | RxErrTotal=%lu\n",
               (unsigned long)dev->tx_total_count, (unsigned long)dev->rx_total_count,
               (unsigned long)dev->tx_err_total_count, (unsigned long)dev->rx_err_total_count);
    } else {
        printf("[HEALTH] Device info: NOT RECEIVED\n");
    }

    // Port Status Table
    printf("[HEALTH] ---- %s FPGA Port Status (pkts=%d, ports=%d) ----\n",
           fpga_name, fpga->packets_received, fpga->port_count_received);
    printf("[HEALTH] Port |    TxCnt |    RxCnt | PolDrop | VLDrop | HP_Ovf | LP_Ovf | BE_Ovf |\n");
    printf("[HEALTH] -----|----------|----------|---------|--------|--------|--------|--------|\n");

    for (int i = 0; i < HEALTH_MAX_PORTS; i++) {
        const struct health_port_info *p = &fpga->ports[i];
        if (p->valid) {
            printf("[HEALTH] %4d | %8lu | %8lu | %7lu | %6lu | %6lu | %6lu | %6lu |\n",
                   p->port_number,
                   (unsigned long)p->tx_count,
                   (unsigned long)p->rx_count,
                   (unsigned long)p->traffic_policy_drop,
                   (unsigned long)p->vlid_drop_count,
                   (unsigned long)p->hp_queue_overflow,
                   (unsigned long)p->lp_queue_overflow,
                   (unsigned long)p->be_queue_overflow);
        }
    }
}

static void health_print_tables(const struct health_cycle_data *cycle)
{
    printf("\n");

    // Assistant FPGA Table
    health_print_fpga_table("ASSISTANT", &cycle->assistant);

    printf("[HEALTH] ================================================\n\n");

    // Manager FPGA Table
    health_print_fpga_table("MANAGER", &cycle->manager);

    printf("[HEALTH] ================================================\n");

    // MCU Table
    printf("[HEALTH] ============ MCU - Status ============\n");
    if (cycle->mcu.valid) {
        const struct health_mcu_info *mcu = &cycle->mcu;
        printf("[HEALTH] DevID=0x%04X | StatusEnable=0x%02X | FW=%d.%d.%d\n",
               mcu->device_id, mcu->status_enable, mcu->fw_major, mcu->fw_minor, mcu->fw_patch);
        printf("[HEALTH] 28V Primary=%s | 28V Secondary=%s | PBIT=%s | CBIT=%s\n",
               (mcu->input_power_status & 0x01) ? "FAIL" : "SUCCESS",
               (mcu->input_power_status & 0x02) ? "FAIL" : "SUCCESS",
               (mcu->pbit == 0x00) ? "SUCCESS" : "FAIL",
               (mcu->cbit == 0x00) ? "SUCCESS" : "FAIL");
        printf("[HEALTH] ---- Current Data ----\n");
        printf("[HEALTH] Rail              | Curr(A) |\n");
        printf("[HEALTH] -----------------|---------|\n");
        printf("[HEALTH] 1V8              | %7.3f |\n", mcu->curr_1v8 / 1000.0);
        printf("[HEALTH] 1V3              | %7.3f |\n", mcu->curr_1v3 / 1000.0);
        printf("[HEALTH] 1V0 Mgr FPGA     | %7.3f |\n", mcu->curr_1v0_mgr / 1000.0);
        printf("[HEALTH] 1V0 Ast FPGA     | %7.3f |\n", mcu->curr_1v0_ast / 1000.0);
        printf("[HEALTH] ---- Voltage Data ----\n");
        printf("[HEALTH] Rail              | Volt(V) |\n");
        printf("[HEALTH] -----------------|---------|\n");
        printf("[HEALTH] 1V8 VCCIO        | %7.3f |\n", mcu->volt_1v8 / 1000.0);
        printf("[HEALTH] 1V3 VCC          | %7.3f |\n", mcu->volt_1v3 / 1000.0);
        printf("[HEALTH] 1V0 Mgr FPGA     | %7.3f |\n", mcu->volt_1v0_mgr / 1000.0);
        printf("[HEALTH] 1V0 Ast FPGA     | %7.3f |\n", mcu->volt_1v0_ast / 1000.0);
        printf("[HEALTH] ---- Temperatures ----\n");
        printf("[HEALTH] Board(CBA)=%.2f°C | FO Trans1=%.2f°C | PHY 1G=%d°C | PHY 100M=%d°C\n",
               mcu->board_temp / 100.0, mcu->fo_trans1_temp / 100.0,
               mcu->eth_phy_1g_temp, mcu->eth_phy_100m_temp);
    } else {
        printf("[HEALTH] MCU: NOT RECEIVED\n");
    }

    printf("[HEALTH] Total responses: %d/%d\n",
           cycle->total_responses_received, HEALTH_MONITOR_EXPECTED_RESPONSES);
    printf("[HEALTH] ================================================\n\n");

    // Power supply readings (voltage / current / power) are pushed to us by
    // MainSoftware over UDP. Append them to the per-cycle health block so
    // they're visible alongside the FPGA/MCU tables both on screen and in
    // the dpdk_app.log that the PDF generator consumes.
    psu_telem_print_table();
}

static int get_interface_index(const char *ifname)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return ifr.ifr_ifindex;
}

// ==========================================
// SOCKET FUNCTIONS
// ==========================================

static int create_raw_socket(const char *ifname, int if_index)
{
    (void)ifname;

    // Create raw socket
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        fprintf(stderr, "[HEALTH] Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    // Bind to interface
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "[HEALTH] Failed to bind socket: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    // Set promiscuous mode for RX
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = if_index;
    mreq.mr_type = PACKET_MR_PROMISC;

    if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "[HEALTH] Warning: Failed to set promiscuous mode: %s\n", strerror(errno));
        // Continue anyway
    }

    return sock;
}

// ==========================================
// PACKET FUNCTIONS
// ==========================================

static int send_health_query(void)
{
    struct health_monitor_state *state = &g_health_monitor;

    // Update sequence number in packet
    state->query_packet[63] = state->sequence;

    // Setup destination address
    struct sockaddr_ll dest;
    memset(&dest, 0, sizeof(dest));
    dest.sll_family = AF_PACKET;
    dest.sll_ifindex = state->if_index;
    dest.sll_halen = ETH_ALEN;
    memcpy(dest.sll_addr, state->query_packet, ETH_ALEN);  // DST MAC

    // Send packet
    ssize_t sent = sendto(state->tx_socket, state->query_packet, HEALTH_MONITOR_QUERY_SIZE,
                          0, (struct sockaddr *)&dest, sizeof(dest));

    if (sent < 0) {
        fprintf(stderr, "[HEALTH] Failed to send query: %s\n", strerror(errno));
        return -1;
    }

    printf("[HEALTH] Query sent (seq=0x%02X)\n", state->sequence);
    return 0;
}

static bool is_health_response(const uint8_t *packet, size_t len)
{
    // Minimum packet size check
    if (len < 14) return false;

    // Check VL_IDX at DST MAC offset 4-5 (all health packets including MCU)
    if (packet[4] == HEALTH_MONITOR_RESPONSE_VL_IDX_HIGH &&
        packet[5] == HEALTH_MONITOR_RESPONSE_VL_IDX_LOW) {
        return true;
    }

    return false;
}

static int receive_health_responses(int timeout_ms, struct health_cycle_data *cycle)
{
    struct health_monitor_state *state = &g_health_monitor;
    uint8_t buffer[HEALTH_MONITOR_RX_BUFFER_SIZE];
    uint64_t start_time = get_time_ms();

    while (cycle->total_responses_received < HEALTH_MONITOR_EXPECTED_RESPONSES) {
        // Calculate remaining timeout
        uint64_t elapsed = get_time_ms() - start_time;
        if (elapsed >= (uint64_t)timeout_ms) {
            break;  // Timeout
        }
        int remaining = timeout_ms - (int)elapsed;

        // Poll for incoming packets
        struct pollfd pfd;
        pfd.fd = state->rx_socket;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, remaining);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[HEALTH] Poll error: %s\n", strerror(errno));
            break;
        }

        if (ret == 0) {
            break;  // Timeout
        }

        if (pfd.revents & POLLIN) {
            ssize_t len = recv(state->rx_socket, buffer, sizeof(buffer), 0);
            if (len < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                fprintf(stderr, "[HEALTH] Recv error: %s\n", strerror(errno));
                break;
            }

            // Check if this is a health response and parse it
            if (is_health_response(buffer, len)) {
                printf("[HEALTH-DBG] Accepted packet: %zd bytes\n", len);
                health_parse_response(buffer, len, cycle);
            }
            // else: ignore non-health packets (PRBS traffic etc.)
        }
    }

    return 0;
}

// ==========================================
// THREAD FUNCTION
// ==========================================

static void *health_monitor_thread_func(void *arg)
{
    (void)arg;
    struct health_monitor_state *state = &g_health_monitor;
    struct health_cycle_data cycle;

    printf("[HEALTH] Thread started\n");

    while (!(*g_stop_flag) && state->running) {
        uint64_t cycle_start = get_time_ms();

        // 1. Reset cycle data
        memset(&cycle, 0, sizeof(cycle));

        // 2. Send query
        if (send_health_query() < 0) {
            // Error sending, wait and retry
            usleep(100000);  // 100ms
            continue;
        }

        // Update stats
        pthread_spin_lock(&state->stats_lock);
        state->stats.queries_sent++;
        state->stats.current_sequence = state->sequence;
        pthread_spin_unlock(&state->stats_lock);

        // 3. Receive and parse responses
        receive_health_responses(HEALTH_MONITOR_RESPONSE_TIMEOUT_MS, &cycle);

        uint64_t cycle_end = get_time_ms();
        uint64_t cycle_time = cycle_end - cycle_start;

        // 4. Check FW version match after warmup (at 10th cycle)
        if (state->warmup_complete && !state->fw_check_done) {
            state->post_warmup_cycle_count++;
            if (state->post_warmup_cycle_count >= 10) {
                state->fw_check_done = true;
                if (cycle.assistant.device_info_valid && cycle.manager.device_info_valid) {
                    const struct health_device_info *ast = &cycle.assistant.device;
                    const struct health_device_info *mgr = &cycle.manager.device;
                    if (ast->sw_ip_major != mgr->sw_ip_major ||
                        ast->sw_ip_minor != mgr->sw_ip_minor ||
                        ast->sw_ip_patch != mgr->sw_ip_patch) {
                        printf("\n");
                        printf("[HEALTH] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                        printf("[HEALTH] ERROR: SW firmware version mismatch detected!\n");
                        printf("[HEALTH]   Assistant FPGA SW_FW = %d.%d.%d\n",
                               ast->sw_ip_major, ast->sw_ip_minor, ast->sw_ip_patch);
                        printf("[HEALTH]   Manager   FPGA SW_FW = %d.%d.%d\n",
                               mgr->sw_ip_major, mgr->sw_ip_minor, mgr->sw_ip_patch);
                        printf("[HEALTH] Stopping test due to firmware mismatch!\n");
                        printf("[HEALTH] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                        printf("\n");
                        *g_stop_flag = true;
                    } else {
                        printf("[HEALTH] FW version check PASSED: Assistant=%d.%d.%d Manager=%d.%d.%d\n",
                               ast->sw_ip_major, ast->sw_ip_minor, ast->sw_ip_patch,
                               mgr->sw_ip_major, mgr->sw_ip_minor, mgr->sw_ip_patch);
                    }
                } else {
                    printf("[HEALTH] WARNING: Could not verify FW versions - missing FPGA data (Assistant=%s, Manager=%s)\n",
                           cycle.assistant.device_info_valid ? "OK" : "MISSING",
                           cycle.manager.device_info_valid ? "OK" : "MISSING");
                }
            }
        }

        // 4b. Check 28V power status after warmup (at 10th cycle)
        if (state->warmup_complete && state->power_status_check_enabled && !state->power_status_check_done) {
            if (state->post_warmup_cycle_count >= 10) {
                state->power_status_check_done = true;
                if (cycle.mcu.valid) {
                    uint8_t actual = cycle.mcu.input_power_status;
                    uint8_t expected = state->expected_power_status;
                    if (actual == expected) {
                        printf("\n");
                        printf("[HEALTH] ✓ 28V Power Status Check PASSED\n");
                        printf("[HEALTH]   Expected: Primary=%s | Secondary=%s\n",
                               (expected & 0x01) ? "FAIL" : "SUCCESS",
                               (expected & 0x02) ? "FAIL" : "SUCCESS");
                        printf("[HEALTH]   Actual:   Primary=%s | Secondary=%s\n",
                               (actual & 0x01) ? "FAIL" : "SUCCESS",
                               (actual & 0x02) ? "FAIL" : "SUCCESS");
                        printf("\n");
                    } else {
                        printf("\n");
                        printf("[HEALTH] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                        printf("[HEALTH] ERROR: 28V Power Status MISMATCH detected!\n");
                        printf("[HEALTH]   Expected: Primary=%s | Secondary=%s (0x%02X)\n",
                               (expected & 0x01) ? "FAIL" : "SUCCESS",
                               (expected & 0x02) ? "FAIL" : "SUCCESS",
                               expected);
                        printf("[HEALTH]   Actual:   Primary=%s | Secondary=%s (0x%02X)\n",
                               (actual & 0x01) ? "FAIL" : "SUCCESS",
                               (actual & 0x02) ? "FAIL" : "SUCCESS",
                               actual);
                        printf("[HEALTH] Stopping test due to 28V power status mismatch!\n");
                        printf("[HEALTH] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                        printf("\n");
                        *g_stop_flag = true;
                    }
                } else {
                    printf("[HEALTH] WARNING: MCU data not received at cycle 10 - cannot verify 28V power status\n");
                }
            }
        }

        // 5. Print parsed data tables (Assistant + Manager)
        health_print_tables(&cycle);

        // 6. Update statistics
        pthread_spin_lock(&state->stats_lock);
        state->stats.responses_received += cycle.total_responses_received;
        state->stats.last_cycle_time_ms = cycle_time;
        state->stats.last_response_count = cycle.total_responses_received;
        if (cycle.total_responses_received < HEALTH_MONITOR_EXPECTED_RESPONSES) {
            state->stats.timeouts++;
        }
        pthread_spin_unlock(&state->stats_lock);

        // 7. Increment sequence (255 -> 1, skip 0)
        if (state->sequence >= 255) {
            state->sequence = 1;
        } else {
            state->sequence++;
        }

        // 8. Wait for remaining time to complete 1 second interval
        uint64_t elapsed = get_time_ms() - cycle_start;
        if (elapsed < HEALTH_MONITOR_QUERY_INTERVAL_MS) {
            usleep((HEALTH_MONITOR_QUERY_INTERVAL_MS - elapsed) * 1000);
        }
    }

    printf("[HEALTH] Thread stopped\n");
    return NULL;
}

// ==========================================
// PUBLIC API
// ==========================================

int init_health_monitor(void)
{
    struct health_monitor_state *state = &g_health_monitor;

    printf("\n=== Initializing Health Monitor ===\n");
    printf("  Interface: %s\n", HEALTH_MONITOR_INTERFACE);
    printf("  Query interval: %d ms\n", HEALTH_MONITOR_QUERY_INTERVAL_MS);
    printf("  Response timeout: %d ms\n", HEALTH_MONITOR_RESPONSE_TIMEOUT_MS);
    printf("  Expected responses: %d (Assistant=%d + Manager=%d + MCU=%d)\n",
           HEALTH_MONITOR_EXPECTED_RESPONSES,
           ASSISTANT_EXPECTED_PACKETS, MANAGER_EXPECTED_PACKETS, MCU_EXPECTED_PACKETS);
    printf("  Response VL_IDX: 0x%04X (%d)\n",
           HEALTH_MONITOR_RESPONSE_VL_IDX, HEALTH_MONITOR_RESPONSE_VL_IDX);

    // Initialize state
    memset(state, 0, sizeof(*state));
    state->tx_socket = -1;
    state->rx_socket = -1;
    state->sequence = HEALTH_MONITOR_SEQ_INIT;
    state->running = false;

    // Copy query template
    memcpy(state->query_packet, health_query_template, HEALTH_MONITOR_QUERY_SIZE);

    // Initialize stats lock
    if (pthread_spin_init(&state->stats_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        fprintf(stderr, "[HEALTH] Failed to init stats lock\n");
        return -1;
    }

    // Get interface index
    state->if_index = get_interface_index(HEALTH_MONITOR_INTERFACE);
    if (state->if_index < 0) {
        fprintf(stderr, "[HEALTH] Interface not found: %s\n", HEALTH_MONITOR_INTERFACE);
        return -1;
    }
    printf("  Interface index: %d\n", state->if_index);

    // Create TX socket
    state->tx_socket = create_raw_socket(HEALTH_MONITOR_INTERFACE, state->if_index);
    if (state->tx_socket < 0) {
        fprintf(stderr, "[HEALTH] Failed to create TX socket\n");
        return -1;
    }
    printf("  TX socket created: fd=%d\n", state->tx_socket);

    // Create RX socket (separate from TX for clean separation)
    state->rx_socket = create_raw_socket(HEALTH_MONITOR_INTERFACE, state->if_index);
    if (state->rx_socket < 0) {
        fprintf(stderr, "[HEALTH] Failed to create RX socket\n");
        close(state->tx_socket);
        state->tx_socket = -1;
        return -1;
    }
    printf("  RX socket created: fd=%d\n", state->rx_socket);

    printf("[HEALTH] Initialization complete\n");
    return 0;
}

int start_health_monitor(volatile bool *stop_flag)
{
    struct health_monitor_state *state = &g_health_monitor;

    if (state->running) {
        fprintf(stderr, "[HEALTH] Already running\n");
        return -1;
    }

    if (state->tx_socket < 0 || state->rx_socket < 0) {
        fprintf(stderr, "[HEALTH] Not initialized\n");
        return -1;
    }

    g_stop_flag = stop_flag;
    state->running = true;

    // Create thread
    if (pthread_create(&state->thread, NULL, health_monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "[HEALTH] Failed to create thread: %s\n", strerror(errno));
        state->running = false;
        return -1;
    }

    printf("[HEALTH] Started\n");
    return 0;
}

void stop_health_monitor(void)
{
    struct health_monitor_state *state = &g_health_monitor;

    if (!state->running) {
        return;
    }

    printf("[HEALTH] Stopping...\n");
    state->running = false;

    // Wait for thread to finish
    pthread_join(state->thread, NULL);

    printf("[HEALTH] Stopped\n");
}

void cleanup_health_monitor(void)
{
    struct health_monitor_state *state = &g_health_monitor;

    // Stop if running
    if (state->running) {
        stop_health_monitor();
    }

    // Close sockets
    if (state->tx_socket >= 0) {
        close(state->tx_socket);
        state->tx_socket = -1;
    }

    if (state->rx_socket >= 0) {
        close(state->rx_socket);
        state->rx_socket = -1;
    }

    // Destroy lock
    pthread_spin_destroy(&state->stats_lock);

    printf("[HEALTH] Cleanup complete\n");
}

void get_health_monitor_stats(struct health_monitor_stats *stats)
{
    struct health_monitor_state *state = &g_health_monitor;

    pthread_spin_lock(&state->stats_lock);
    memcpy(stats, &state->stats, sizeof(*stats));
    pthread_spin_unlock(&state->stats_lock);
}

void print_health_monitor_stats(void)
{
    struct health_monitor_stats stats;
    get_health_monitor_stats(&stats);

    uint64_t expected = stats.queries_sent * HEALTH_MONITOR_EXPECTED_RESPONSES;
    double success_rate = (expected > 0) ?
        (100.0 * stats.responses_received / expected) : 0.0;

    printf("[HEALTH] Stats: Queries=%lu | Responses=%lu/%lu (%.1f%%) | Timeouts=%lu | Seq=0x%02X\n",
           (unsigned long)stats.queries_sent,
           (unsigned long)stats.responses_received,
           (unsigned long)expected,
           success_rate,
           (unsigned long)stats.timeouts,
           stats.current_sequence);
}

bool is_health_monitor_running(void)
{
    return g_health_monitor.running;
}

void health_monitor_set_warmup_complete(void)
{
    g_health_monitor.warmup_complete = true;
    g_health_monitor.post_warmup_cycle_count = 0;
    g_health_monitor.fw_check_done = false;
    g_health_monitor.power_status_check_done = false;
    printf("[HEALTH] Warmup complete notification received - FW check will run at cycle 10\n");
    if (g_health_monitor.power_status_check_enabled) {
        printf("[HEALTH] 28V Power Status check will also run at cycle 10\n");
    }
}

void health_monitor_set_expected_power_status(uint8_t expected_status)
{
    g_health_monitor.expected_power_status = expected_status;
    g_health_monitor.power_status_check_enabled = true;
    g_health_monitor.power_status_check_done = false;
    printf("[HEALTH] Expected 28V Power Status set: 0x%02X (Primary=%s, Secondary=%s)\n",
           expected_status,
           (expected_status & 0x01) ? "FAIL" : "SUCCESS",
           (expected_status & 0x02) ? "FAIL" : "SUCCESS");
}