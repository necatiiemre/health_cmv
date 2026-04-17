#ifndef HEALTH_TYPES_H
#define HEALTH_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// ==========================================
// PACKET STRUCTURE CONSTANTS
// ==========================================

// Offsets from raw packet start
#define HEALTH_ETH_HEADER_SIZE       14
#define HEALTH_IP_HEADER_SIZE        20
#define HEALTH_UDP_HEADER_SIZE       8
#define HEALTH_UDP_PAYLOAD_OFFSET    (HEALTH_ETH_HEADER_SIZE + HEALTH_IP_HEADER_SIZE + HEALTH_UDP_HEADER_SIZE)  // 42

// Device header size (from UDP payload start)
#define HEALTH_DEVICE_HEADER_SIZE    111

// Mini header size for packets without full device header
#define HEALTH_MINI_HEADER_SIZE      7   // DevID(2) + OpType(1) + CfgType(1) + FrameLen(2) + Reserved(1)

// Port data constants
#define HEALTH_PORT_DATA_SIZE        129
#define HEALTH_MAX_PORTS             35

// Packet sizes for type detection
#define HEALTH_PKT_SIZE_WITH_HEADER  1187  // Device header + 8 ports
#define HEALTH_PKT_SIZE_8_PORTS      1083  // 8 ports (no header)
#define HEALTH_PKT_SIZE_3_PORTS      438   // 3 ports
#define HEALTH_PKT_SIZE_MCU          94    // MCU data

// ==========================================
// FPGA TYPES
// ==========================================

// Per-cycle packet ordering:
//   pkt1 (1187) + pkt2 (1083) = Assistant FPGA  (16 ports)
//   pkt3 (1187) + pkt4 (1083) + pkt5 (438) = Manager FPGA (19 ports)
//   pkt6 (94) = MCU

typedef enum {
    FPGA_TYPE_ASSISTANT = 0,
    FPGA_TYPE_MANAGER   = 1,
    FPGA_TYPE_COUNT     = 2
} fpga_type_t;

// Status enable values for device identification (byte 6 of UDP payload)
#define STATUS_ENABLE_MANAGER    0x01  // Manager FPGA (ports 16-34)
#define STATUS_ENABLE_ASSISTANT  0x03  // Assistant FPGA (ports 0-15)
#define STATUS_ENABLE_MCU        0x05  // MCU

// Assistant FPGA: 2 packets (1187 + 1083) = 8 + 8 = 16 ports (ports 0-15)
#define ASSISTANT_EXPECTED_PACKETS   2
#define ASSISTANT_MAX_PORTS          16

// Manager FPGA: 3 packets (1187 + 1083 + 438) = 8 + 8 + 3 = 19 ports (ports 16-34)
#define MANAGER_EXPECTED_PACKETS     3
#define MANAGER_MAX_PORTS            19

// MCU: 1 packet (94 bytes)
#define MCU_EXPECTED_PACKETS         1

// Total expected per cycle
#define HEALTH_TOTAL_EXPECTED_PACKETS (ASSISTANT_EXPECTED_PACKETS + MANAGER_EXPECTED_PACKETS + MCU_EXPECTED_PACKETS)  // 6

// ==========================================
// DEVICE HEADER OFFSETS (from UDP payload)
// ==========================================

