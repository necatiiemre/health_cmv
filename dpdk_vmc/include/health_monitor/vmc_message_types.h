#ifndef VMC_MESSAGE_TYPES_H
#define VMC_MESSAGE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// ============================================================================

// COMMON HEADER
typedef struct __attribute__((packed)) 
{
    uint8_t  message_identifier;            // Byte 0        | 1 byte
    uint16_t message_len;                   // Byte 1 - 2    | 2 byte
    uint64_t timestamp;                     // Byte 3 - 10   | 8 byte
} vmp_cmsw_header_t;                        // TOPLAM: 11 byte

typedef struct __attribute__((packed)) 
{
    vmp_cmsw_header_t header_st;
} pbit_result_req_mes_t;


// ============================================================================
//
// PBIT REPORT  --  FLCS VL = 10 // VS VL = 13
//
// ============================================================================

// 1. VMP PBIT REPORT STRUCTURES  
typedef struct __attribute__((packed)) 
{
    uint8_t flcs_cpu_status         : 1;    // Byte 449, bit 0
    uint8_t vs_cpu_status           : 1;    // Byte 449, bit 1
    uint8_t eMMC_storage_status     : 1;    // Byte 449, bit 2
    uint8_t MRAM_storage_status     : 1;    // Byte 449, bit 3
    uint8_t reserved                : 4;    // Byte 449, bit 4 - 7
} vmp_storage_and_status_t;                 // TOPLAM: 1 byte

typedef struct __attribute__((packed)) 
{
    uint8_t major;                          // Byte 446        | 1 byte
    uint8_t minor;                          // Byte 447        | 1 byte
    uint8_t bugfix;                         // Byte 448        | 1 byte
} vmp_cmsw_lib_ver_t;                       // TOPLAM: 3 byte

typedef struct __attribute__((packed)) 
{
    uint32_t reserved_2;                    // Byte 438 - 441  | 4 byte
    uint8_t  reserved_1;                    // Byte 442        | 1 byte
    uint8_t  major;                         // Byte 443        | 1 byte
    uint8_t  minor;                         // Byte 444        | 1 byte
    uint8_t  bugfix;                        // Byte 445        | 1 byte
} vmp_dtn_sw_fw_version_t;                  // TOPLAM: 8 byte

typedef struct __attribute__((packed)) 
{
    uint32_t reserved_2;                    // Byte 430 - 433  | 4 byte
    uint8_t  reserved_1;                    // Byte 434        | 1 byte
    uint8_t  major;                         // Byte 435        | 1 byte
    uint8_t  minor;                         // Byte 436        | 1 byte
    uint8_t  bugfix;                        // Byte 437        | 1 byte
} vmp_dtn_sw_es_fw_version_t;               // TOPLAM: 8 byte

typedef struct __attribute__((packed)) 
{
    uint32_t reserved_2;                    // Byte 422 - 425  | 4 byte
    uint8_t  reserved_1;                    // Byte 426        | 1 byte
    uint8_t  major;                         // Byte 427        | 1 byte
    uint8_t  minor;                         // Byte 428        | 1 byte
    uint8_t  bugfix;                        // Byte 429        | 1 byte
} mmp_dtn_es_fw_version_t;                  // TOPLAM: 8 byte

typedef struct __attribute__((packed)) 
{
    mmp_dtn_es_fw_version_t     mmp_dtn_es_fw_version;      // Byte 422 - 429  | 8 byte
    vmp_dtn_sw_es_fw_version_t  vmp_dtn_sw_es_fw_version;   // Byte 430 - 437  | 8 byte
    vmp_dtn_sw_fw_version_t     vmp_dtn_sw_fw_version;      // Byte 438 - 445  | 8 byte
} dtn_pbit_data_t;                                          // TOPLAM: 24 byte

typedef struct __attribute__((packed)) 
{
    uint8_t policy_cmd;                     // list[0] için Byte 14        | 1 byte
    int32_t ret_val;                        // list[0] için Byte 15 - 18   | 4 byte
} policy_step_exec_t;                       // TOPLAM: 5 byte (80 kez tekrar, Byte 14 - 413)

