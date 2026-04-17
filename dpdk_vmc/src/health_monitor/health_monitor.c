#include <health_monitor.h>
#include <vmc_message_types.h>
#include <stdio.h>

#define HEALTH_MONITOR_DASHBOARD_INTERVAL_MS 1000

struct rte_ring *g_health_ring = NULL;
struct rte_mempool *g_health_mempool = NULL;

// static pthread_t monitor_thread;
static volatile bool is_running = false;
static volatile bool *g_stop_flag = NULL;

// FLCS
static struct vmc_pbit_data_t flcs_vmc_pbit_response_data;
static struct bm_engineering_cbit_report_t flcs_bm_engineering_cbit_report;
static struct dtn_es_cbit_report_t flcs_dtn_es_cbit_report;
static struct dtn_sw_cbit_report_t flcs_dtn_sw_cbit_report;
// static struct Pcs_profile_stats flcs_pcs_profile_stats;

// VS
static struct vmc_pbit_data_t vs_vmc_pbit_response_data;
static struct bm_engineering_cbit_report_t vs_bm_flag_cbit_report;
static struct dtn_es_cbit_report_t vs_dtn_es_cbit_report;
static struct dtn_sw_cbit_report_t vs_dtn_sw_cbit_report;
// static struct Pcs_profile_stats vs_pcs_profile_stats;


// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

// 1. VMP PBIT REPORT

void print_vmc_pbit_report(const vmc_pbit_data_t *data, const char *device_name) 
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                            [%s] VMP PBIT REPORT                                       \n", prefix);
    printf("========================================================================================\n");
    
    printf("[ GENERAL INFO ]\n");
    printf(" LRU ID            : %u\n", data->lru_id);
    printf(" VMC Serial Number : %u\n", data->vmc_serial_number);
    printf(" FLCS CPU PBIT     : 0x%04X\n", data->flcs_cpu_pbit);
    printf(" VS CPU PBIT       : 0x%04X\n", data->vs_cpu_pbit);
    printf(" Msg Identifier    : 0x%02X | Length: %u bytes\n", data->header_st.message_identifier, data->header_st.message_len);
    
    printf("\n[ FIRMWARE & SOFTWARE VERSIONS ]\n");
    printf(" BM CD Firmware Version : %u.%u.%u\n", data->bm_cd_firmware_version_st.major, data->bm_cd_firmware_version_st.minor, data->bm_cd_firmware_version_st.bugfix);
    printf(" VMP CMSW Library Ver   : %u.%u.%u\n", data->vmp_cmsw_lib_ver.major, data->vmp_cmsw_lib_ver.minor, data->vmp_cmsw_lib_ver.bugfix);
    
    printf("\n[ STORAGE & COMPONENT STATUS ]\n");
    printf(" FLCS CPU Status        : %s\n", data->vmp_storage_and_status_st.flcs_cpu_status ? "OK" : "FAIL");
    printf(" VS CPU Status          : %s\n", data->vmp_storage_and_status_st.vs_cpu_status ? "OK" : "FAIL");
    printf(" eMMC Storage Status    : %s\n", data->vmp_storage_and_status_st.eMMC_storage_status ? "OK" : "FAIL");
    printf(" MRAM Storage Status    : %s\n", data->vmp_storage_and_status_st.MRAM_storage_status ? "OK" : "FAIL");

    printf("\n[ POLICY EXECUTION STATUS ]\n");
    printf(" Exec Status Flag       : %u\n", data->policy_steps_exec_status);
    printf(" Total Policy Steps     : %u\n", data->number_of_policy_step);
    
    if (data->number_of_policy_step > 0) {
        printf(" -----------------------------------------\n");
        printf(" %-15s | %-15s\n", "STEP COMMAND", "RETURN VALUE");
        printf(" -----------------------------------------\n");
        int steps = (data->number_of_policy_step > 80) ? 80 : data->number_of_policy_step;
        for (int i = 0; i < steps; i++) {
            printf(" CMD: 0x%02X        | RET: %d\n", data->list[i].policy_cmd, data->list[i].ret_val);
        }
    }
    printf("========================================================================================\n");
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

// 2. BM ENGINEERING / FLAG DATA CBIT REPORT

