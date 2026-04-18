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
} hm_bm_slot_t;

typedef struct {
    bm_flag_cbit_report_t data;
    pthread_mutex_t       lock;
    bool                  updated;
    uint64_t              update_count;
} hm_bm_flag_slot_t;

typedef struct {
    dtn_es_cbit_report_t data;
    pthread_mutex_t      lock;
    bool                 updated;
    uint64_t             update_count;
} hm_dtn_es_slot_t;

typedef struct {
    dtn_sw_cbit_report_t data;
    pthread_mutex_t      lock;
    bool                 updated;
    uint64_t             update_count;
} hm_dtn_sw_slot_t;

// VS tarafı — CPU USAGE + PBIT + 4 CBIT türü
static hm_pcs_slot_t      vs_cpu_usage_slot      = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_pbit_slot_t     vs_pbit_slot           = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_bm_slot_t       vs_bm_engineering_slot = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_bm_flag_slot_t  vs_bm_flag_slot        = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_dtn_es_slot_t   vs_dtn_es_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_dtn_sw_slot_t   vs_dtn_sw_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};

// FLCS tarafı — ayna
static hm_pcs_slot_t      flcs_cpu_usage_slot      = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_pbit_slot_t     flcs_pbit_slot           = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_bm_slot_t       flcs_bm_engineering_slot = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_bm_flag_slot_t  flcs_bm_flag_slot        = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_dtn_es_slot_t   flcs_dtn_es_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};
static hm_dtn_sw_slot_t   flcs_dtn_sw_slot         = {.lock = PTHREAD_MUTEX_INITIALIZER};

// Printer thread durumu
static pthread_t        g_printer_thread;
static volatile bool   *g_stop_flag = NULL;
static volatile bool    g_printer_running = false;

// Debug/diagnostic sayaçları (atomic artırılır, lock gerekmez)
static volatile uint64_t g_hm_rx_total        = 0;  // hm_handle_packet çağrı sayısı
static volatile uint64_t g_hm_rx_vs_cpu       = 0;
static volatile uint64_t g_hm_rx_flcs_cpu     = 0;
static volatile uint64_t g_hm_rx_cbit         = 0;  // VL-ID 11/14 toplam
static volatile uint64_t g_hm_rx_unknown_vlid = 0;
static volatile uint64_t g_hm_rx_unknown_msg  = 0;  // CBIT içinde bilinmeyen msg_id
static volatile uint64_t g_hm_rx_short        = 0;
static volatile uint64_t g_hm_rx_empty        = 0;  // boş CBIT paketleri (tx/rx total = 0)

// CBIT paketlerinde "boşluk" kontrolü — traffic counter'ları tamamen sıfırsa
// paket VMC tarafının henüz veri toplamadığı bir döngüde gönderildiğini
// gösterir. Bu paketlerle slot'u güncellemiyoruz; dolu paketleri
// ezmemeleri için updated bayrağı set edilmez.
static inline bool dtn_sw_is_empty(const dtn_sw_cbit_report_t *d)
{
    const dtn_sw_status_mon_t *s = &d->dtn_sw_monitoring_st.status;
    return (s->A664_SW_TX_TOTAL_COUNT == 0 && s->A664_SW_RX_TOTAL_COUNT == 0);
}

static inline bool dtn_es_is_empty(const dtn_es_cbit_report_t *d)
{
    const dtn_es_monitoring_t *m = &d->dtn_es_monitoring_st;
    return (m->A664_ES_TX_INCOMING_COUNT   == 0 &&
            m->A664_ES_RX_A_INCOMING_COUNT == 0 &&
            m->A664_ES_RX_B_INCOMING_COUNT == 0);
}

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

// Float dizisini yerinde BE→host swap et (IEEE-754 4-byte değerler).
static inline void bswap_float_array(float *arr, size_t count)
{
    uint32_t *u = (uint32_t *)arr;
    for (size_t i = 0; i < count; i++) {
        u[i] = __builtin_bswap32(u[i]);
    }
}