typedef struct __attribute__((packed)) 
{
    uint8_t reserved;                       // Byte 414        | 1 byte
    uint8_t major;                          // Byte 415        | 1 byte
    uint8_t minor;                          // Byte 416        | 1 byte
    uint8_t bugfix;                         // Byte 417        | 1 byte
} bm_cd_firmware_version_t;                 // TOPLAM: 4 byte

typedef struct __attribute__((packed))
{
    vmp_cmsw_header_t        header_st;                      // Byte 0   - 10   | 11 byte
    uint8_t                  lru_id;                         // Byte 11         | 1 byte
    uint8_t                  number_of_policy_step;          // Byte 12         | 1 byte
    uint8_t                  policy_steps_exec_status;       // Byte 13         | 1 byte
    policy_step_exec_t       list[80];                       // Byte 14  - 413  | 400 byte
    bm_cd_firmware_version_t bm_cd_firmware_version_st;      // Byte 414 - 417  | 4 byte
    uint32_t                 vmc_serial_number;              // Byte 418 - 421  | 4 byte
    dtn_pbit_data_t          dtn_pbit_data_st;               // Byte 422 - 445  | 24 byte
    vmp_cmsw_lib_ver_t       vmp_cmsw_lib_ver;               // Byte 446 - 448  | 3 byte
    vmp_storage_and_status_t vmp_storage_and_status_st;      // Byte 449        | 1 byte
    uint16_t                 vs_cpu_pbit;                    // Byte 450 - 451  | 2 byte
    uint16_t                 flcs_cpu_pbit;                  // Byte 452 - 453  | 2 byte
} vmc_pbit_data_t;                                           // TOPLAM: 454 byte                                   

// ============================================================================
// 
// CBIT REPORTS  -- FLCS VL = 11 / VS VL = 14
//
// ============================================================================

// 2. BM ENGINEERING & FLAG DATA CBIT REPORT STRUCTURES
typedef struct __attribute__((packed)) 
{
    float VSCPU_12V_current;                    // Byte 13  - 16   | 4 byte
    float VSCPU_core_imon;                      // Byte 17  - 20   | 4 byte
    float VSCPU_3v3_rail_input_current;         // Byte 21  - 24   | 4 byte
    float VSCPU_G1VDD_input_current;            // Byte 25  - 28   | 4 byte
    float VSCPU_XVDD_input_current;             // Byte 29  - 32   | 4 byte
    float VSCPU_SVDD_input_current;             // Byte 33  - 36   | 4 byte
    float VSCPU_G1VDD_1v35_rail_voltage;        // Byte 37  - 40   | 4 byte
    float VSCPU_BM_ADC_3V_1;                    // Byte 41  - 44   | 4 byte
    float VSCPU_1v8_rail_voltage;               // Byte 45  - 48   | 4 byte
    float VSCPU_VCORE_rail_voltage;             // Byte 49  - 52   | 4 byte
    float VSCPU_S1VDD_rail_voltage;             // Byte 53  - 56   | 4 byte
    float VSCPU_S2VDD_rail_voltage;             // Byte 57  - 60   | 4 byte
    float VSCPU_X1VDD_rail_voltage;             // Byte 61  - 64   | 4 byte
    float VSCPU_X2VDD_rail_voltage;             // Byte 65  - 68   | 4 byte
    float VSCPU_12V_main_voltage;               // Byte 69  - 72   | 4 byte
    float VSCPU_BM_ADC_3V_2;                    // Byte 73  - 76   | 4 byte
    float VSCPU_1v8_rail_input_current;         // Byte 77  - 80   | 4 byte
    float VSCPU_core_local_temperature;         // Byte 81  - 84   | 4 byte
    float VSCPU_core_remote_temperature;        // Byte 85  - 88   | 4 byte
    float VSCPU_RAM_temperature;                // Byte 89  - 92   | 4 byte
    float VSCPU_FLASH_temperature;              // Byte 93  - 96   | 4 byte
    float VSCPU_power_IC_temperature;           // Byte 97  - 100  | 4 byte
} bm_vs_cpu_status_data_t;                      // TOPLAM: 88 byte