#define DEV_OFF_DEVICE_ID            0    // 2 bytes
#define DEV_OFF_OPERATION_TYPE       2    // 1 byte (ReadWriteFlag)
#define DEV_OFF_CONFIG_TYPE          3    // 1 byte (config type start address)
#define DEV_OFF_FRAME_LENGTH         4    // 2 bytes (monitoring data length)
#define DEV_OFF_STATUS_ENABLE        6    // 1 byte (assistant/manager flag)
#define DEV_OFF_STATUS_ADDR          7    // 2 bytes (status address)
#define DEV_OFF_TX_TOTAL_COUNT       9    // 6 bytes
#define DEV_OFF_RX_TOTAL_COUNT       15   // 6 bytes
#define DEV_OFF_TX_ERR_TOTAL_COUNT   21   // 6 bytes
#define DEV_OFF_RX_ERR_TOTAL_COUNT   27   // 6 bytes
#define DEV_OFF_HEARTBEAT            33   // 1 byte
#define DEV_OFF_DEV_ID2              34   // 2 bytes
#define DEV_OFF_PORT_COUNT           36   // 1 byte
#define DEV_OFF_TOKEN_BUCKET         37   // 1 byte
#define DEV_OFF_SW_MODE              38   // 1 byte
#define DEV_OFF_PADDING1             39   // 3 bytes
#define DEV_OFF_VENDOR_ID            42   // 1 byte
#define DEV_OFF_AUTO_MAC_UPDATE      43   // 1 byte
#define DEV_OFF_UPSTREAM_MODE        44   // 1 byte
#define DEV_OFF_SW_IP_CORE_VER       45   // 6 bytes
#define DEV_OFF_ES_IP_CORE_VER       51   // 6 bytes
#define DEV_OFF_SW_INPUT_FIFO        57   // 2 bytes
#define DEV_OFF_PKT_PRO_FIFO         59   // 2 bytes
#define DEV_OFF_SW_OUTPUT_FIFO       61   // 2 bytes
#define DEV_OFF_HP_FIFO_SIZE         63   // 2 bytes
#define DEV_OFF_LP_FIFO_SIZE         65   // 2 bytes
#define DEV_OFF_BE_FIFO_SIZE         67   // 2 bytes
#define DEV_OFF_PADDING2             69   // 1 byte
#define DEV_OFF_TOD_NS               70   // 5 bytes
#define DEV_OFF_PADDING3             75   // 1 byte
#define DEV_OFF_TOD_SEC              76   // 5 bytes
#define DEV_OFF_ETH_WRONG_DEV_CNT   81   // 6 bytes
#define DEV_OFF_ETH_WRONG_OP_CNT    87   // 6 bytes
#define DEV_OFF_ETH_WRONG_TYPE_CNT  93   // 6 bytes
#define DEV_OFF_RESERVED1            99   // 2 bytes
#define DEV_OFF_FPGA_VOLTAGE         101  // 2 bytes
#define DEV_OFF_FPGA_TEMP            103  // 2 bytes
#define DEV_OFF_CONFIG_ID            105  // 2 bytes
#define DEV_OFF_RESERVED2            107  // 4 bytes

// ==========================================
// PORT DATA OFFSETS (from port data start)
// ==========================================

#define PORT_OFF_PORT_NUMBER         0    // 2 bytes (monitoring port address)
#define PORT_OFF_BIT_STATUS          2    // 1 byte (upper 4: bit test result, lower 4: get port stats)
#define PORT_OFF_CRC_ERR_CNT         3    // 6 bytes
#define PORT_OFF_ALI_ERR_CNT         9    // 6 bytes (alignment error)
#define PORT_OFF_LEN_EXC_64          15   // 6 bytes (length exceed count 64)
#define PORT_OFF_LEN_EXC_1518        21   // 6 bytes (length exceed count 1518)
#define PORT_OFF_MIN_VL_FRAME_ERR    27   // 6 bytes (length exceed vl min)
#define PORT_OFF_MAX_VL_FRAME_ERR    33   // 6 bytes (length exceed vl max)
#define PORT_OFF_INP_PORT_TERR_CNT   39   // 6 bytes (input port terror count)
#define PORT_OFF_TRAFFIC_POLICY_DROP 45   // 6 bytes (traffic filter count)
#define PORT_OFF_BE_COUNT            51   // 6 bytes (consider count)
#define PORT_OFF_TX_COUNT            57   // 6 bytes
#define PORT_OFF_RX_COUNT            63   // 6 bytes
#define PORT_OFF_VL_SOURCE_ERR       69   // 6 bytes (count err vl)
#define PORT_OFF_MAX_DELAY_ERR       75   // 6 bytes (count err over max delay)
#define PORT_OFF_QUEUE_OVERFLOW      81   // 6 bytes (count err queue overflow)
#define PORT_OFF_VLID_DROP           87   // 6 bytes (undefined vl err count)
#define PORT_OFF_UNDEF_MAC           93   // 6 bytes (undefined be mac err count)
#define PORT_OFF_HP_QUEUE_OVERFLOW   99   // 6 bytes
#define PORT_OFF_LP_QUEUE_OVERFLOW   105  // 6 bytes
#define PORT_OFF_BE_QUEUE_OVERFLOW   111  // 6 bytes
#define PORT_OFF_MAX_DELAY_PARAM     117  // 6 bytes (max delay parameter)
#define PORT_OFF_PORT_SPEED          123  // 6 bytes (0=1000M, 1=10M, 2=100M)