// vmp_cmsw_header_t alanlarını (msg_len, timestamp) swap et.
static inline void swap_vmp_header(vmp_cmsw_header_t *h)
{
    h->message_len = be16_to_host(h->message_len);
    h->timestamp   = be64_to_host(h->timestamp);
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

// BM Engineering raporunu parse eder. Struct 397 B'lik wire formatıyla
// BIRE BIR uyumlu (kanıtlandı). Status blokları yalnızca float32 alanlardan
// oluşuyor; header sonrası 384 byte = 96 adet float kesintisiz sıralı.
static void parse_bm_engineering(bm_engineering_cbit_report_t *dst, const uint8_t *payload)
{
    memcpy(dst, payload, sizeof(*dst));

    swap_vmp_header(&dst->header_st);

    // Float region start: vmp_cmsw_header_t(11) + lru_id(1) + comm_status(1) = 13
    // 96 float × 4 byte = 384 byte. Packed struct içinde unaligned erişim
    // olmasın diye memcpy ile okuyup yazıyoruz.
    uint8_t *bytes = (uint8_t *)dst + 13;
    for (size_t i = 0; i < 96; i++) {
        uint32_t v;
        memcpy(&v, bytes + i * 4, sizeof(v));
        v = __builtin_bswap32(v);
        memcpy(bytes + i * 4, &v, sizeof(v));
    }
    (void)bswap_float_array; // helper leri ilerideki yeni parse'larda kullanacağız
}

// BM FLAG (105 B) parse — header + lru + 93 B opaque payload.
static void parse_bm_flag(bm_flag_cbit_report_t *dst, const uint8_t *payload)
{
    memcpy(dst, payload, sizeof(*dst));
    swap_vmp_header(&dst->header_st);
}

// DTN ES parse — wire 352 B (HW_VCC_INT çıkarıldı).
// A664_ES_FW_VER alanı bitfield (40-bit reserved + 3 u8), swap gerekmez.
// Kalan uint16/uint32/uint64 alanları BE→host çevrilir.
static void parse_dtn_es(dtn_es_cbit_report_t *dst, const uint8_t *payload)
{
    memcpy(dst, payload, sizeof(*dst));
    swap_vmp_header(&dst->header_st);

    dtn_es_monitoring_t *m = &dst->dtn_es_monitoring_st;

    // A664_ES_FW_VER: bitfield (40 reserved + major/minor/bugfix) — swap yok

    // uint64 ve uint32 alanları tek tek swap
    m->A664_ES_DEV_ID        = be64_to_host(m->A664_ES_DEV_ID);
    m->A664_ES_MODE          = be64_to_host(m->A664_ES_MODE);
    m->A664_ES_CONFIG_ID     = be64_to_host(m->A664_ES_CONFIG_ID);
    m->A664_ES_BIT_STATUS    = be64_to_host(m->A664_ES_BIT_STATUS);
    m->A664_ES_CONFIG_STATUS = be64_to_host(m->A664_ES_CONFIG_STATUS);

    m->A664_PTP_CONFIG_ID  = be16_to_host(m->A664_PTP_CONFIG_ID);
    m->A664_PTP_SYNC_VL_ID = be16_to_host(m->A664_PTP_SYNC_VL_ID);
    m->A664_PTP_REQ_VL_ID  = be16_to_host(m->A664_PTP_REQ_VL_ID);
    m->A664_PTP_RES_VL_ID  = be16_to_host(m->A664_PTP_RES_VL_ID);

    // HW_TEMP, HW_VCC_INT, TRANSCEIVER_TEMP hepsi float32 BE — bytes-swap edip yorum değişmiyor
    {
        uint32_t v;
        memcpy(&v, &m->A664_ES_HW_TEMP, 4);           v = __builtin_bswap32(v); memcpy(&m->A664_ES_HW_TEMP,          &v, 4);
        memcpy(&v, &m->A664_ES_HW_VCC_INT, 4);        v = __builtin_bswap32(v); memcpy(&m->A664_ES_HW_VCC_INT,       &v, 4);
        memcpy(&v, &m->A664_ES_TRANSCEIVER_TEMP, 4);  v = __builtin_bswap32(v); memcpy(&m->A664_ES_TRANSCEIVER_TEMP, &v, 4);
    }
    m->A664_ES_PORT_A_STATUS                      = be64_to_host(m->A664_ES_PORT_A_STATUS);
    m->A664_ES_PORT_B_STATUS                      = be64_to_host(m->A664_ES_PORT_B_STATUS);
    m->A664_ES_TX_INCOMING_COUNT                  = be64_to_host(m->A664_ES_TX_INCOMING_COUNT);
    m->A664_ES_TX_A_OUTGOING_COUNT                = be64_to_host(m->A664_ES_TX_A_OUTGOING_COUNT);
    m->A664_ES_TX_B_OUTGOING_COUNT                = be64_to_host(m->A664_ES_TX_B_OUTGOING_COUNT);
    m->A664_ES_TX_VLID_DROP_COUNT                 = be64_to_host(m->A664_ES_TX_VLID_DROP_COUNT);
    m->A664_ES_TX_LMIN_LMAX_DROP_COUNT            = be64_to_host(m->A664_ES_TX_LMIN_LMAX_DROP_COUNT);
    m->A664_ES_TX_MAX_JITTER_DROP_COUNT           = be64_to_host(m->A664_ES_TX_MAX_JITTER_DROP_COUNT);
    m->A664_ES_RX_A_INCOMING_COUNT                = be64_to_host(m->A664_ES_RX_A_INCOMING_COUNT);
    m->A664_ES_RX_B_INCOMING_COUNT                = be64_to_host(m->A664_ES_RX_B_INCOMING_COUNT);
    m->A664_ES_RX_OUTGOING_COUNT                  = be64_to_host(m->A664_ES_RX_OUTGOING_COUNT);
    m->A664_ES_RX_A_VLID_DROP_COUNT               = be64_to_host(m->A664_ES_RX_A_VLID_DROP_COUNT);
    m->A664_ES_RX_A_LMIN_LMAX_DROP_COUNT          = be64_to_host(m->A664_ES_RX_A_LMIN_LMAX_DROP_COUNT);
    m->A664_ES_RX_A_NET_ERR_COUNT                 = be64_to_host(m->A664_ES_RX_A_NET_ERR_COUNT);
    m->A664_ES_RX_A_SEQ_ERR_COUNT                 = be64_to_host(m->A664_ES_RX_A_SEQ_ERR_COUNT);
    m->A664_ES_RX_A_CRC_ERROR_COUNT               = be64_to_host(m->A664_ES_RX_A_CRC_ERROR_COUNT);
    m->A664_ES_RX_A_IP_CHECKSUM_ERROR_COUNT       = be64_to_host(m->A664_ES_RX_A_IP_CHECKSUM_ERROR_COUNT);
    m->A664_ES_RX_B_VLID_DROP_COUNT               = be64_to_host(m->A664_ES_RX_B_VLID_DROP_COUNT);
    m->A664_ES_RX_B_LMIN_LMAX_DROP_COUNT          = be64_to_host(m->A664_ES_RX_B_LMIN_LMAX_DROP_COUNT);
    m->A664_ES_RX_B_SEQ_ERR_COUNT                 = be64_to_host(m->A664_ES_RX_B_SEQ_ERR_COUNT);
    m->A664_ES_RX_B_NET_ERR_COUNT                 = be64_to_host(m->A664_ES_RX_B_NET_ERR_COUNT);
    m->A664_ES_RX_B_CRC_ERROR_COUNT               = be64_to_host(m->A664_ES_RX_B_CRC_ERROR_COUNT);
    m->A664_ES_RX_B_IP_CHECKSUM_ERROR_COUNT       = be64_to_host(m->A664_ES_RX_B_IP_CHECKSUM_ERROR_COUNT);
    m->A664_BSP_TX_PACKET_COUNT                   = be64_to_host(m->A664_BSP_TX_PACKET_COUNT);
    m->A664_BSP_TX_BYTE_COUNT                     = be64_to_host(m->A664_BSP_TX_BYTE_COUNT);
    m->A664_BSP_TX_ERROR_COUNT                    = be64_to_host(m->A664_BSP_TX_ERROR_COUNT);
    m->A664_BSP_RX_PACKET_COUNT                   = be64_to_host(m->A664_BSP_RX_PACKET_COUNT);
    m->A664_BSP_RX_BYTE_COUNT                     = be64_to_host(m->A664_BSP_RX_BYTE_COUNT);
    m->A664_BSP_RX_ERROR_COUNT                    = be64_to_host(m->A664_BSP_RX_ERROR_COUNT);
    m->A664_BSP_RX_MISSED_FRAME_COUNT             = be64_to_host(m->A664_BSP_RX_MISSED_FRAME_COUNT);
    m->A664_BSP_VER                               = be64_to_host(m->A664_BSP_VER);
    m->A664_ES_VENDOR_TYPE                        = be64_to_host(m->A664_ES_VENDOR_TYPE);
    m->A664_ES_BSP_QUEUING_RX_VL_PORT_DROP_COUNT  = be64_to_host(m->A664_ES_BSP_QUEUING_RX_VL_PORT_DROP_COUNT);
}

// DTN SW parse — wire 1064 B (57 B status + 8×124 B port).
static void parse_dtn_sw(dtn_sw_cbit_report_t *dst, const uint8_t *payload)
{
    memcpy(dst, payload, sizeof(*dst));
    swap_vmp_header(&dst->header_st);

    // Status
    dtn_sw_status_mon_t *s = &dst->dtn_sw_monitoring_st.status;
    s->A664_SW_TX_TOTAL_COUNT      = be64_to_host(s->A664_SW_TX_TOTAL_COUNT);
    s->A664_SW_RX_TOTAL_COUNT      = be64_to_host(s->A664_SW_RX_TOTAL_COUNT);
    // float BE → host (uint32 swap + reinterpret)
    {
        uint32_t v;
        memcpy(&v, &s->A664_SW_TRANSCEIVER_TEMP, 4);
        v = __builtin_bswap32(v);
        memcpy(&s->A664_SW_TRANSCEIVER_TEMP, &v, 4);

        memcpy(&v, &s->A664_SW_SHARED_TRANSCEIVER_TEMP, 4);
        v = __builtin_bswap32(v);
        memcpy(&s->A664_SW_SHARED_TRANSCEIVER_TEMP, &v, 4);

        memcpy(&v, &s->A664_SW_VOLTAGE, 4);
        v = __builtin_bswap32(v);
        memcpy(&s->A664_SW_VOLTAGE, &v, 4);

        memcpy(&v, &s->A664_SW_TEMPERATURE, 4);
        v = __builtin_bswap32(v);
        memcpy(&s->A664_SW_TEMPERATURE, &v, 4);
    }
    s->A664_SW_DEV_ID              = be16_to_host(s->A664_SW_DEV_ID);
    s->A664_SW_FW_VER              = be64_to_host(s->A664_SW_FW_VER);
    s->A664_SW_EMBEDEED_ES_FW_VER  = be64_to_host(s->A664_SW_EMBEDEED_ES_FW_VER);
    s->A664_SW_CONFIGURATION_ID    = be16_to_host(s->A664_SW_CONFIGURATION_ID);

    // Ports
    for (int i = 0; i < 8; i++) {
        dtn_sw_port_mon_t *p = &dst->dtn_sw_monitoring_st.port[i];
        p->A664_SW_PORT_ID = be16_to_host(p->A664_SW_PORT_ID);
        p->A664_SW_PORT_i_CRC_ERR_COUNT              = be64_to_host(p->A664_SW_PORT_i_CRC_ERR_COUNT);
        p->A664_SW_PORT_i_MIN_VL_FRAME_ERR_COUNT     = be64_to_host(p->A664_SW_PORT_i_MIN_VL_FRAME_ERR_COUNT);
        p->A664_SW_PORT_i_MAX_VL_FRAME_ERR_COUNT     = be64_to_host(p->A664_SW_PORT_i_MAX_VL_FRAME_ERR_COUNT);
        p->A664_SW_PORT_i_TRAFFIC_POLCY_DROP_COUNT   = be64_to_host(p->A664_SW_PORT_i_TRAFFIC_POLCY_DROP_COUNT);
        p->A664_SW_PORT_i_BE_COUNT                   = be64_to_host(p->A664_SW_PORT_i_BE_COUNT);
        p->A664_SW_PORT_i_TX_COUNT                   = be64_to_host(p->A664_SW_PORT_i_TX_COUNT);
        p->A664_SW_PORT_i_RX_COUNT                   = be64_to_host(p->A664_SW_PORT_i_RX_COUNT);
        p->A664_SW_PORT_i_VL_SOURCE_ERR_COUNT        = be64_to_host(p->A664_SW_PORT_i_VL_SOURCE_ERR_COUNT);
        p->A664_SW_PORT_i_MAX_DELAY_ERR_COUNT        = be64_to_host(p->A664_SW_PORT_i_MAX_DELAY_ERR_COUNT);
        p->A664_SW_PORT_i_VLID_DROP_COUNT            = be64_to_host(p->A664_SW_PORT_i_VLID_DROP_COUNT);
        p->A664_SW_PORT_i_UNDEF_MAC_COUNT            = be64_to_host(p->A664_SW_PORT_i_UNDEF_MAC_COUNT);
        p->A664_SW_PORT_i_HIGH_PRTY_QUE_OVRFLW_COUNT = be64_to_host(p->A664_SW_PORT_i_HIGH_PRTY_QUE_OVRFLW_COUNT);
        p->A664_SW_PORT_i_LOW_PRTY_QUE_OVRFLW_COUNT  = be64_to_host(p->A664_SW_PORT_i_LOW_PRTY_QUE_OVRFLW_COUNT);
        p->A664_SW_PORT_i_MAX_DELAY                  = be64_to_host(p->A664_SW_PORT_i_MAX_DELAY);
        p->A664_SW_PORT_i_SPEED                      = be64_to_host(p->A664_SW_PORT_i_SPEED);
    }
}

// ============================================================================
// RX fast-path entry point
// ============================================================================
void hm_handle_packet(uint16_t vl_id, const uint8_t *payload, uint16_t len)
{
    if (payload == NULL) return;

    __atomic_add_fetch(&g_hm_rx_total, 1, __ATOMIC_RELAXED);

    switch (vl_id)
    {
        case HEALTH_MONITOR_VS_CPU_USAGE_VLID:
            if (len < sizeof(Pcs_profile_stats)) {
                __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                return;
            }
            pthread_mutex_lock(&vs_cpu_usage_slot.lock);
            parse_pcs_profile_stats(&vs_cpu_usage_slot.data, payload);
            vs_cpu_usage_slot.updated = true;
            vs_cpu_usage_slot.update_count++;
            pthread_mutex_unlock(&vs_cpu_usage_slot.lock);
            __atomic_add_fetch(&g_hm_rx_vs_cpu, 1, __ATOMIC_RELAXED);
            break;

        case HEALTH_MONITOR_FLCS_CPU_USAGE_VLID:
            if (len < sizeof(Pcs_profile_stats)) {
                __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                return;
            }
            pthread_mutex_lock(&flcs_cpu_usage_slot.lock);
            parse_pcs_profile_stats(&flcs_cpu_usage_slot.data, payload);
            flcs_cpu_usage_slot.updated = true;
            flcs_cpu_usage_slot.update_count++;
            pthread_mutex_unlock(&flcs_cpu_usage_slot.lock);
            __atomic_add_fetch(&g_hm_rx_flcs_cpu, 1, __ATOMIC_RELAXED);
            break;

        case HEALTH_MONITOR_FLCS_CBIT_VLID:
        case HEALTH_MONITOR_VS_CBIT_VLID:
        {
            if (len < sizeof(vmp_cmsw_header_t)) {
                __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                return;
            }
            __atomic_add_fetch(&g_hm_rx_cbit, 1, __ATOMIC_RELAXED);

            const bool is_vs = (vl_id == HEALTH_MONITOR_VS_CBIT_VLID);
            const uint8_t msg_id = payload[0];

            switch (msg_id)
            {
                case HM_CBIT_MSG_ID_BM_ENGINEERING:
                {
                    if (len < sizeof(bm_engineering_cbit_report_t)) {
                        __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    hm_bm_slot_t *slot = is_vs ? &vs_bm_engineering_slot
                                               : &flcs_bm_engineering_slot;
                    pthread_mutex_lock(&slot->lock);
                    parse_bm_engineering(&slot->data, payload);
                    slot->updated = true;
                    slot->update_count++;
                    pthread_mutex_unlock(&slot->lock);
                    break;
                }

                case HM_CBIT_MSG_ID_BM_FLAG:
                {
                    if (len < sizeof(bm_flag_cbit_report_t)) {
                        __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    hm_bm_flag_slot_t *slot = is_vs ? &vs_bm_flag_slot : &flcs_bm_flag_slot;
                    pthread_mutex_lock(&slot->lock);
                    parse_bm_flag(&slot->data, payload);
                    slot->updated = true;
                    slot->update_count++;
                    pthread_mutex_unlock(&slot->lock);
                    break;
                }

                case HM_CBIT_MSG_ID_DTN_ES:
                {
                    if (len < sizeof(dtn_es_cbit_report_t)) {
                        __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    // Geçici tampona parse et; boşsa slot'u değiştirme.
                    dtn_es_cbit_report_t tmp;
                    parse_dtn_es(&tmp, payload);
                    if (dtn_es_is_empty(&tmp)) {
                        __atomic_add_fetch(&g_hm_rx_empty, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    hm_dtn_es_slot_t *slot = is_vs ? &vs_dtn_es_slot : &flcs_dtn_es_slot;
                    pthread_mutex_lock(&slot->lock);
                    slot->data = tmp;
                    slot->updated = true;
                    slot->update_count++;
                    pthread_mutex_unlock(&slot->lock);
                    break;
                }

                case HM_CBIT_MSG_ID_DTN_SW:
                {
                    if (len < sizeof(dtn_sw_cbit_report_t)) {
                        __atomic_add_fetch(&g_hm_rx_short, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    dtn_sw_cbit_report_t tmp;
                    parse_dtn_sw(&tmp, payload);
                    if (dtn_sw_is_empty(&tmp)) {
                        __atomic_add_fetch(&g_hm_rx_empty, 1, __ATOMIC_RELAXED);
                        break;
                    }
                    hm_dtn_sw_slot_t *slot = is_vs ? &vs_dtn_sw_slot : &flcs_dtn_sw_slot;
                    pthread_mutex_lock(&slot->lock);
                    slot->data = tmp;
                    slot->updated = true;
                    slot->update_count++;
                    pthread_mutex_unlock(&slot->lock);
                    break;
                }

                default:
                    __atomic_add_fetch(&g_hm_rx_unknown_msg, 1, __ATOMIC_RELAXED);
                    break;
            }
            break;
        }

        // TODO: PBIT_RESPONSE (VLID 0x0a / 0x0d) aynı desenle eklenecek
        default:
            __atomic_add_fetch(&g_hm_rx_unknown_vlid, 1, __ATOMIC_RELAXED);
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

static bool drain_and_print_bm_eng_slot(hm_bm_slot_t *slot, const char *device_name)
{
    bool printed = false;
    bm_engineering_cbit_report_t local;

    pthread_mutex_lock(&slot->lock);
    if (slot->updated) {
        local = slot->data;
        slot->updated = false;
        printed = true;
    }
    pthread_mutex_unlock(&slot->lock);

    if (printed) {
        print_bm_cbit_report(&local, "BM ENGINEERING CBIT REPORT", device_name);
    }
    return printed;
}

static bool drain_and_print_bm_flag_slot(hm_bm_flag_slot_t *slot, const char *device_name)
{
    bool printed = false;
    bm_flag_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; slot->updated = false; printed = true; }
    pthread_mutex_unlock(&slot->lock);
    if (printed) print_bm_flag_cbit_report(&local, device_name);
    return printed;
}

static bool drain_and_print_dtn_es_slot(hm_dtn_es_slot_t *slot, const char *device_name)
{
    bool printed = false;
    dtn_es_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; slot->updated = false; printed = true; }
    pthread_mutex_unlock(&slot->lock);
    if (printed) print_dtn_es_cbit_report(&local, device_name);
    return printed;
}

static bool drain_and_print_dtn_sw_slot(hm_dtn_sw_slot_t *slot, const char *device_name)
{
    bool printed = false;
    dtn_sw_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; slot->updated = false; printed = true; }
    pthread_mutex_unlock(&slot->lock);
    if (printed) print_dtn_sw_cbit_report(&local, device_name);
    return printed;
}

static void *hm_printer_thread_function(void *arg)
{
    (void)arg;
    uint64_t tick = 0;
    while (g_stop_flag == NULL || !(*g_stop_flag))
    {
        bool any = false;
        any |= drain_and_print_pcs_slot(&vs_cpu_usage_slot,   "VS");
        any |= drain_and_print_pcs_slot(&flcs_cpu_usage_slot, "FLCS");

        any |= drain_and_print_bm_eng_slot(&vs_bm_engineering_slot,   "VS");
        any |= drain_and_print_bm_eng_slot(&flcs_bm_engineering_slot, "FLCS");

        any |= drain_and_print_bm_flag_slot(&vs_bm_flag_slot,   "VS");
        any |= drain_and_print_bm_flag_slot(&flcs_bm_flag_slot, "FLCS");

        any |= drain_and_print_dtn_es_slot(&vs_dtn_es_slot,   "VS");
        any |= drain_and_print_dtn_es_slot(&flcs_dtn_es_slot, "FLCS");

        any |= drain_and_print_dtn_sw_slot(&vs_dtn_sw_slot,   "VS");
        any |= drain_and_print_dtn_sw_slot(&flcs_dtn_sw_slot, "FLCS");

        // Her saniye tanılama satırı bas (printer thread canlı mı + paket geliyor mu göster).
        // Böylece "hiç çıktı görmüyorum" durumunda bile thread'in çalıştığı net olur.
        uint64_t total        = __atomic_load_n(&g_hm_rx_total,        __ATOMIC_RELAXED);
        uint64_t vs_cnt       = __atomic_load_n(&g_hm_rx_vs_cpu,       __ATOMIC_RELAXED);
        uint64_t flcs_cnt     = __atomic_load_n(&g_hm_rx_flcs_cpu,     __ATOMIC_RELAXED);
        uint64_t cbit_cnt     = __atomic_load_n(&g_hm_rx_cbit,         __ATOMIC_RELAXED);
        uint64_t unknown_vlid = __atomic_load_n(&g_hm_rx_unknown_vlid, __ATOMIC_RELAXED);
        uint64_t unknown_msg  = __atomic_load_n(&g_hm_rx_unknown_msg,  __ATOMIC_RELAXED);
        uint64_t short_cnt    = __atomic_load_n(&g_hm_rx_short,        __ATOMIC_RELAXED);
        uint64_t empty_cnt    = __atomic_load_n(&g_hm_rx_empty,        __ATOMIC_RELAXED);
        printf("[HM] tick=%lu total=%lu vs_cpu=%lu flcs_cpu=%lu cbit=%lu empty=%lu unk_vlid=%lu unk_msg=%lu short=%lu printed=%d\n",
               (unsigned long)tick,
               (unsigned long)total,
               (unsigned long)vs_cnt,
               (unsigned long)flcs_cnt,
               (unsigned long)cbit_cnt,
               (unsigned long)empty_cnt,
               (unsigned long)unknown_vlid,
               (unsigned long)unknown_msg,
               (unsigned long)short_cnt,
               any ? 1 : 0);
        fflush(stdout);
        tick++;

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

// 2b. BM FLAG — 105 B. Her flag byte'ı BM Engineering'deki ilgili float
// alanının durum bayrağı. İsimler BM Engineering struct'ından birebir.
// BM Engineering toplam 96 float, BM Flag payload 93 byte → VMC Board
// kısmı ilk 4 alanla sınırlandırıldı (spec gelince tamamlanacak).

static const char * const BM_FLAG_NAMES_VS_CPU[22] = {
    "VSCPU_12V_current",           "VSCPU_core_imon",
    "VSCPU_3v3_rail_input_current","VSCPU_G1VDD_input_current",
    "VSCPU_XVDD_input_current",    "VSCPU_SVDD_input_current",
    "VSCPU_G1VDD_1v35_rail_voltage","VSCPU_BM_ADC_3V_1",
    "VSCPU_1v8_rail_voltage",      "VSCPU_VCORE_rail_voltage",
    "VSCPU_S1VDD_rail_voltage",    "VSCPU_S2VDD_rail_voltage",
    "VSCPU_X1VDD_rail_voltage",    "VSCPU_X2VDD_rail_voltage",
    "VSCPU_12V_main_voltage",      "VSCPU_BM_ADC_3V_2",
    "VSCPU_1v8_rail_input_current","VSCPU_core_local_temperature",
    "VSCPU_core_remote_temperature","VSCPU_RAM_temperature",
    "VSCPU_FLASH_temperature",     "VSCPU_power_IC_temperature",
};
static const char * const BM_FLAG_NAMES_FLCS_CPU[22] = {
    "FCCPU_12V_current",           "FCCPU_core_imon",
    "FCCPU_3v3_rail_input_current","FCCPU_G1VDD_input_current",
    "FCCPU_XVDD_input_current",    "FCCPU_SVDD_input_current",
    "FCCPU_G1VDD_1v35_rail_voltage","FCCPU_BM_ADC_3V_1",
    "FCCPU_1v8_rail_voltage",      "FCCPU_VCORE_rail_voltage",
    "FCCPU_S1VDD_rail_voltage",    "FCCPU_S2VDD_rail_voltage",
    "FCCPU_X1VDD_rail_voltage",    "FCCPU_X2VDD_rail_voltage",
    "FCCPU_12V_main_voltage",      "FCCPU_BM_ADC_3V_2",
    "FCCPU_core_local_temperature","FCCPU_core_remote_temperature",
    "FCCPU_RAM_temperature",       "FCCPU_FLASH_temperature",
    "FCCPU_1v8_input_current",     "FCCPU_power_IC_temperature",
};
static const char * const BM_FLAG_NAMES_DTN_ES[15] = {
    "DTN_ES_VDD_input_current",    "DTN_ES_3v3_rail_input_current",
    "DTN_ES_FO_RX_imon_r",         "DTN_ES_1v8_rail_input_current",
    "DTN_ES_1v3_rail_input_current","DTN_ES_12V_main_current",
    "DTN_ES_BM_3V_1",              "DTN_ES_1V_VDD_rail_voltage",
    "DTN_ES_2v5_rail_voltage",     "DTN_ES_1v8_rail_voltage",
    "DTN_ES_1V_VDDA_rail_voltage", "DTN_ES_3v3_VDDIX_rail_voltage",
    "DTN_ES_2v5_rail_input_current","DTN_ES_12V_main_voltage",
    "DTN_ES_BM_3V_2",
};
static const char * const BM_FLAG_NAMES_DTN_VSW[15] = {
    "DTN_VSW_VDD_input_current",   "DTN_VSW_3v3_rail_input_current",
    "DTN_VSW_FO_V_RX_imon_r",      "DTN_VSW_1v8_rail_input_current",
    "DTN_VSW_1v3_rail_input_current","DTN_VSW_12V_main_current",
    "DTN_VSW_BM_ADC_3V_1",         "DTN_VSW_1V_VDD_rail_voltage",
    "DTN_VSW_2v5_rail_voltage",    "DTN_VSW_1v8_rail_voltage",
    "DTN_VSW_1V_VDDA_rail_voltage","DTN_VSW_2v5_rail_input_current",
    "DTN_VSW_12V_main_voltage",    "DTN_VSW_FO_VF_RX_imon_r",
    "DTN_VSW_3v3_VDDIX_voltage",
};
static const char * const BM_FLAG_NAMES_DTN_FSW[15] = {
    "DTN_FSW_VDD_input_current",   "DTN_FSW_3v3_rail_input_current",
    "DTN_FSW_FO_F_RX_imon_r",      "DTN_FSW_1v8_rail_input_current",
    "DTN_FSW_1v3_rail_input_current","DTN_FSW_12V_main_current",
    "DTN_FSW_BM_ADC_3V_1",         "DTN_FSW_1V_VDD_rail_voltage",
    "DTN_FSW_2v5_rail_voltage",    "DTN_FSW_1v8_rail_voltage",
    "DTN_FSW_1V_VDDA_rail_voltage","DTN_FSW_3v3_VDDIX_rail_voltage",
    "DTN_FSW_2v5_rail_input_current","DTN_FSW_12V_main_voltage",
    "DTN_FSW_BM_ADC_3V_2",
};
// VMC Board'un ilk 4 alanı (flag payload'unda sadece 4 byte var).
static const char * const BM_FLAG_NAMES_VMC_BOARD[4] = {
    "PSM_PWR_PRI_VOLS", "PSM_PWR_SEC_VOLS",
    "PSM_INPUT_CURS",   "PSM_TEMP",
};

static void print_named_flag_section(const char *title,
                                     const char * const *names,
                                     const uint8_t *bytes,
                                     size_t count, size_t base)
{
    printf("\n[ %s ]  (%zu bytes, payload offset %zu..%zu)\n",
           title, count, base, base + count - 1);
    printf(" %-34s | FLAG | OFF\n", "FIELD");
    printf("------------------------------------|------|------\n");
    for (size_t i = 0; i < count; i++) {
        printf(" %-34s | 0x%02x | +%zu\n", names[i], bytes[i], base + i);
    }
}

void print_bm_flag_cbit_report(const bm_flag_cbit_report_t *data, const char *device_name)
{
    if (!data) return;
    const char *prefix = device_name ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                           [%s] BM FLAG CBIT REPORT                                     \n", prefix);
    printf("========================================================================================\n");

    printf("[ GENERAL ]\n");
    printf(" Msg ID       : %-18u | Msg Len      : %u\n",
           data->header_st.message_identifier, data->header_st.message_len);
    printf(" Timestamp    : %-18lu | LRU ID       : %u\n",
           (unsigned long)data->header_st.timestamp, data->lru_id);

    const uint8_t *p = data->payload;
    // VS_CPU(22) | FLCS_CPU(22) | DTN_ES(15) | DTN_VSW(15) | DTN_FSW(15) | VMC_BOARD(4) = 93
    print_named_flag_section("VS CPU FLAGS",    BM_FLAG_NAMES_VS_CPU,    p +  0, 22,  0);
    print_named_flag_section("FLCS CPU FLAGS",  BM_FLAG_NAMES_FLCS_CPU,  p + 22, 22, 22);
    print_named_flag_section("DTN ES FLAGS",    BM_FLAG_NAMES_DTN_ES,    p + 44, 15, 44);
    print_named_flag_section("DTN VSW FLAGS",   BM_FLAG_NAMES_DTN_VSW,   p + 59, 15, 59);
    print_named_flag_section("DTN FSW FLAGS",   BM_FLAG_NAMES_DTN_FSW,   p + 74, 15, 74);
    print_named_flag_section("VMC BOARD FLAGS", BM_FLAG_NAMES_VMC_BOARD, p + 89,  4, 89);

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
    printf(" ES FW Ver    : %u.%u.%u              | HW Temp      : %.3f °C\n",
           es->A664_ES_FW_VER.major, es->A664_ES_FW_VER.minor, es->A664_ES_FW_VER.bugfix,
           es->A664_ES_HW_TEMP);
    printf(" Device ID    : %-18lu | HW VCC Int   : %.3f V\n",
           (unsigned long)es->A664_ES_DEV_ID, es->A664_ES_HW_VCC_INT);
    printf(" ES Mode      : %-18lu | Transc Temp  : %.3f °C\n",
           (unsigned long)es->A664_ES_MODE, es->A664_ES_TRANSCEIVER_TEMP);
    printf(" PTP RC Stat  : %-18u | Port A Sync  : %u\n", es->A664_PTP_RC_STATUS, es->A664_PTP_PORT_A_SYNC);
    printf(" Config ID    : %-18lu | Port B Sync  : %u\n", (unsigned long)es->A664_ES_CONFIG_ID, es->A664_PTP_PORT_B_SYNC);

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
    // FW version'ı u64'ün düşük 3 byte'ı major.minor.bugfix olarak yorumla
    uint8_t fw_major  = (uint8_t)((sw->A664_SW_FW_VER >> 16) & 0xff);
    uint8_t fw_minor  = (uint8_t)((sw->A664_SW_FW_VER >>  8) & 0xff);
    uint8_t fw_bugfix = (uint8_t)( sw->A664_SW_FW_VER        & 0xff);
    uint8_t ef_major  = (uint8_t)((sw->A664_SW_EMBEDEED_ES_FW_VER >> 16) & 0xff);
    uint8_t ef_minor  = (uint8_t)((sw->A664_SW_EMBEDEED_ES_FW_VER >>  8) & 0xff);
    uint8_t ef_bugfix = (uint8_t)( sw->A664_SW_EMBEDEED_ES_FW_VER        & 0xff);

    printf(" Device ID    : %-18u | FW Version   : %u.%u.%u\n",
           sw->A664_SW_DEV_ID, fw_major, fw_minor, fw_bugfix);
    printf(" TX Total Cnt : %-18lu | RX Total Cnt : %lu\n",
           (unsigned long)sw->A664_SW_TX_TOTAL_COUNT, (unsigned long)sw->A664_SW_RX_TOTAL_COUNT);
    printf(" Transc Temp  : %-8.3f °C     | Shared Temp  : %.3f °C\n",
           sw->A664_SW_TRANSCEIVER_TEMP, sw->A664_SW_SHARED_TRANSCEIVER_TEMP);
    printf(" Voltage      : %-8.3f V      | Temperature  : %.3f °C\n",
           sw->A664_SW_VOLTAGE, sw->A664_SW_TEMPERATURE);
    printf(" Port Count   : %-18u | Mode         : %u\n", sw->A664_SW_PORT_COUNT, sw->A664_SW_MODE);
    printf(" Config ID    : %-18u | Embedded FW  : %u.%u.%u\n",
           sw->A664_SW_CONFIGURATION_ID, ef_major, ef_minor, ef_bugfix);

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
//
// Birim notu: tüm zaman alanları NANOSANIYE (ns).
//   total_run_time  : örnekleme penceresi genişliği (ör. ~47 ms ≈ 47,000,000 ns)
//   usage           : o pencerede CPU'nun meşgul geçirdiği süre (ns)
//   percentage      ≈ (usage / total_run_time) × 100
//   latest_read_time: son örneklemenin zaman damgası (ns, monotonik)
//   Bellek alanları BYTE cinsindendir.
static inline double ns_to_ms(uint64_t ns) { return (double)ns / 1.0e6; }
static inline double ns_to_s (uint64_t ns) { return (double)ns / 1.0e9; }
static inline double b_to_kb (uint64_t b)  { return (double)b  / 1024.0; }
static inline double b_to_mb (uint64_t b)  { return (double)b  / (1024.0 * 1024.0); }

void print_pcs_profile_stats(const Pcs_profile_stats *data, const char *device_name)
{
    if (!data) return;

    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                             [%s] CPU USAGE / PROFILE STATS                            \n", prefix);
    printf("========================================================================================\n");
    printf("(time fields in ns, memory fields in bytes)\n");

    printf("\n[ GENERAL ]\n");
    printf(" Sample Count     : %lu\n",                     (unsigned long)data->sample_count);
    printf(" Latest Read Time : %lu ns  (%.3f s)\n",        (unsigned long)data->latest_read_time, ns_to_s (data->latest_read_time));
    printf(" Total Run Time   : %lu ns  (%.3f ms window)\n",(unsigned long)data->total_run_time,   ns_to_ms(data->total_run_time));

    const Pcs_cpu_exec_time_type *ct = &data->cpu_exec_time;
    printf("\n[ CPU EXEC TIME ]  usage = CPU busy ns in the %.3f ms window\n", ns_to_ms(data->total_run_time));
    printf(" %-6s | %-10s | %-18s | %-12s\n", "KIND", "PERCENT", "USAGE (ns)", "USAGE (ms)");
    printf("--------|------------|--------------------|-------------\n");
    printf(" %-6s | %-8u %% | %-18lu | %10.3f\n", "min",  ct->min_exec_time.percentage,  (unsigned long)ct->min_exec_time.usage,  ns_to_ms(ct->min_exec_time.usage));
    printf(" %-6s | %-8u %% | %-18lu | %10.3f\n", "max",  ct->max_exec_time.percentage,  (unsigned long)ct->max_exec_time.usage,  ns_to_ms(ct->max_exec_time.usage));
    printf(" %-6s | %-8u %% | %-18lu | %10.3f\n", "avg",  ct->avg_exec_time.percentage,  (unsigned long)ct->avg_exec_time.usage,  ns_to_ms(ct->avg_exec_time.usage));
    printf(" %-6s | %-8u %% | %-18lu | %10.3f\n", "last", ct->last_exec_time.percentage, (unsigned long)ct->last_exec_time.usage, ns_to_ms(ct->last_exec_time.usage));

    printf("\n[ MEMORY PROFILE ]  sizes in bytes (KB = bytes/1024, MB = bytes/1048576)\n");
    printf(" %-6s | %-14s | %-14s | %-14s | %-10s | %-10s\n",
           "REGION", "TOTAL (B)", "USED (B)", "MAX USED (B)", "TOTAL(MB)", "USED(KB)");
    printf("--------|----------------|----------------|----------------|------------|-----------\n");
    printf(" %-6s | %-14lu | %-14lu | %-14lu | %10.2f | %10.2f\n", "heap",
           (unsigned long)data->heap_mem.total_size,
           (unsigned long)data->heap_mem.used_size,
           (unsigned long)data->heap_mem.max_used_size,
           b_to_mb(data->heap_mem.total_size),
           b_to_kb(data->heap_mem.used_size));
    printf(" %-6s | %-14lu | %-14lu | %-14lu | %10.2f | %10.2f\n", "stack",
           (unsigned long)data->stack_mem.total_size,
           (unsigned long)data->stack_mem.used_size,
           (unsigned long)data->stack_mem.max_used_size,
           b_to_mb(data->stack_mem.total_size),
           b_to_kb(data->stack_mem.used_size));
    printf("========================================================================================\n");
}