typedef struct __attribute__((packed)) 
{
    float FCCPU_12V_current;                    // Byte 101 - 104  | 4 byte
    float FCCPU_core_imon;                      // Byte 105 - 108  | 4 byte
    float FCCPU_3v3_rail_input_current;         // Byte 109 - 112  | 4 byte
    float FCCPU_G1VDD_input_current;            // Byte 113 - 116  | 4 byte
    float FCCPU_XVDD_input_current;             // Byte 117 - 120  | 4 byte
    float FCCPU_SVDD_input_current;             // Byte 121 - 124  | 4 byte
    float FCCPU_G1VDD_1v35_rail_voltage;        // Byte 125 - 128  | 4 byte
    float FCCPU_BM_ADC_3V_1;                    // Byte 129 - 132  | 4 byte
    float FCCPU_1v8_rail_voltage;               // Byte 133 - 136  | 4 byte
    float FCCPU_VCORE_rail_voltage;             // Byte 137 - 140  | 4 byte
    float FCCPU_S1VDD_rail_voltage;             // Byte 141 - 144  | 4 byte
    float FCCPU_S2VDD_rail_voltage;             // Byte 145 - 148  | 4 byte
    float FCCPU_X1VDD_rail_voltage;             // Byte 149 - 152  | 4 byte
    float FCCPU_X2VDD_rail_voltage;             // Byte 153 - 156  | 4 byte
    float FCCPU_12V_main_voltage;               // Byte 157 - 160  | 4 byte
    float FCCPU_BM_ADC_3V_2;                    // Byte 161 - 164  | 4 byte
    float FCCPU_core_local_temperature;         // Byte 165 - 168  | 4 byte
    float FCCPU_core_remote_temperature;        // Byte 169 - 172  | 4 byte
    float FCCPU_RAM_temperature;                // Byte 173 - 176  | 4 byte
    float FCCPU_FLASH_temperature;              // Byte 177 - 180  | 4 byte
    float FCCPU_1v8_input_current;              // Byte 181 - 184  | 4 byte
    float FCCPU_power_IC_temperature;           // Byte 185 - 188  | 4 byte
} bm_flcs_cpu_status_data_t;                    // TOPLAM: 88 byte

typedef struct __attribute__((packed)) 
{
    float DTN_ES_VDD_input_current;             // Byte 189 - 192  | 4 byte
    float DTN_ES_3v3_rail_input_current;        // Byte 193 - 196  | 4 byte
    float DTN_ES_FO_RX_imon_r;                  // Byte 197 - 200  | 4 byte
    float DTN_ES_1v8_rail_input_current;        // Byte 201 - 204  | 4 byte
    float DTN_ES_1v3_rail_input_current;        // Byte 205 - 208  | 4 byte
    float DTN_ES_12V_main_current;              // Byte 209 - 212  | 4 byte
    float DTN_ES_BM_3V_1;                       // Byte 213 - 216  | 4 byte
    float DTN_ES_1V_VDD_rail_voltage;           // Byte 217 - 220  | 4 byte
    float DTN_ES_2v5_rail_voltage;              // Byte 221 - 224  | 4 byte
    float DTN_ES_1v8_rail_voltage;              // Byte 225 - 228  | 4 byte
    float DTN_ES_1V_VDDA_rail_voltage;          // Byte 229 - 232  | 4 byte
    float DTN_ES_3v3_VDDIX_rail_voltage;        // Byte 233 - 236  | 4 byte
    float DTN_ES_2v5_rail_input_current;        // Byte 237 - 240  | 4 byte
    float DTN_ES_12V_main_voltage;              // Byte 241 - 244  | 4 byte
    float DTN_ES_BM_3V_2;                       // Byte 245 - 248  | 4 byte
} bm_dtn_es_status_data_t;                      // TOPLAM: 60 byte