// ==========================================
// DATA STRUCTURES
// ==========================================

/**
 * @brief Device header information (parsed from 1187-byte response)
 */
struct health_device_info {
    uint16_t device_id;           // Device ID
    uint8_t  operation_type;      // ReadWriteFlag (e.g., 0x53)
    uint8_t  config_type;         // Config type start address (e.g., 0x44)
    uint16_t frame_length;        // Monitoring data (frame) length
    uint8_t  status_enable;       // Status enable (assistant/manager indicator)
    uint16_t status_addr;         // Status address
    uint64_t tx_total_count;      // Total TX frame count
    uint64_t rx_total_count;      // Total RX frame count
    uint64_t tx_err_total_count;  // Total error TX frame count
    uint64_t rx_err_total_count;  // Total error RX frame count
    uint8_t  heartbeat;           // Heartbeat counter
    uint16_t device_id2;          // Device ID (repeated)
    uint8_t  port_count;          // Number of ports (e.g., 0x23 = 35)
    uint8_t  token_bucket_status; // Token bucket status
    uint8_t  sw_mode;             // Switch mode
    uint8_t  vendor_id;           // Vendor ID
    uint8_t  auto_mac_update;     // Auto MAC update
    uint8_t  upstream_mode;       // Upstream mode
    uint8_t  sw_ip_major;         // SW IP core version major
    uint8_t  sw_ip_minor;         // SW IP core version minor
    uint8_t  sw_ip_patch;         // SW IP core version patch
    uint8_t  es_ip_major;         // ES IP core version major
    uint8_t  es_ip_minor;         // ES IP core version minor
    uint8_t  es_ip_patch;         // ES IP core version patch
    uint16_t sw_input_fifo_size;  // SW input FIFO size
    uint16_t pkt_pro_fifo_size;   // Packet pro output FIFO size
    uint16_t sw_output_fifo_size; // SW output FIFO size
    uint16_t hp_fifo_size;        // High priority FIFO size
    uint16_t lp_fifo_size;        // Low priority FIFO size
    uint16_t be_fifo_size;        // Best effort FIFO size
    uint64_t tod_ns;              // Time of day nanoseconds (5 bytes)
    uint64_t tod_sec;             // Time of day seconds (5 bytes)
    uint64_t eth_wrong_dev_cnt;   // Eth conf wrong device ID count
    uint64_t eth_wrong_op_cnt;    // Eth conf wrong operation mode count
    uint64_t eth_wrong_type_cnt;  // Eth conf wrong type count
    uint16_t fpga_voltage;        // FPGA voltage (raw)
    int16_t  fpga_temp;           // FPGA temperature (raw, signed)
    uint16_t config_id;           // Configuration ID
};

/**
 * @brief Port monitoring data (parsed from response)
 */
