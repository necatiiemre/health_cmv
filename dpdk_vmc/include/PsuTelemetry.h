/**
 * @file PsuTelemetry.h
 * @brief Wire format for 1 Hz PSU measurements pushed from MainSoftware (PC)
 *        to DPDK (server) over UDP.
 *
 * This header is duplicated verbatim at dpdk/include/PsuTelemetry.h.
 * Both copies MUST stay in sync - they describe the exact same bytes on the
 * wire. If you add a field, bump PSU_TELEM_VERSION so receivers can reject
 * mismatched peers cleanly.
 *
 * Transport:
 *   - Direction: MainSoftware -> DPDK (push, 1 Hz)
 *   - Protocol : UDP
 *   - Port     : 20050 on the server's IP
 *   - Packet   : exactly sizeof(psu_telem_pkt_t) bytes, host byte order
 *                (both peers are x86_64 Linux on the same LAN).
 */
#ifndef PSU_TELEMETRY_H
#define PSU_TELEMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSU_TELEM_MAGIC    0x50535530u   /* 'P','S','U','0' */
#define PSU_TELEM_VERSION  1
#define PSU_TELEM_PORT     20050

/* flags bitfield */
#define PSU_TELEM_FLAG_OUTPUT_ON    (1u << 0)  /* PSU output relay is on */
#define PSU_TELEM_FLAG_PSU_ERROR    (1u << 1)  /* SCPI query failed / timeout */
#define PSU_TELEM_FLAG_RECONNECTED  (1u << 2)  /* reconnect happened this tick */

/* model field */
#define PSU_TELEM_MODEL_PSU30   0
#define PSU_TELEM_MODEL_PSU300  1

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* PSU_TELEM_MAGIC */
    uint16_t version;        /* PSU_TELEM_VERSION */
    uint16_t flags;          /* PSU_TELEM_FLAG_* */
    uint64_t ts_unix_ns;     /* publisher's wall-clock at sample time */
    uint32_t seq;            /* monotonic, for stale / ordering detection */
    float    voltage_v;      /* MEAS:VOLT? */
    float    current_a;      /* MEAS:CURR? */
    float    power_w;        /* voltage_v * current_a */
    float    set_voltage_v;  /* VOLT? (setpoint) */
    float    set_current_a;  /* CURR? (setpoint) */
    uint8_t  model;          /* PSU_TELEM_MODEL_* */
    uint8_t  reserved[3];    /* must be 0 on the wire */
} psu_telem_pkt_t;
#pragma pack(pop)

/* Compile-time size guard. If this assert fires, the struct layout has
 * drifted between the two copies or padding snuck in. Fix immediately.
 * Current layout: 4+2+2+8+4+4+4+4+4+4+1+3 = 44 bytes. */
#define PSU_TELEM_PKT_SIZE 44
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(psu_telem_pkt_t) == PSU_TELEM_PKT_SIZE,
               "psu_telem_pkt_t size drift - wire format broken");
#elif defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(psu_telem_pkt_t) == PSU_TELEM_PKT_SIZE,
              "psu_telem_pkt_t size drift - wire format broken");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSU_TELEMETRY_H */