typedef struct __attribute__((packed)) 
{
    float DTN_VSW_VDD_input_current;            // Byte 249 - 252  | 4 byte
    float DTN_VSW_3v3_rail_input_current;       // Byte 253 - 256  | 4 byte
    float DTN_VSW_FO_V_RX_imon_r;               // Byte 257 - 260  | 4 byte
    float DTN_VSW_1v8_rail_input_current;       // Byte 261 - 264  | 4 byte
    float DTN_VSW_1v3_rail_input_current;       // Byte 265 - 268  | 4 byte
    float DTN_VSW_12V_main_current;             // Byte 269 - 272  | 4 byte
    float DTN_VSW_BM_ADC_3V_1;                  // Byte 273 - 276  | 4 byte
    float DTN_VSW_1V_VDD_rail_voltage;          // Byte 277 - 280  | 4 byte
    float DTN_VSW_2v5_rail_voltage;             // Byte 281 - 284  | 4 byte
    float DTN_VSW_1v8_rail_voltage;             // Byte 285 - 288  | 4 byte
    float DTN_VSW_1V_VDDA_rail_voltage;         // Byte 289 - 292  | 4 byte
    float DTN_VSW_2v5_rail_input_current;       // Byte 293 - 296  | 4 byte
    float DTN_VSW_12V_main_voltage;             // Byte 297 - 300  | 4 byte
    float DTN_VSW_FO_VF_RX_imon_r;              // Byte 301 - 304  | 4 byte
    float DTN_VSW_3v3_VDDIX_voltage;            // Byte 305 - 308  | 4 byte
} bm_vs_dtn_sw_status_data_t;                   // TOPLAM: 60 byte

typedef struct __attribute__((packed)) 
{
    float DTN_FSW_VDD_input_current;            // Byte 309 - 312  | 4 byte
    float DTN_FSW_3v3_rail_input_current;       // Byte 313 - 316  | 4 byte
    float DTN_FSW_FO_F_RX_imon_r;               // Byte 317 - 320  | 4 byte
    float DTN_FSW_1v8_rail_input_current;       // Byte 321 - 324  | 4 byte
    float DTN_FSW_1v3_rail_input_current;       // Byte 325 - 328  | 4 byte
    float DTN_FSW_12V_main_current;             // Byte 329 - 332  | 4 byte
    float DTN_FSW_BM_ADC_3V_1;                  // Byte 333 - 336  | 4 byte
    float DTN_FSW_1V_VDD_rail_voltage;          // Byte 337 - 340  | 4 byte
    float DTN_FSW_2v5_rail_voltage;             // Byte 341 - 344  | 4 byte
    float DTN_FSW_1v8_rail_voltage;             // Byte 345 - 348  | 4 byte
    float DTN_FSW_1V_VDDA_rail_voltage;         // Byte 349 - 352  | 4 byte
    float DTN_FSW_3v3_VDDIX_rail_voltage;       // Byte 353 - 356  | 4 byte
    float DTN_FSW_2v5_rail_input_current;       // Byte 357 - 360  | 4 byte
    float DTN_FSW_12V_main_voltage;             // Byte 361 - 364  | 4 byte
    float DTN_FSW_BM_ADC_3V_2;                  // Byte 365 - 368  | 4 byte
} bm_flcs_dtn_sw_status_data_t;                 // TOPLAM: 60 byte

typedef struct __attribute__((packed)) 
{
    float PSM_PWR_PRI_VOLS;                     // Byte 369 - 372  | 4 byte
    float PSM_PWR_SEC_VOLS;                     // Byte 373 - 376  | 4 byte
    float PSM_INPUT_CURS;                       // Byte 377 - 380  | 4 byte
    float PSM_TEMP;                             // Byte 381 - 384  | 4 byte
    float BM_FPGA_temperature;                  // Byte 385 - 388  | 4 byte
    float Board_edge_temperature;               // Byte 389 - 392  | 4 byte
    float BRD_MNGR_12V_main_current;            // Byte 393 - 396  | 4 byte
} bm_vmc_board_status_data_t;                   // TOPLAM: 28 byte