struct health_port_info {
    uint16_t port_number;         // Monitoring port address
    uint8_t  bit_status;          // Upper 4 bit: bit test result, Lower 4 bit: get port stats
    uint64_t crc_err_count;       // CRC error count
    uint64_t ali_err_count;       // Alignment error count
    uint64_t len_exc_64;          // Length exceed count 64
    uint64_t len_exc_1518;        // Length exceed count 1518
    uint64_t min_vl_frame_err;    // Length exceed VL min
    uint64_t max_vl_frame_err;    // Length exceed VL max
    uint64_t inp_port_terr_cnt;   // Input port terror count
    uint64_t traffic_policy_drop; // Traffic filter count
    uint64_t be_count;            // Consider count
    uint64_t tx_count;            // TX frame count
    uint64_t rx_count;            // RX frame count
    uint64_t vl_source_err;       // Count error VL
    uint64_t max_delay_err;       // Count error over max delay
    uint64_t queue_overflow;      // Count error queue overflow
    uint64_t vlid_drop_count;     // Undefined VL error count
    uint64_t undef_mac_count;     // Undefined BE MAC error count
    uint64_t hp_queue_overflow;   // High priority queue overflow count
    uint64_t lp_queue_overflow;   // Low priority queue overflow count
    uint64_t be_queue_overflow;   // Best effort queue overflow count
    uint64_t max_delay_param;     // Max delay parameter
    uint64_t port_speed;          // Port speed (0=1000M, 1=10M, 2=100M)
    bool     valid;               // Data received flag
};

// ==========================================
// MCU DATA OFFSETS (from UDP payload)
// ==========================================

#define MCU_OFF_DEVICE_ID            0    // 2 bytes
#define MCU_OFF_OPERATION_TYPE       2    // 1 byte (ReadWriteFlag)
#define MCU_OFF_CONFIG_TYPE          3    // 1 byte
#define MCU_OFF_FRAME_LENGTH         4    // 2 bytes
#define MCU_OFF_STATUS_ENABLE        6    // 1 byte
#define MCU_OFF_FW_VERSION           7    // 2 bytes (MCU firmware version major.minor.patch)
#define MCU_OFF_INPUT_POWER_STATUS   9    // 1 byte (bit0: 28V Primary, bit1: 28V Secondary; 0=SUCCESS, 1=FAIL)
#define MCU_OFF_PBIT                 10   // 1 byte (DTN IRSW CBA components PBIT: 0x00=SUCCESS, 0x01=FAIL)
#define MCU_OFF_CBIT                 11   // 1 byte (DTN IRSW CBA components CBIT: 0x00=SUCCESS, 0x01=FAIL)
// Current data (readValue/1000)
#define MCU_OFF_CURR_12V             12   // 2 bytes (12V current)
#define MCU_OFF_CURR_3V3             14   // 2 bytes (3.3V current)
#define MCU_OFF_CURR_1V8             16   // 2 bytes (1.8V current)
#define MCU_OFF_CURR_3V3_FO          18   // 2 bytes (3.3V FO transceiver current)
#define MCU_OFF_CURR_1V3             20   // 2 bytes (1.3V current)
#define MCU_OFF_CURR_1V0_MGR         22   // 2 bytes (1.0V DTN IRSW manager FPGA current)
#define MCU_OFF_CURR_1V0_AST         24   // 2 bytes (1.0V DTN IRSW assistant FPGA current)
// Voltage data (readValue/1000)
#define MCU_OFF_VOLT_3V3             26   // 2 bytes (3.3V voltage)
#define MCU_OFF_VOLT_3V3_FO          28   // 2 bytes (3.3V FO transceiver voltage)
#define MCU_OFF_VOLT_12V             30   // 2 bytes (12V voltage)
#define MCU_OFF_VOLT_1V8             32   // 2 bytes (1.8V VCCIO voltage)
#define MCU_OFF_VOLT_1V3             34   // 2 bytes (1.3V VCC voltage)
#define MCU_OFF_VOLT_1V0_MGR         36   // 2 bytes (1.0V VDD DTN IRSW manager FPGA voltage)
#define MCU_OFF_VOLT_1V0_AST         38   // 2 bytes (1.0V VDD DTN IRSW assistant FPGA voltage)
// Temperature data
#define MCU_OFF_BOARD_TEMP           40   // 2 bytes (DTN IRSW CBA temperature, readValue/100)
#define MCU_OFF_FO_TRANS1_TEMP       42   // 2 bytes (FO transceiver 1 temperature, readValue/100)
#define MCU_OFF_RESERVED1            44   // 2 bytes (reserved)
#define MCU_OFF_RESERVED2            46   // 2 bytes (reserved)
#define MCU_OFF_ETH_PHY_1G_TEMP      48   // 1 byte (Ethernet PHY 1G temperature, signed)
#define MCU_OFF_ETH_PHY_100M_TEMP    49   // 1 byte (Ethernet PHY 100mbit temperature, signed)