void print_bm_cbit_report(const bm_engineering_cbit_report_t *data, const char *report_title, const char *device_name) 
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                   [%s] %s                      \n", prefix, report_title);
    printf("========================================================================================\n");
    
    printf("[ GENERAL ]\n");
    printf(" LRU ID      : %u\n", data->lru_id);
    printf(" Comm Status : %u\n", data->comm_status);

    printf("\n[ VS CPU STATUS DATA ]\n");
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Current", data->vs_status_st.VSCPU_12V_current, "Core Imon", data->vs_status_st.VSCPU_core_imon);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "3v3 Rail Input Curr", data->vs_status_st.VSCPU_3v3_rail_input_current, "G1VDD Input Curr", data->vs_status_st.VSCPU_G1VDD_input_current);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Main Voltage", data->vs_status_st.VSCPU_12V_main_voltage, "Core Local Temp", data->vs_status_st.VSCPU_core_local_temperature);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "RAM Temp", data->vs_status_st.VSCPU_RAM_temperature, "FLASH Temp", data->vs_status_st.VSCPU_FLASH_temperature);
    
    printf("\n[ FLCS CPU STATUS DATA ]\n");
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Current", data->flcs_status_st.FCCPU_12V_current, "Core Imon", data->flcs_status_st.FCCPU_core_imon);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Main Voltage", data->flcs_status_st.FCCPU_12V_main_voltage, "Core Local Temp", data->flcs_status_st.FCCPU_core_local_temperature);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "RAM Temp", data->flcs_status_st.FCCPU_RAM_temperature, "FLASH Temp", data->flcs_status_st.FCCPU_FLASH_temperature);

    printf("\n[ DTN ES STATUS DATA ]\n");
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "VDD Input Current", data->dtn_es_status_st.DTN_ES_VDD_input_current, "12V Main Current", data->dtn_es_status_st.DTN_ES_12V_main_current);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Main Voltage", data->dtn_es_status_st.DTN_ES_12V_main_voltage, "1v8 Rail Voltage", data->dtn_es_status_st.DTN_ES_1v8_rail_voltage);

    printf("\n[ DTN VSW STATUS DATA ]\n");
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "VDD Input Current", data->vs_dtn_sw_status_st.DTN_VSW_VDD_input_current, "12V Main Current", data->vs_dtn_sw_status_st.DTN_VSW_12V_main_current);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "12V Main Voltage", data->vs_dtn_sw_status_st.DTN_VSW_12V_main_voltage, "FO V RX Imon R", data->vs_dtn_sw_status_st.DTN_VSW_FO_V_RX_imon_r);

    printf("\n[ VMC BOARD STATUS DATA ]\n");
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "PSM PRI VOLS", data->vmc_board_status_st.PSM_PWR_PRI_VOLS, "PSM SEC VOLS", data->vmc_board_status_st.PSM_PWR_SEC_VOLS);
    printf(" %-30s: %8.4f | %-30s: %8.4f\n", "BM FPGA Temp", data->vmc_board_status_st.BM_FPGA_temperature, "Board Edge Temp", data->vmc_board_status_st.Board_edge_temperature);
    printf("========================================================================================\n");
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

// DTN ES CBIT MESSAGES