typedef struct __attribute__((packed)) 
{
    vmp_cmsw_header_t            header_st;              // Byte 0   - 10   | 11 byte
    uint8_t                      lru_id;                 // Byte 11         | 1 byte
    uint8_t                      comm_status;            // Byte 12         | 1 byte
    bm_vs_cpu_status_data_t      vs_status_st;           // Byte 13  - 100  | 88 byte
    bm_flcs_cpu_status_data_t    flcs_status_st;         // Byte 101 - 188  | 88 byte
    bm_dtn_es_status_data_t      dtn_es_status_st;       // Byte 189 - 248  | 60 byte
    bm_vs_dtn_sw_status_data_t   vs_dtn_sw_status_st;    // Byte 249 - 308  | 60 byte
    bm_flcs_dtn_sw_status_data_t flcs_dtn_sw_status_st;  // Byte 309 - 368  | 60 byte
    bm_vmc_board_status_data_t   vmc_board_status_st;    // Byte 369 - 396  | 28 byte
} bm_engineering_cbit_report_t;                          // TOPLAM: 397 byte

// ============================================================================
// ============================================================================

// 3. DTN ES CBIT MESSAGES
typedef struct __attribute__((packed)) 
{
    uint64_t reserved : 40;                                     // Byte 15  - 19   | 5 byte
    uint8_t  major;                                             // Byte 20         | 1 byte
    uint8_t  minor;                                             // Byte 21         | 1 byte
    uint8_t  bugfix;                                            // Byte 22         | 1 byte
} A664_ES_FW_VER_t;                                             // TOPLAM: 8 byte