/**
 * @brief MCU data (parsed from 94-byte response)
 *
 * Current/voltage values: raw / 1000.0
 * Board/FO temperature: signed raw / 100.0
 * PHY temperatures: signed raw value
 */
struct health_mcu_info {
    // Header
    uint16_t device_id;
    uint8_t  operation_type;
    uint8_t  config_type;
    uint16_t frame_length;
    uint8_t  status_enable;
    uint8_t  fw_major;            // MCU firmware version major (high nibble of byte 0)
    uint8_t  fw_minor;            // MCU firmware version minor (low nibble of byte 0)
    uint8_t  fw_patch;            // MCU firmware version patch (byte 1)
    uint8_t  input_power_status;  // bit0: 28V Primary, bit1: 28V Secondary (0=SUCCESS, 1=FAIL)
    uint8_t  pbit;                // DTN IRSW CBA components PBIT (0x00=SUCCESS, 0x01=FAIL)
    uint8_t  cbit;                // DTN IRSW CBA components CBIT (0x00=SUCCESS, 0x01=FAIL)

    // Current data (raw value, divide by 1000 for display)
    uint16_t curr_12v;
    uint16_t curr_3v3;
    uint16_t curr_1v8;
    uint16_t curr_3v3_fo;         // 3.3V FO transceiver
    uint16_t curr_1v3;
    uint16_t curr_1v0_mgr;       // 1.0V DTN IRSW manager FPGA
    uint16_t curr_1v0_ast;       // 1.0V DTN IRSW assistant FPGA

    // Voltage data (raw value, divide by 1000 for display)
    uint16_t volt_3v3;
    uint16_t volt_3v3_fo;         // 3.3V FO transceiver
    uint16_t volt_12v;
    uint16_t volt_1v8;            // 1.8V VCCIO
    uint16_t volt_1v3;            // 1.3V VCC
    uint16_t volt_1v0_mgr;       // 1.0V VDD DTN IRSW manager FPGA
    uint16_t volt_1v0_ast;       // 1.0V VDD DTN IRSW assistant FPGA

    // Temperature data
    int16_t  board_temp;          // DTN IRSW CBA temperature (raw / 100.0, signed)
    int16_t  fo_trans1_temp;      // FO transceiver 1 temperature (raw / 100.0, signed)
    int8_t   eth_phy_1g_temp;     // Ethernet PHY 1G temperature (signed)
    int8_t   eth_phy_100m_temp;   // Ethernet PHY 100mbit temperature (signed)

    bool     valid;
};

/**
 * @brief Per-FPGA cycle data (device info + ports)
 */
struct health_fpga_data {
    struct health_device_info device;               // FPGA device info
    struct health_port_info   ports[HEALTH_MAX_PORTS]; // Port data
    uint8_t  packets_received;                      // Packets received for this FPGA
    bool     device_info_valid;                     // Device info parsed flag
    uint8_t  port_count_received;                   // Number of ports parsed
};

/**
 * @brief Complete health cycle data (assistant + manager + MCU)
 */
struct health_cycle_data {
    struct health_fpga_data  assistant;             // Assistant FPGA data
    struct health_fpga_data  manager;               // Manager FPGA data
    struct health_mcu_info   mcu;                   // MCU data
    uint8_t  total_responses_received;              // Total responses this cycle
    uint8_t  last_fpga_type;                        // Last identified FPGA from 1187-byte packet
                                                    // 0=none, STATUS_ENABLE_ASSISTANT or STATUS_ENABLE_MANAGER
};

#endif // HEALTH_TYPES_H