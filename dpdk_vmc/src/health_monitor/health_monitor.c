#include <health_monitor.h>
#include <vmc_message_types.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

// ============================================================================
// Slot altyapısı
// ----------------------------------------------------------------------------
// rx_worker fast-path (her port için ayrı lcore) hm_handle_packet() içinden
// slot'a memcpy yapar; printer thread 1 Hz'de slot'u okur ve print eder.
// 200 pps toplam yük için mutex yeterli — spinlock'a gerek yok.
// ============================================================================

typedef struct {
    Pcs_profile_stats data;
    pthread_mutex_t   lock;
    bool              updated;
    uint64_t          update_count;
} hm_pcs_slot_t;

typedef struct {
    vmc_pbit_data_t   data;
    pthread_mutex_t   lock;
    bool              updated;
    uint64_t          update_count;
} hm_pbit_slot_t;

typedef struct {
    bm_engineering_cbit_report_t data;
    pthread_mutex_t              lock;
    bool                         updated;
    uint64_t                     update_count;
} hm_cbit_slot_t;

// VS tarafı
static hm_pcs_slot_t  vs_cpu_usage_slot    = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_pbit_slot_t vs_pbit_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_cbit_slot_t vs_cbit_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};

// FLCS tarafı (şu an handler yok ama slot yeri hazır)
static hm_pcs_slot_t  flcs_cpu_usage_slot  = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_pbit_slot_t flcs_pbit_slot       = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_cbit_slot_t flcs_cbit_slot       = {.lock = PTHREAD_MUTEX_INITIALIZER};

// Printer thread durumu
static pthread_t        g_printer_thread;
static volatile bool   *g_stop_flag = NULL;
static volatile bool    g_printer_running = false;