typedef struct __attribute__((packed)) 
{
    A664_ES_FW_VER_t A664_ES_FW_VER;                            // Byte 15  - 22   | 8 byte
    uint64_t A664_ES_DEV_ID;                                    // Byte 23  - 30   | 8 byte
    uint64_t A664_ES_MODE;                                      // Byte 31  - 38   | 8 byte
    uint64_t A664_ES_CONFIG_ID;                                 // Byte 39  - 46   | 8 byte
    uint64_t A664_ES_BIT_STATUS;                                // Byte 47  - 54   | 8 byte
    uint64_t A664_ES_CONFIG_STATUS;                             // Byte 55  - 62   | 8 byte
    uint16_t A664_PTP_CONFIG_ID;                                // Byte 63  - 64   | 2 byte
    uint8_t  A664_PTP_DEVICE_TYPE;                              // Byte 65         | 1 byte
    uint8_t  A664_PTP_RC_STATUS;                                // Byte 66         | 1 byte
    uint8_t  A664_PTP_PORT_A_SYNC;                              // Byte 67         | 1 byte
    uint8_t  A664_PTP_PORT_B_SYNC;                              // Byte 68         | 1 byte
    uint16_t A664_PTP_SYNC_VL_ID;                               // Byte 69  - 70   | 2 byte
    uint16_t A664_PTP_REQ_VL_ID;                                // Byte 71  - 72   | 2 byte
    uint16_t A664_PTP_RES_VL_ID;                                // Byte 73  - 74   | 2 byte
    uint8_t  A664_PTP_TOD_NETWORK;                              // Byte 75         | 1 byte
    uint32_t A664_ES_HW_TEMP;                                   // Byte 76  - 79   | 4 byte
    uint32_t A664_ES_HW_VCC_INT;                                // Byte 80  - 83   | 4 byte
    uint64_t A664_ES_TRANSCEIVER_TEMP;                          // Byte 84  - 91   | 8 byte
    uint64_t A664_ES_PORT_A_STATUS;                             // Byte 92  - 99   | 8 byte
    uint64_t A664_ES_PORT_B_STATUS;                             // Byte 100 - 107  | 8 byte
    uint64_t A664_ES_TX_INCOMING_COUNT;                         // Byte 108 - 115  | 8 byte
    uint64_t A664_ES_TX_A_OUTGOING_COUNT;                       // Byte 116 - 123  | 8 byte
    uint64_t A664_ES_TX_B_OUTGOING_COUNT;                       // Byte 124 - 131  | 8 byte
    uint64_t A664_ES_TX_VLID_DROP_COUNT;                        // Byte 132 - 139  | 8 byte
    uint64_t A664_ES_TX_LMIN_LMAX_DROP_COUNT;                   // Byte 140 - 147  | 8 byte
    uint64_t A664_ES_TX_MAX_JITTER_DROP_COUNT;                  // Byte 148 - 155  | 8 byte
    uint64_t A664_ES_RX_A_INCOMING_COUNT;                       // Byte 156 - 163  | 8 byte
    uint64_t A664_ES_RX_B_INCOMING_COUNT;                       // Byte 164 - 171  | 8 byte
    uint64_t A664_ES_RX_OUTGOING_COUNT;                         // Byte 172 - 179  | 8 byte
    uint64_t A664_ES_RX_A_VLID_DROP_COUNT;                      // Byte 180 - 187  | 8 byte
    uint64_t A664_ES_RX_A_LMIN_LMAX_DROP_COUNT;                 // Byte 188 - 195  | 8 byte
    uint64_t A664_ES_RX_A_NET_ERR_COUNT;                        // Byte 196 - 203  | 8 byte
    uint64_t A664_ES_RX_A_SEQ_ERR_COUNT;                        // Byte 204 - 211  | 8 byte
    uint64_t A664_ES_RX_A_CRC_ERROR_COUNT;                      // Byte 212 - 219  | 8 byte
    uint64_t A664_ES_RX_A_IP_CHECKSUM_ERROR_COUNT;              // Byte 220 - 227  | 8 byte
    uint64_t A664_ES_RX_B_VLID_DROP_COUNT;                      // Byte 228 - 235  | 8 byte
    uint64_t A664_ES_RX_B_LMIN_LMAX_DROP_COUNT;                 // Byte 236 - 243  | 8 byte
    uint64_t A664_ES_RX_B_SEQ_ERR_COUNT;                        // Byte 244 - 251  | 8 byte
    uint64_t A664_ES_RX_B_NET_ERR_COUNT;                        // Byte 252 - 259  | 8 byte
    uint64_t A664_ES_RX_B_CRC_ERROR_COUNT;                      // Byte 260 - 267  | 8 byte
    uint64_t A664_ES_RX_B_IP_CHECKSUM_ERROR_COUNT;              // Byte 268 - 275  | 8 byte
    uint64_t A664_BSP_TX_PACKET_COUNT;                          // Byte 276 - 283  | 8 byte
    uint64_t A664_BSP_TX_BYTE_COUNT;                            // Byte 284 - 291  | 8 byte
    uint64_t A664_BSP_TX_ERROR_COUNT;                           // Byte 292 - 299  | 8 byte
    uint64_t A664_BSP_RX_PACKET_COUNT;                          // Byte 300 - 307  | 8 byte
    uint64_t A664_BSP_RX_BYTE_COUNT;                            // Byte 308 - 315  | 8 byte
    uint64_t A664_BSP_RX_ERROR_COUNT;                           // Byte 316 - 323  | 8 byte
    uint64_t A664_BSP_RX_MISSED_FRAME_COUNT;                    // Byte 324 - 331  | 8 byte
    uint64_t A664_BSP_VER;                                      // Byte 332 - 339  | 8 byte
    uint64_t A664_ES_VENDOR_TYPE;                               // Byte 340 - 347  | 8 byte
    uint64_t A664_ES_BSP_QUEUING_RX_VL_PORT_DROP_COUNT;         // Byte 348 - 355  | 8 byte
} dtn_es_monitoring_t;                                          // TOPLAM: 341 byte

typedef struct __attribute__((packed)) 
{
    vmp_cmsw_header_t   header_st;                              // Byte 0   - 10   | 11 byte
    uint8_t             lru_id;                                 // Byte 11         | 1 byte
    uint8_t             side_type;                              // Byte 12         | 1 byte
    uint8_t             network_type;                           // Byte 13         | 1 byte
    uint8_t             comm_status;                            // Byte 14         | 1 byte
    dtn_es_monitoring_t dtn_es_monitoring_st;                   // Byte 15  - 355  | 341 byte
} dtn_es_cbit_report_t;                                         // TOPLAM: 356 byte

