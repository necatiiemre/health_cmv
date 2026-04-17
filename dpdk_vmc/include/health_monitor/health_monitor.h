#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <vmc_message_types.h>

#define HEALTH_MONITOR_INTERFACE "eno12409"

// ============================================================================
// VL-ID tanımları
// ============================================================================
#define HEALTH_MONITOR_FLCS_CPU_USAGE_VLID      0x0009
#define HEALTH_MONITOR_VS_CPU_USAGE_VLID        0x0010

#define HEALTH_MONITOR_FLCS_PBIT_REQUEST_VLID   0x000c
#define HEALTH_MONITOR_VS_PBIT_REQUEST_VLID     0x000f

#define HEALTH_MONITOR_FLCS_PBIT_RESPONSE_VLID  0x000a
#define HEALTH_MONITOR_VS_PBIT_RESPONSE_VLID    0x000d

#define HEALTH_MONITOR_FLCS_CBIT_VLID           0x000b
#define HEALTH_MONITOR_VS_CBIT_VLID             0x000e

// HM VL-ID aralığı (rx_worker erken-dallanmasında aralık kontrolü için).
// Bu aralığa düşen paketler PRBS doğrulamasına girmez.
#define HM_VL_ID_MIN 0x0009
#define HM_VL_ID_MAX 0x0010

static inline bool hm_is_health_monitor_vl_id(uint16_t vl_id)
{
    return (vl_id >= HM_VL_ID_MIN && vl_id <= HM_VL_ID_MAX);
}

// Dashboard basım aralığı (ms)
#define HEALTH_MONITOR_DASHBOARD_INTERVAL_MS 1000

// ============================================================================
// RX fast-path entry point: rx_worker içinden çağrılır.
// payload  = UDP payload başı
// len      = UDP payload uzunluğu (>= beklenen struct boyutu olmalı)
// ============================================================================
void hm_handle_packet(uint16_t vl_id, const uint8_t *payload, uint16_t len);

// ============================================================================
// Printer thread yaşam döngüsü (main.c'den çağrılır)
// ============================================================================
int  hm_start_printer_thread(volatile bool *stop_flag);
void hm_stop_printer_thread(void);

// ============================================================================
// Print fonksiyonları (printer thread ya da debug amaçlı dışarıdan çağrılır)
// ============================================================================
void print_vmc_pbit_report     (const vmc_pbit_data_t *data,           const char *device_name);
void print_bm_cbit_report      (const bm_engineering_cbit_report_t *data, const char *report_title, const char *device_name);
void print_dtn_es_cbit_report  (const dtn_es_cbit_report_t *data,      const char *device_name);
void print_dtn_sw_cbit_report  (const dtn_sw_cbit_report_t *data,      const char *device_name);
void print_pcs_profile_stats   (const Pcs_profile_stats *data,         const char *device_name);

#endif /* HEALTH_MONITOR_H */
