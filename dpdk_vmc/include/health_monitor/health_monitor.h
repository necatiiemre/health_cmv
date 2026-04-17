#include <vmc_message_types.h>

#define HEALTH_MONITOR_INTERFACE "eno12409"

#define HEALTH_MONITOR_FLCS_CPU_USAGE_VLID 0x0009
#define HEALTH_MONITOR_VS_CPU_USAGE_VLID 0x0010

#define HEALTH_MONITOR_FLCS_PBIT_REQUEST_VLID 0x000c
#define HEALTH_MONITOR_VS_PBIT_REQUEST_VLID 0x000f

#define HEALTH_MONITOR_FLCS_PBIT_RESPONSE_VLID 0x000a 
#define HEALTH_MONITOR_VS_PBIT_RESPONSE_VLID 0x000d

#define HEALTH_MONITOR_FLCS_CBIT_VLID 0x000b
#define HEALTH_MONITOR_VS_CBIT_VLID 0x000e



void print_vmc_pbit_report(const vmc_pbit_data_t *data);
void print_bm_cbit_report(const bm_engineering_cbit_report_t *data, const char *report_title);
void print_dtn_es_cbit_report(const dtn_es_cbit_report_t *data);
void print_dtn_sw_cbit_report(const dtn_sw_cbit_report_t *data);