// ============================================================================
// ============================================================================

// 4. DTN SW CBIT MESSAGES
typedef struct __attribute__((packed)) 
{
    uint64_t A664_SW_TX_TOTAL_COUNT;                            // Byte 15  - 22   | 8 byte
    uint64_t A664_SW_RX_TOTAL_COUNT;                            // Byte 23  - 30   | 8 byte
    uint64_t A664_SW_TRANSCEIVER_TEMP;                          // Byte 31  - 38   | 8 byte
    uint64_t A664_SW_SHARED_TRANSCEIVER_TEMP;                   // Byte 39  - 46   | 8 byte
    uint16_t A664_SW_DEV_ID;                                    // Byte 47  - 48   | 2 byte
    uint8_t  A664_SW_PORT_COUNT;                                // Byte 49         | 1 byte
    uint8_t  A664_SW_TOKEN_BUCKET;                              // Byte 50         | 1 byte
    uint8_t  A664_SW_MODE;                                      // Byte 51         | 1 byte
    uint8_t  A664_SW_BE_MAC_UPDATE;                             // Byte 52         | 1 byte
    uint8_t  A664_SW_BE_UPSTREAM_MODE;                          // Byte 53         | 1 byte
    uint64_t A664_SW_FW_VER;                                    // Byte 54  - 61   | 8 byte
    uint64_t A664_SW_EMBEDEED_ES_FW_VER;                        // Byte 62  - 69   | 8 byte
    uint16_t A664_SW_VOLTAGE;                                   // Byte 70  - 71   | 2 byte
    uint16_t A664_SW_TEMPERATURE;                               // Byte 72  - 73   | 2 byte
    uint16_t A664_SW_CONFIGURATION_ID;                          // Byte 74  - 75   | 2 byte
} dtn_sw_status_mon_t;                                          // TOPLAM: 61 byte

typedef struct __attribute__((packed)) 
{
    uint16_t A664_SW_PORT_ID;                                   // Byte 76  - 77   | 2 byte  (port[0] başlangıcı)
    uint8_t  A664_SW_PORT_i_BIT_STATUS;                         // Byte 78         | 1 byte
    uint8_t  A664_SW_PORT_i_STATUS;                             // Byte 79         | 1 byte
    uint64_t A664_SW_PORT_i_CRC_ERR_COUNT;                      // Byte 80  - 87   | 8 byte
    uint64_t A664_SW_PORT_i_MIN_VL_FRAME_ERR_COUNT;             // Byte 88  - 95   | 8 byte
    uint64_t A664_SW_PORT_i_MAX_VL_FRAME_ERR_COUNT;             // Byte 96  - 103  | 8 byte
    uint64_t A664_SW_PORT_i_TRAFFIC_POLCY_DROP_COUNT;           // Byte 104 - 111  | 8 byte
    uint64_t A664_SW_PORT_i_BE_COUNT;                           // Byte 112 - 119  | 8 byte
    uint64_t A664_SW_PORT_i_TX_COUNT;                           // Byte 120 - 127  | 8 byte
    uint64_t A664_SW_PORT_i_RX_COUNT;                           // Byte 128 - 135  | 8 byte
    uint64_t A664_SW_PORT_i_VL_SOURCE_ERR_COUNT;                // Byte 136 - 143  | 8 byte
    uint64_t A664_SW_PORT_i_MAX_DELAY_ERR_COUNT;                // Byte 144 - 151  | 8 byte
    uint64_t A664_SW_PORT_i_VLID_DROP_COUNT;                    // Byte 152 - 159  | 8 byte
    uint64_t A664_SW_PORT_i_UNDEF_MAC_COUNT;                    // Byte 160 - 167  | 8 byte
    uint64_t A664_SW_PORT_i_HIGH_PRTY_QUE_OVRFLW_COUNT;         // Byte 168 - 175  | 8 byte
    uint64_t A664_SW_PORT_i_LOW_PRTY_QUE_OVRFLW_COUNT;          // Byte 176 - 183  | 8 byte
    uint64_t A664_SW_PORT_i_BE_QUE_OVRFLW_COUNT;                // Byte 184 - 191  | 8 byte
    uint64_t A664_SW_PORT_i_MAX_DELAY;                          // Byte 192 - 199  | 8 byte
    uint64_t A664_SW_PORT_i_SPEED;                              // Byte 200 - 207  | 8 byte
} dtn_sw_port_mon_t;                                            // TOPLAM: 132 byte (her bir eleman)