void print_dtn_es_cbit_report(const dtn_es_cbit_report_t *data, const char *device_name) 
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                           [%s] DTN ES CBIT REPORT                                      \n", prefix);
    printf("========================================================================================\n");
    
    printf("[ GENERAL INFO ]\n");
    printf(" LRU ID       : %u                  | Network Type : %u\n", data->lru_id, data->network_type);
    printf(" Side Type    : %u                  | Comm Status  : %u\n", data->side_type, data->comm_status);
    
    const dtn_es_monitoring_t *es = &data->dtn_es_monitoring_st;
    printf("\n[ DEVICE & PTP STATUS ]\n");
    printf(" ES FW Ver    : %u.%u.%u              | HW Temp      : %u\n", es->A664_ES_FW_VER.major, es->A664_ES_FW_VER.minor, es->A664_ES_FW_VER.bugfix, es->A664_ES_HW_TEMP);
    printf(" Device ID    : 0x%016lX | Transc Temp  : %lu\n", es->A664_ES_DEV_ID, es->A664_ES_TRANSCEIVER_TEMP);
    printf(" ES Mode      : %lu                  | Port A Sync  : %u\n", es->A664_ES_MODE, es->A664_PTP_PORT_A_SYNC);
    printf(" PTP RC Stat  : %u                  | Port B Sync  : %u\n", es->A664_PTP_RC_STATUS, es->A664_PTP_PORT_B_SYNC);

    printf("\n[ TRAFFIC METRICS ]\n");
    printf(" %-40s | %-40s\n", "TX COUNTERS", "RX COUNTERS");
    printf("------------------------------------------|---------------------------------------------\n");
    printf(" TX Incoming  : %-23lu | RX Incoming A : %lu\n", es->A664_ES_TX_INCOMING_COUNT, es->A664_ES_RX_A_INCOMING_COUNT);
    printf(" TX Outgoing A: %-23lu | RX Incoming B : %lu\n", es->A664_ES_TX_A_OUTGOING_COUNT, es->A664_ES_RX_B_INCOMING_COUNT);
    printf(" TX Outgoing B: %-23lu | RX Outgoing   : %lu\n", es->A664_ES_TX_B_OUTGOING_COUNT, es->A664_ES_RX_OUTGOING_COUNT);
    printf(" TX VLID Drop : %-23lu | RX VLID Drop A: %lu\n", es->A664_ES_TX_VLID_DROP_COUNT, es->A664_ES_RX_A_VLID_DROP_COUNT);
    printf(" TX Max Jitter: %-23lu | RX CRC Err A  : %lu\n", es->A664_ES_TX_MAX_JITTER_DROP_COUNT, es->A664_ES_RX_A_CRC_ERROR_COUNT);
    printf("========================================================================================\n");
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

// DTN SW CBIT MESSAGES

void print_dtn_sw_cbit_report(const dtn_sw_cbit_report_t *data, const char *device_name) 
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                            [%s] DTN SW CBIT REPORT                                      \n", prefix);
    printf("========================================================================================\n");
    
    printf("[ SWITCH GENERAL INFO ]\n");
    printf(" LRU ID       : %u                  | Network Type : %u\n", data->lru_id, data->network_type);
    
    const dtn_sw_status_mon_t *sw = &data->dtn_sw_monitoring_st.status;
    printf("\n[ SWITCH STATUS ]\n");
    printf(" Device ID    : 0x%04X              | FW Version   : 0x%016lX\n", sw->A664_SW_DEV_ID, sw->A664_SW_FW_VER);
    printf(" TX Total Cnt : %-18lu | RX Total Cnt : %lu\n", sw->A664_SW_TX_TOTAL_COUNT, sw->A664_SW_RX_TOTAL_COUNT);
    printf(" Transc Temp  : %-18lu | Voltage      : %u\n", sw->A664_SW_TRANSCEIVER_TEMP, sw->A664_SW_VOLTAGE);
    printf(" Port Count   : %-18u | Mode         : %u\n", sw->A664_SW_PORT_COUNT, sw->A664_SW_MODE);

    printf("\n[ PORT STATUS OVERVIEW ]\n");
    printf(" %-4s | %-6s | %-8s | %-12s | %-12s | %-12s | %-12s\n", 
           "PORT", "STATUS", "SPEED", "TX COUNT", "RX COUNT", "CRC ERR", "VLID DROP");
    printf("------|--------|----------|--------------|--------------|--------------|--------------\n");

    for (int i = 0; i < 8; i++) {
        const dtn_sw_port_mon_t *p = &data->dtn_sw_monitoring_st.port[i];
        printf(" #%-3u | %-6u | %-8lu | %-12lu | %-12lu | %-12lu | %-12lu\n",
               p->A664_SW_PORT_ID,
               p->A664_SW_PORT_i_STATUS,
               p->A664_SW_PORT_i_SPEED,
               p->A664_SW_PORT_i_TX_COUNT,
               p->A664_SW_PORT_i_RX_COUNT,
               p->A664_SW_PORT_i_CRC_ERR_COUNT,
               p->A664_SW_PORT_i_VLID_DROP_COUNT);
    }
    printf("========================================================================================\n");
}

static void *health_monitor_thread_function (void *arg)
{
    (void)arg;
}

int init_health_monitor(void) 
{

}