// ============================================================================
// BE→host endian yardımcıları
// ============================================================================
static inline uint64_t be64_to_host(uint64_t v) { return __builtin_bswap64(v); }
static inline uint32_t be32_to_host(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t be16_to_host(uint16_t v) { return __builtin_bswap16(v); }

// Pcs_monitor_type içindeki tek uint64 alan
static inline void swap_monitor(Pcs_monitor_type *m)
{
    m->usage = be64_to_host(m->usage);
}

// Pcs_mem_profile_type içindeki 3 uint64 alan
static inline void swap_mem(Pcs_mem_profile_type *m)
{
    m->total_size    = be64_to_host(m->total_size);
    m->used_size     = be64_to_host(m->used_size);
    m->max_used_size = be64_to_host(m->max_used_size);
}

// ============================================================================
// VS CPU USAGE parse
// ============================================================================
static void parse_pcs_profile_stats(Pcs_profile_stats *dst, const uint8_t *payload)
{
    memcpy(dst, payload, sizeof(*dst));

    dst->sample_count     = be64_to_host(dst->sample_count);
    dst->latest_read_time = be64_to_host(dst->latest_read_time);
    dst->total_run_time   = be64_to_host(dst->total_run_time);

    swap_monitor(&dst->cpu_exec_time.min_exec_time);
    swap_monitor(&dst->cpu_exec_time.max_exec_time);
    swap_monitor(&dst->cpu_exec_time.avg_exec_time);
    swap_monitor(&dst->cpu_exec_time.last_exec_time);

    swap_mem(&dst->heap_mem);
    swap_mem(&dst->stack_mem);
}

// ============================================================================
// RX fast-path entry point
// ============================================================================
void hm_handle_packet(uint16_t vl_id, const uint8_t *payload, uint16_t len)
{
    if (payload == NULL) return;

    switch (vl_id)
    {
        case HEALTH_MONITOR_VS_CPU_USAGE_VLID:
            if (len < sizeof(Pcs_profile_stats)) return;
            pthread_mutex_lock(&vs_cpu_usage_slot.lock);
            parse_pcs_profile_stats(&vs_cpu_usage_slot.data, payload);
            vs_cpu_usage_slot.updated = true;
            vs_cpu_usage_slot.update_count++;
            pthread_mutex_unlock(&vs_cpu_usage_slot.lock);
            break;

        case HEALTH_MONITOR_FLCS_CPU_USAGE_VLID:
            // Handler ileride eklenecek; şimdilik sadece sayaç artır.
            if (len < sizeof(Pcs_profile_stats)) return;
            pthread_mutex_lock(&flcs_cpu_usage_slot.lock);
            parse_pcs_profile_stats(&flcs_cpu_usage_slot.data, payload);
            flcs_cpu_usage_slot.updated = true;
            flcs_cpu_usage_slot.update_count++;
            pthread_mutex_unlock(&flcs_cpu_usage_slot.lock);
            break;

        // TODO: diğer HM VLID'leri (PBIT_RESPONSE, CBIT) aynı desene göre eklenecek
        default:
            break;
    }
}

// ============================================================================
// Printer thread — 1 Hz dashboard
// ============================================================================
static bool drain_and_print_pcs_slot(hm_pcs_slot_t *slot, const char *device_name)
{
    bool printed = false;
    Pcs_profile_stats local;

    pthread_mutex_lock(&slot->lock);
    if (slot->updated) {
        local = slot->data;
        slot->updated = false;
        printed = true;
    }
    pthread_mutex_unlock(&slot->lock);

    if (printed) {
        print_pcs_profile_stats(&local, device_name);
    }
    return printed;
}

static void *hm_printer_thread_function(void *arg)
{
    (void)arg;
    while (g_stop_flag == NULL || !(*g_stop_flag))
    {
        bool any = false;
        any |= drain_and_print_pcs_slot(&vs_cpu_usage_slot,   "VS");
        any |= drain_and_print_pcs_slot(&flcs_cpu_usage_slot, "FLCS");
        // Diğer slot drain'leri (PBIT, CBIT) buraya eklenecek.

        (void)any;
        usleep(HEALTH_MONITOR_DASHBOARD_INTERVAL_MS * 1000u);
    }
    return NULL;
}

int hm_start_printer_thread(volatile bool *stop_flag)
{
    if (g_printer_running) return 0;
    g_stop_flag = stop_flag;
    int ret = pthread_create(&g_printer_thread, NULL, hm_printer_thread_function, NULL);
    if (ret != 0) {
        fprintf(stderr, "hm_start_printer_thread: pthread_create failed: %s\n", strerror(ret));
        return -1;
    }
    g_printer_running = true;
    printf("[HM] Health monitor printer thread started (interval=%d ms)\n",
           HEALTH_MONITOR_DASHBOARD_INTERVAL_MS);
    return 0;
}

void hm_stop_printer_thread(void)
{
    if (!g_printer_running) return;
    pthread_join(g_printer_thread, NULL);
    g_printer_running = false;
    printf("[HM] Health monitor printer thread stopped\n");
}

// ============================================================================
// Print fonksiyonları
// ============================================================================

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

// 3. DTN ES CBIT
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

// 4. DTN SW CBIT
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

// 5. CPU USAGE (Pcs_profile_stats)
void print_pcs_profile_stats(const Pcs_profile_stats *data, const char *device_name)
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                             [%s] CPU USAGE / PROFILE STATS                            \n", prefix);
    printf("========================================================================================\n");

    printf("[ GENERAL ]\n");
    printf(" Sample Count     : %lu\n", (unsigned long)data->sample_count);
    printf(" Latest Read Time : %lu\n", (unsigned long)data->latest_read_time);
    printf(" Total Run Time   : %lu\n", (unsigned long)data->total_run_time);

    const Pcs_cpu_exec_time_type *ct = &data->cpu_exec_time;
    printf("\n[ CPU EXEC TIME ]\n");
    printf(" %-10s | %-12s | %-18s\n", "KIND", "PERCENTAGE", "USAGE");
    printf("------------|--------------|--------------------\n");
    printf(" %-10s | %-10u %% | %lu\n", "min",  ct->min_exec_time.percentage,  (unsigned long)ct->min_exec_time.usage);
    printf(" %-10s | %-10u %% | %lu\n", "max",  ct->max_exec_time.percentage,  (unsigned long)ct->max_exec_time.usage);
    printf(" %-10s | %-10u %% | %lu\n", "avg",  ct->avg_exec_time.percentage,  (unsigned long)ct->avg_exec_time.usage);
    printf(" %-10s | %-10u %% | %lu\n", "last", ct->last_exec_time.percentage, (unsigned long)ct->last_exec_time.usage);

    printf("\n[ MEMORY PROFILE ]\n");
    printf(" %-10s | %-14s | %-14s | %-14s\n", "REGION", "TOTAL", "USED", "MAX USED");
    printf("------------|----------------|----------------|----------------\n");
    printf(" %-10s | %-14lu | %-14lu | %-14lu\n", "heap",
           (unsigned long)data->heap_mem.total_size,
           (unsigned long)data->heap_mem.used_size,
           (unsigned long)data->heap_mem.max_used_size);
    printf(" %-10s | %-14lu | %-14lu | %-14lu\n", "stack",
           (unsigned long)data->stack_mem.total_size,
           (unsigned long)data->stack_mem.used_size,
           (unsigned long)data->stack_mem.max_used_size);
    printf("========================================================================================\n");
}