typedef struct __attribute__((packed)) 
{
    dtn_sw_status_mon_t status;                                 // Byte 15  - 75   | 61 byte
    dtn_sw_port_mon_t   port[8];                                // Byte 76  - 1131 | 1056 byte (132 x 8)
} dtn_sw_monitoring_t;                                          // TOPLAM: 1117 byte

typedef struct __attribute__((packed)) 
{
    vmp_cmsw_header_t   header_st;                              // Byte 0    - 10    | 11 byte
    uint8_t             lru_id;                                 // Byte 11           | 1 byte
    uint8_t             side_type;                              // Byte 12           | 1 byte
    uint8_t             network_type;                           // Byte 13           | 1 byte
    uint8_t             comm_status;                            // Byte 14           | 1 byte
    dtn_sw_monitoring_t dtn_sw_monitoring_st;                   // Byte 15   - 1131  | 1117 byte
} dtn_sw_cbit_report_t;                                         // TOPLAM: 1132 byte

// ============================================================================
//
// CPU USAGE  --  FLCS VL =   // VS VL = 
//
// ============================================================================

// 4. CPU USAGE
// NOT: Pcs_monitor_type wire formatında natural alignment kullanılıyor
// (percentage sonrası 7 byte padding, usage 8-byte aligned) → 16 byte.
typedef struct Pcs_monitor_type
{
    uint8_t  percentage;                                        // Byte 0          | 1 byte
    /* 7 byte padding (doğal 8-byte hizalama) */                //                 | 7 byte pad
    uint64_t usage;                                             // Byte 8   - 15   | 8 byte
} Pcs_monitor_type;                                             // TOPLAM: 16 byte

typedef struct Pcs_cpu_exec_time_type
{
    Pcs_monitor_type min_exec_time;                             // Byte 0   - 15   | 16 byte
    Pcs_monitor_type max_exec_time;                             // Byte 16  - 31   | 16 byte
    Pcs_monitor_type avg_exec_time;                             // Byte 32  - 47   | 16 byte
    Pcs_monitor_type last_exec_time;                            // Byte 48  - 63   | 16 byte
} Pcs_cpu_exec_time_type;                                       // TOPLAM: 64 byte

typedef struct Pcs_mem_profile_type
{
    uint64_t total_size;                                        // Byte 0   - 7    | 8 byte
    uint64_t used_size;                                         // Byte 8   - 15   | 8 byte
    uint64_t max_used_size;                                     // Byte 16  - 23   | 8 byte
} Pcs_mem_profile_type;                                         // TOPLAM: 24 byte

typedef struct Pcs_profile_stats
{
    uint64_t               sample_count;                        // Byte 0    - 7    | 8 byte
    uint64_t               latest_read_time;                    // Byte 8    - 15   | 8 byte
    uint64_t               total_run_time;                      // Byte 16   - 23   | 8 byte
    Pcs_cpu_exec_time_type cpu_exec_time;                       // Byte 24   - 87   | 64 byte
    Pcs_mem_profile_type   heap_mem;                            // Byte 88   - 111  | 24 byte
    Pcs_mem_profile_type   stack_mem;                           // Byte 112  - 135  | 24 byte
} Pcs_profile_stats;                                            // TOPLAM: 136 byte

#endif /* VMC_MESSAGE_TYPES_H */