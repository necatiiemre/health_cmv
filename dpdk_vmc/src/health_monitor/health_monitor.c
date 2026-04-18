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

// (Önceden printer için pthread state vardı; dashboard artık main thread'de.)

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
// ----------------------------------------------------------------------------
// Her tick'te SLOT BAŞINA en son veriyi bas — "updated" bayrağı "en az bir kez
// veri geldi mi?" anlamında kullanılıyor; bayrak tick'ler arasında temizlenmez.
// Böylece paket gelmediği saniyelerde bile son güncel snapshot ekrana gelir.
// ============================================================================
static bool drain_and_print_pcs_slot(hm_pcs_slot_t *slot, const char *device_name)
{
    bool has_data = false;
    Pcs_profile_stats local;

    pthread_mutex_lock(&slot->lock);
    if (slot->updated) {
        local = slot->data;
        has_data = true;
    }
    pthread_mutex_unlock(&slot->lock);

    if (has_data) {
        print_pcs_profile_stats(&local, device_name);
    }
    return has_data;
}

static bool drain_and_print_bm_eng_slot(hm_bm_slot_t *slot, const char *device_name)
{
    bool has_data = false;
    bm_engineering_cbit_report_t local;

    pthread_mutex_lock(&slot->lock);
    if (slot->updated) {
        local = slot->data;
        has_data = true;
    }
    pthread_mutex_unlock(&slot->lock);

    if (has_data) {
        print_bm_cbit_report(&local, "BM ENGINEERING CBIT REPORT", device_name);
    }
    return has_data;
}

static bool drain_and_print_bm_flag_slot(hm_bm_flag_slot_t *slot, const char *device_name)
{
    bool has_data = false;
    bm_flag_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; has_data = true; }
    pthread_mutex_unlock(&slot->lock);
    if (has_data) print_bm_flag_cbit_report(&local, device_name);
    return has_data;
}

static bool drain_and_print_dtn_es_slot(hm_dtn_es_slot_t *slot, const char *device_name)
{
    bool has_data = false;
    dtn_es_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; has_data = true; }
    pthread_mutex_unlock(&slot->lock);
    if (has_data) print_dtn_es_cbit_report(&local, device_name);
    return has_data;
}

static bool drain_and_print_dtn_sw_slot(hm_dtn_sw_slot_t *slot, const char *device_name)
{
    bool has_data = false;
    dtn_sw_cbit_report_t local;
    pthread_mutex_lock(&slot->lock);
    if (slot->updated) { local = slot->data; has_data = true; }
    pthread_mutex_unlock(&slot->lock);
    if (has_data) print_dtn_sw_cbit_report(&local, device_name);
    return has_data;
}

// ============================================================================
// Public dashboard — main thread içinden her saniye çağrılır.
// Stats tablosundan sonra çalıştırıldığında çıktı sıralı akar.
// ============================================================================
void hm_print_dashboard(void)
{
    static uint64_t tick = 0;

    // Dashboard cycle header — her saniyenin başlangıcını net işaretler
    printf("\n\n");
    printf("########################################################################################\n");
    printf("###  HEALTH MONITOR DASHBOARD — tick %-6lu                                            ###\n",
           (unsigned long)tick);
    printf("########################################################################################\n");

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

    // Her tick sonunda tanı satırı — sayaçlar + paket gelip gelmediği net.
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

// 2. BM ENGINEERING / FLAG DATA CBIT REPORT — tüm 96 float alanı basılır
static void bm_section_header(const char *title)
{
    printf("\n+------------------------------------------------+------------+\n");
    printf("| %-47s|   VALUE    |\n", title);
    printf("+------------------------------------------------+------------+\n");
}
static void bm_row(const char *name, float v)
{
    printf("| %-47s| %10.4f |\n", name, v);
}
static void bm_section_footer(void)
{
    printf("+------------------------------------------------+------------+\n");
}

void print_bm_cbit_report(const bm_engineering_cbit_report_t *data, const char *report_title, const char *device_name)
{
    if (!data) return;
    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                   [%s] %s                      \n", prefix, report_title);
    printf("========================================================================================\n");
    printf("[ GENERAL ]\n");
    printf(" Msg ID       : %-18u | Msg Len      : %u\n",
           data->header_st.message_identifier, data->header_st.message_len);
    printf(" Timestamp    : %-18lu | LRU ID       : %u\n",
           (unsigned long)data->header_st.timestamp, data->lru_id);
    printf(" Comm Status  : %u\n", data->comm_status);

    const bm_vs_cpu_status_data_t *v = &data->vs_status_st;
    bm_section_header("VS CPU STATUS DATA (22 fields)");
    bm_row("VSCPU_12V_current",                v->VSCPU_12V_current);
    bm_row("VSCPU_core_imon",                  v->VSCPU_core_imon);
    bm_row("VSCPU_3v3_rail_input_current",     v->VSCPU_3v3_rail_input_current);
    bm_row("VSCPU_G1VDD_input_current",        v->VSCPU_G1VDD_input_current);
    bm_row("VSCPU_XVDD_input_current",         v->VSCPU_XVDD_input_current);
    bm_row("VSCPU_SVDD_input_current",         v->VSCPU_SVDD_input_current);
    bm_row("VSCPU_G1VDD_1v35_rail_voltage",    v->VSCPU_G1VDD_1v35_rail_voltage);
    bm_row("VSCPU_BM_ADC_3V_1",                v->VSCPU_BM_ADC_3V_1);
    bm_row("VSCPU_1v8_rail_voltage",           v->VSCPU_1v8_rail_voltage);
    bm_row("VSCPU_VCORE_rail_voltage",         v->VSCPU_VCORE_rail_voltage);
    bm_row("VSCPU_S1VDD_rail_voltage",         v->VSCPU_S1VDD_rail_voltage);
    bm_row("VSCPU_S2VDD_rail_voltage",         v->VSCPU_S2VDD_rail_voltage);
    bm_row("VSCPU_X1VDD_rail_voltage",         v->VSCPU_X1VDD_rail_voltage);
    bm_row("VSCPU_X2VDD_rail_voltage",         v->VSCPU_X2VDD_rail_voltage);
    bm_row("VSCPU_12V_main_voltage",           v->VSCPU_12V_main_voltage);
    bm_row("VSCPU_BM_ADC_3V_2",                v->VSCPU_BM_ADC_3V_2);
    bm_row("VSCPU_1v8_rail_input_current",     v->VSCPU_1v8_rail_input_current);
    bm_row("VSCPU_core_local_temperature",     v->VSCPU_core_local_temperature);
    bm_row("VSCPU_core_remote_temperature",    v->VSCPU_core_remote_temperature);
    bm_row("VSCPU_RAM_temperature",            v->VSCPU_RAM_temperature);
    bm_row("VSCPU_FLASH_temperature",          v->VSCPU_FLASH_temperature);
    bm_row("VSCPU_power_IC_temperature",       v->VSCPU_power_IC_temperature);
    bm_section_footer();

    const bm_flcs_cpu_status_data_t *f = &data->flcs_status_st;
    bm_section_header("FLCS CPU STATUS DATA (22 fields)");
    bm_row("FCCPU_12V_current",                f->FCCPU_12V_current);
    bm_row("FCCPU_core_imon",                  f->FCCPU_core_imon);
    bm_row("FCCPU_3v3_rail_input_current",     f->FCCPU_3v3_rail_input_current);
    bm_row("FCCPU_G1VDD_input_current",        f->FCCPU_G1VDD_input_current);
    bm_row("FCCPU_XVDD_input_current",         f->FCCPU_XVDD_input_current);
    bm_row("FCCPU_SVDD_input_current",         f->FCCPU_SVDD_input_current);
    bm_row("FCCPU_G1VDD_1v35_rail_voltage",    f->FCCPU_G1VDD_1v35_rail_voltage);
    bm_row("FCCPU_BM_ADC_3V_1",                f->FCCPU_BM_ADC_3V_1);
    bm_row("FCCPU_1v8_rail_voltage",           f->FCCPU_1v8_rail_voltage);
    bm_row("FCCPU_VCORE_rail_voltage",         f->FCCPU_VCORE_rail_voltage);
    bm_row("FCCPU_S1VDD_rail_voltage",         f->FCCPU_S1VDD_rail_voltage);
    bm_row("FCCPU_S2VDD_rail_voltage",         f->FCCPU_S2VDD_rail_voltage);
    bm_row("FCCPU_X1VDD_rail_voltage",         f->FCCPU_X1VDD_rail_voltage);
    bm_row("FCCPU_X2VDD_rail_voltage",         f->FCCPU_X2VDD_rail_voltage);
    bm_row("FCCPU_12V_main_voltage",           f->FCCPU_12V_main_voltage);
    bm_row("FCCPU_BM_ADC_3V_2",                f->FCCPU_BM_ADC_3V_2);
    bm_row("FCCPU_core_local_temperature",     f->FCCPU_core_local_temperature);
    bm_row("FCCPU_core_remote_temperature",    f->FCCPU_core_remote_temperature);
    bm_row("FCCPU_RAM_temperature",            f->FCCPU_RAM_temperature);
    bm_row("FCCPU_FLASH_temperature",          f->FCCPU_FLASH_temperature);
    bm_row("FCCPU_1v8_input_current",          f->FCCPU_1v8_input_current);
    bm_row("FCCPU_power_IC_temperature",       f->FCCPU_power_IC_temperature);
    bm_section_footer();

    const bm_dtn_es_status_data_t *e = &data->dtn_es_status_st;
    bm_section_header("DTN ES STATUS DATA (15 fields)");
    bm_row("DTN_ES_VDD_input_current",         e->DTN_ES_VDD_input_current);
    bm_row("DTN_ES_3v3_rail_input_current",    e->DTN_ES_3v3_rail_input_current);
    bm_row("DTN_ES_FO_RX_imon_r",              e->DTN_ES_FO_RX_imon_r);
    bm_row("DTN_ES_1v8_rail_input_current",    e->DTN_ES_1v8_rail_input_current);
    bm_row("DTN_ES_1v3_rail_input_current",    e->DTN_ES_1v3_rail_input_current);
    bm_row("DTN_ES_12V_main_current",          e->DTN_ES_12V_main_current);
    bm_row("DTN_ES_BM_3V_1",                   e->DTN_ES_BM_3V_1);
    bm_row("DTN_ES_1V_VDD_rail_voltage",       e->DTN_ES_1V_VDD_rail_voltage);
    bm_row("DTN_ES_2v5_rail_voltage",          e->DTN_ES_2v5_rail_voltage);
    bm_row("DTN_ES_1v8_rail_voltage",          e->DTN_ES_1v8_rail_voltage);
    bm_row("DTN_ES_1V_VDDA_rail_voltage",      e->DTN_ES_1V_VDDA_rail_voltage);
    bm_row("DTN_ES_3v3_VDDIX_rail_voltage",    e->DTN_ES_3v3_VDDIX_rail_voltage);
    bm_row("DTN_ES_2v5_rail_input_current",    e->DTN_ES_2v5_rail_input_current);
    bm_row("DTN_ES_12V_main_voltage",          e->DTN_ES_12V_main_voltage);
    bm_row("DTN_ES_BM_3V_2",                   e->DTN_ES_BM_3V_2);
    bm_section_footer();

    const bm_vs_dtn_sw_status_data_t *vs = &data->vs_dtn_sw_status_st;
    bm_section_header("DTN VSW STATUS DATA (15 fields)");
    bm_row("DTN_VSW_VDD_input_current",        vs->DTN_VSW_VDD_input_current);
    bm_row("DTN_VSW_3v3_rail_input_current",   vs->DTN_VSW_3v3_rail_input_current);
    bm_row("DTN_VSW_FO_V_RX_imon_r",           vs->DTN_VSW_FO_V_RX_imon_r);
    bm_row("DTN_VSW_1v8_rail_input_current",   vs->DTN_VSW_1v8_rail_input_current);
    bm_row("DTN_VSW_1v3_rail_input_current",   vs->DTN_VSW_1v3_rail_input_current);
    bm_row("DTN_VSW_12V_main_current",         vs->DTN_VSW_12V_main_current);
    bm_row("DTN_VSW_BM_ADC_3V_1",              vs->DTN_VSW_BM_ADC_3V_1);
    bm_row("DTN_VSW_1V_VDD_rail_voltage",      vs->DTN_VSW_1V_VDD_rail_voltage);
    bm_row("DTN_VSW_2v5_rail_voltage",         vs->DTN_VSW_2v5_rail_voltage);
    bm_row("DTN_VSW_1v8_rail_voltage",         vs->DTN_VSW_1v8_rail_voltage);
    bm_row("DTN_VSW_1V_VDDA_rail_voltage",     vs->DTN_VSW_1V_VDDA_rail_voltage);
    bm_row("DTN_VSW_2v5_rail_input_current",   vs->DTN_VSW_2v5_rail_input_current);
    bm_row("DTN_VSW_12V_main_voltage",         vs->DTN_VSW_12V_main_voltage);
    bm_row("DTN_VSW_FO_VF_RX_imon_r",          vs->DTN_VSW_FO_VF_RX_imon_r);
    bm_row("DTN_VSW_3v3_VDDIX_voltage",        vs->DTN_VSW_3v3_VDDIX_voltage);
    bm_section_footer();

    const bm_flcs_dtn_sw_status_data_t *fs = &data->flcs_dtn_sw_status_st;
    bm_section_header("DTN FSW STATUS DATA (15 fields)");
    bm_row("DTN_FSW_VDD_input_current",        fs->DTN_FSW_VDD_input_current);
    bm_row("DTN_FSW_3v3_rail_input_current",   fs->DTN_FSW_3v3_rail_input_current);
    bm_row("DTN_FSW_FO_F_RX_imon_r",           fs->DTN_FSW_FO_F_RX_imon_r);
    bm_row("DTN_FSW_1v8_rail_input_current",   fs->DTN_FSW_1v8_rail_input_current);
    bm_row("DTN_FSW_1v3_rail_input_current",   fs->DTN_FSW_1v3_rail_input_current);
    bm_row("DTN_FSW_12V_main_current",         fs->DTN_FSW_12V_main_current);
    bm_row("DTN_FSW_BM_ADC_3V_1",              fs->DTN_FSW_BM_ADC_3V_1);
    bm_row("DTN_FSW_1V_VDD_rail_voltage",      fs->DTN_FSW_1V_VDD_rail_voltage);
    bm_row("DTN_FSW_2v5_rail_voltage",         fs->DTN_FSW_2v5_rail_voltage);
    bm_row("DTN_FSW_1v8_rail_voltage",         fs->DTN_FSW_1v8_rail_voltage);
    bm_row("DTN_FSW_1V_VDDA_rail_voltage",     fs->DTN_FSW_1V_VDDA_rail_voltage);
    bm_row("DTN_FSW_3v3_VDDIX_rail_voltage",   fs->DTN_FSW_3v3_VDDIX_rail_voltage);
    bm_row("DTN_FSW_2v5_rail_input_current",   fs->DTN_FSW_2v5_rail_input_current);
    bm_row("DTN_FSW_12V_main_voltage",         fs->DTN_FSW_12V_main_voltage);
    bm_row("DTN_FSW_BM_ADC_3V_2",              fs->DTN_FSW_BM_ADC_3V_2);
    bm_section_footer();

    const bm_vmc_board_status_data_t *b = &data->vmc_board_status_st;
    bm_section_header("VMC BOARD STATUS DATA (7 fields)");
    bm_row("PSM_PWR_PRI_VOLS",                 b->PSM_PWR_PRI_VOLS);
    bm_row("PSM_PWR_SEC_VOLS",                 b->PSM_PWR_SEC_VOLS);
    bm_row("PSM_INPUT_CURS",                   b->PSM_INPUT_CURS);
    bm_row("PSM_TEMP",                         b->PSM_TEMP);
    bm_row("BM_FPGA_temperature",              b->BM_FPGA_temperature);
    bm_row("Board_edge_temperature",           b->Board_edge_temperature);
    bm_row("BRD_MNGR_12V_main_current",        b->BRD_MNGR_12V_main_current);
    bm_section_footer();
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

// 3. DTN ES CBIT — tüm 47 monitoring alanı tek tabloda basılır
static void u64_row(const char *name, uint64_t v)
{
    printf("| %-47s| %20lu |\n", name, (unsigned long)v);
}
static void u64_hex_row(const char *name, uint64_t v)
{
    printf("| %-47s| 0x%018lx |\n", name, (unsigned long)v);
}
static void u32_row(const char *name, uint32_t v)
{
    printf("| %-47s| %20u |\n", name, v);
}
static void u16_row(const char *name, uint16_t v)
{
    printf("| %-47s| %20u |\n", name, v);
}
static void u8_row(const char *name, uint8_t v)
{
    printf("| %-47s| %20u |\n", name, v);
}
static void float_row(const char *name, float v, const char *unit)
{
    printf("| %-47s| %14.3f %-5s |\n", name, v, unit);
}
static void section_header_wide(const char *title)
{
    printf("\n+------------------------------------------------+----------------------+\n");
    printf("| %-47s| %-20s |\n", title, "VALUE");
    printf("+------------------------------------------------+----------------------+\n");
}
static void section_footer_wide(void)
{
    printf("+------------------------------------------------+----------------------+\n");
}

void print_dtn_es_cbit_report(const dtn_es_cbit_report_t *data, const char *device_name)
{
    if (!data) return;
    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                           [%s] DTN ES CBIT REPORT                                      \n", prefix);
    printf("========================================================================================\n");
    printf("[ GENERAL ]\n");
    printf(" Msg ID       : %-18u | Msg Len      : %u\n",
           data->header_st.message_identifier, data->header_st.message_len);
    printf(" Timestamp    : %-18lu | LRU ID       : %u\n",
           (unsigned long)data->header_st.timestamp, data->lru_id);
    printf(" Side Type    : %-18u | Network Type : %u\n", data->side_type, data->network_type);
    printf(" Comm Status  : %u\n", data->comm_status);

    const dtn_es_monitoring_t *m = &data->dtn_es_monitoring_st;

    section_header_wide("DEVICE / CONFIG");
    printf("| %-47s| %14u.%u.%u       |\n", "A664_ES_FW_VER (major.minor.bugfix)",
           m->A664_ES_FW_VER.major, m->A664_ES_FW_VER.minor, m->A664_ES_FW_VER.bugfix);
    u64_row("A664_ES_DEV_ID",           m->A664_ES_DEV_ID);
    u64_row("A664_ES_MODE",             m->A664_ES_MODE);
    u64_row("A664_ES_CONFIG_ID",        m->A664_ES_CONFIG_ID);
    u64_hex_row("A664_ES_BIT_STATUS",   m->A664_ES_BIT_STATUS);
    u64_hex_row("A664_ES_CONFIG_STATUS",m->A664_ES_CONFIG_STATUS);
    u64_row("A664_ES_VENDOR_TYPE",      m->A664_ES_VENDOR_TYPE);
    u64_row("A664_BSP_VER",             m->A664_BSP_VER);
    section_footer_wide();

    section_header_wide("PTP");
    u16_row("A664_PTP_CONFIG_ID",    m->A664_PTP_CONFIG_ID);
    u8_row ("A664_PTP_DEVICE_TYPE",  m->A664_PTP_DEVICE_TYPE);
    u8_row ("A664_PTP_RC_STATUS",    m->A664_PTP_RC_STATUS);
    u8_row ("A664_PTP_PORT_A_SYNC",  m->A664_PTP_PORT_A_SYNC);
    u8_row ("A664_PTP_PORT_B_SYNC",  m->A664_PTP_PORT_B_SYNC);
    u16_row("A664_PTP_SYNC_VL_ID",   m->A664_PTP_SYNC_VL_ID);
    u16_row("A664_PTP_REQ_VL_ID",    m->A664_PTP_REQ_VL_ID);
    u16_row("A664_PTP_RES_VL_ID",    m->A664_PTP_RES_VL_ID);
    u8_row ("A664_PTP_TOD_NETWORK",  m->A664_PTP_TOD_NETWORK);
    section_footer_wide();

    section_header_wide("HARDWARE SENSORS");
    float_row("A664_ES_HW_TEMP",          m->A664_ES_HW_TEMP,          "degC");
    float_row("A664_ES_HW_VCC_INT",       m->A664_ES_HW_VCC_INT,       "V");
    float_row("A664_ES_TRANSCEIVER_TEMP", m->A664_ES_TRANSCEIVER_TEMP, "degC");
    section_footer_wide();

    section_header_wide("PORT STATUS");
    u64_hex_row("A664_ES_PORT_A_STATUS", m->A664_ES_PORT_A_STATUS);
    u64_hex_row("A664_ES_PORT_B_STATUS", m->A664_ES_PORT_B_STATUS);
    section_footer_wide();

    section_header_wide("TX COUNTERS");
    u64_row("A664_ES_TX_INCOMING_COUNT",       m->A664_ES_TX_INCOMING_COUNT);
    u64_row("A664_ES_TX_A_OUTGOING_COUNT",     m->A664_ES_TX_A_OUTGOING_COUNT);
    u64_row("A664_ES_TX_B_OUTGOING_COUNT",     m->A664_ES_TX_B_OUTGOING_COUNT);
    u64_row("A664_ES_TX_VLID_DROP_COUNT",      m->A664_ES_TX_VLID_DROP_COUNT);
    u64_row("A664_ES_TX_LMIN_LMAX_DROP_COUNT", m->A664_ES_TX_LMIN_LMAX_DROP_COUNT);
    u64_row("A664_ES_TX_MAX_JITTER_DROP_COUNT",m->A664_ES_TX_MAX_JITTER_DROP_COUNT);
    section_footer_wide();

    section_header_wide("RX A COUNTERS");
    u64_row("A664_ES_RX_A_INCOMING_COUNT",          m->A664_ES_RX_A_INCOMING_COUNT);
    u64_row("A664_ES_RX_A_VLID_DROP_COUNT",         m->A664_ES_RX_A_VLID_DROP_COUNT);
    u64_row("A664_ES_RX_A_LMIN_LMAX_DROP_COUNT",    m->A664_ES_RX_A_LMIN_LMAX_DROP_COUNT);
    u64_row("A664_ES_RX_A_NET_ERR_COUNT",           m->A664_ES_RX_A_NET_ERR_COUNT);
    u64_row("A664_ES_RX_A_SEQ_ERR_COUNT",           m->A664_ES_RX_A_SEQ_ERR_COUNT);
    u64_row("A664_ES_RX_A_CRC_ERROR_COUNT",         m->A664_ES_RX_A_CRC_ERROR_COUNT);
    u64_row("A664_ES_RX_A_IP_CHECKSUM_ERROR_COUNT", m->A664_ES_RX_A_IP_CHECKSUM_ERROR_COUNT);
    section_footer_wide();

    section_header_wide("RX B COUNTERS");
    u64_row("A664_ES_RX_B_INCOMING_COUNT",          m->A664_ES_RX_B_INCOMING_COUNT);
    u64_row("A664_ES_RX_B_VLID_DROP_COUNT",         m->A664_ES_RX_B_VLID_DROP_COUNT);
    u64_row("A664_ES_RX_B_LMIN_LMAX_DROP_COUNT",    m->A664_ES_RX_B_LMIN_LMAX_DROP_COUNT);
    u64_row("A664_ES_RX_B_SEQ_ERR_COUNT",           m->A664_ES_RX_B_SEQ_ERR_COUNT);
    u64_row("A664_ES_RX_B_NET_ERR_COUNT",           m->A664_ES_RX_B_NET_ERR_COUNT);
    u64_row("A664_ES_RX_B_CRC_ERROR_COUNT",         m->A664_ES_RX_B_CRC_ERROR_COUNT);
    u64_row("A664_ES_RX_B_IP_CHECKSUM_ERROR_COUNT", m->A664_ES_RX_B_IP_CHECKSUM_ERROR_COUNT);
    section_footer_wide();

    section_header_wide("RX TOTAL + BSP");
    u64_row("A664_ES_RX_OUTGOING_COUNT",        m->A664_ES_RX_OUTGOING_COUNT);
    u64_row("A664_BSP_TX_PACKET_COUNT",         m->A664_BSP_TX_PACKET_COUNT);
    u64_row("A664_BSP_TX_BYTE_COUNT",           m->A664_BSP_TX_BYTE_COUNT);
    u64_row("A664_BSP_TX_ERROR_COUNT",          m->A664_BSP_TX_ERROR_COUNT);
    u64_row("A664_BSP_RX_PACKET_COUNT",         m->A664_BSP_RX_PACKET_COUNT);
    u64_row("A664_BSP_RX_BYTE_COUNT",           m->A664_BSP_RX_BYTE_COUNT);
    u64_row("A664_BSP_RX_ERROR_COUNT",          m->A664_BSP_RX_ERROR_COUNT);
    u64_row("A664_BSP_RX_MISSED_FRAME_COUNT",   m->A664_BSP_RX_MISSED_FRAME_COUNT);
    u64_row("A664_ES_BSP_QUEUING_RX_VL_PORT_DROP_COUNT",
            m->A664_ES_BSP_QUEUING_RX_VL_PORT_DROP_COUNT);
    section_footer_wide();

    printf("========================================================================================\n");
}

// 4. DTN SW CBIT — tam status + 8 port (her portun 19 alanı da basılır)
static void print_dtn_sw_port_block(int idx, const dtn_sw_port_mon_t *p)
{
    char title[64];
    snprintf(title, sizeof(title), "PORT[%d]  (port_id=%u)", idx, p->A664_SW_PORT_ID);
    section_header_wide(title);
    u16_row("A664_SW_PORT_ID",                             p->A664_SW_PORT_ID);
    u8_row ("A664_SW_PORT_i_BIT_STATUS",                   p->A664_SW_PORT_i_BIT_STATUS);
    u8_row ("A664_SW_PORT_i_STATUS",                       p->A664_SW_PORT_i_STATUS);
    u64_row("A664_SW_PORT_i_CRC_ERR_COUNT",                p->A664_SW_PORT_i_CRC_ERR_COUNT);
    u64_row("A664_SW_PORT_i_MIN_VL_FRAME_ERR_COUNT",       p->A664_SW_PORT_i_MIN_VL_FRAME_ERR_COUNT);
    u64_row("A664_SW_PORT_i_MAX_VL_FRAME_ERR_COUNT",       p->A664_SW_PORT_i_MAX_VL_FRAME_ERR_COUNT);
    u64_row("A664_SW_PORT_i_TRAFFIC_POLCY_DROP_COUNT",     p->A664_SW_PORT_i_TRAFFIC_POLCY_DROP_COUNT);
    u64_row("A664_SW_PORT_i_BE_COUNT",                     p->A664_SW_PORT_i_BE_COUNT);
    u64_row("A664_SW_PORT_i_TX_COUNT",                     p->A664_SW_PORT_i_TX_COUNT);
    u64_row("A664_SW_PORT_i_RX_COUNT",                     p->A664_SW_PORT_i_RX_COUNT);
    u64_row("A664_SW_PORT_i_VL_SOURCE_ERR_COUNT",          p->A664_SW_PORT_i_VL_SOURCE_ERR_COUNT);
    u64_row("A664_SW_PORT_i_MAX_DELAY_ERR_COUNT",          p->A664_SW_PORT_i_MAX_DELAY_ERR_COUNT);
    u64_row("A664_SW_PORT_i_VLID_DROP_COUNT",              p->A664_SW_PORT_i_VLID_DROP_COUNT);
    u64_row("A664_SW_PORT_i_UNDEF_MAC_COUNT",              p->A664_SW_PORT_i_UNDEF_MAC_COUNT);
    u64_row("A664_SW_PORT_i_HIGH_PRTY_QUE_OVRFLW_COUNT",   p->A664_SW_PORT_i_HIGH_PRTY_QUE_OVRFLW_COUNT);
    u64_row("A664_SW_PORT_i_LOW_PRTY_QUE_OVRFLW_COUNT",    p->A664_SW_PORT_i_LOW_PRTY_QUE_OVRFLW_COUNT);
    u64_row("A664_SW_PORT_i_MAX_DELAY",                    p->A664_SW_PORT_i_MAX_DELAY);
    u64_row("A664_SW_PORT_i_SPEED",                        p->A664_SW_PORT_i_SPEED);
    section_footer_wide();
}

void print_dtn_sw_cbit_report(const dtn_sw_cbit_report_t *data, const char *device_name)
{
    if (!data) return;
    const char *prefix = (device_name != NULL) ? device_name : "UNKNOWN";

    printf("\n========================================================================================\n");
    printf("                            [%s] DTN SW CBIT REPORT                                      \n", prefix);
    printf("========================================================================================\n");
    printf("[ GENERAL ]\n");
    printf(" Msg ID       : %-18u | Msg Len      : %u\n",
           data->header_st.message_identifier, data->header_st.message_len);
    printf(" Timestamp    : %-18lu | LRU ID       : %u\n",
           (unsigned long)data->header_st.timestamp, data->lru_id);
    printf(" Side Type    : %-18u | Network Type : %u\n", data->side_type, data->network_type);
    printf(" Comm Status  : %u\n", data->comm_status);

    const dtn_sw_status_mon_t *sw = &data->dtn_sw_monitoring_st.status;

    // FW version'ı u64'ün düşük 3 byte'ı major.minor.bugfix olarak yorumla
    uint8_t fw_major  = (uint8_t)((sw->A664_SW_FW_VER >> 16) & 0xff);
    uint8_t fw_minor  = (uint8_t)((sw->A664_SW_FW_VER >>  8) & 0xff);
    uint8_t fw_bugfix = (uint8_t)( sw->A664_SW_FW_VER        & 0xff);
    uint8_t ef_major  = (uint8_t)((sw->A664_SW_EMBEDEED_ES_FW_VER >> 16) & 0xff);
    uint8_t ef_minor  = (uint8_t)((sw->A664_SW_EMBEDEED_ES_FW_VER >>  8) & 0xff);
    uint8_t ef_bugfix = (uint8_t)( sw->A664_SW_EMBEDEED_ES_FW_VER        & 0xff);

    section_header_wide("SWITCH STATUS (14 fields)");
    u64_row("A664_SW_TX_TOTAL_COUNT",           sw->A664_SW_TX_TOTAL_COUNT);
    u64_row("A664_SW_RX_TOTAL_COUNT",           sw->A664_SW_RX_TOTAL_COUNT);
    float_row("A664_SW_TRANSCEIVER_TEMP",       sw->A664_SW_TRANSCEIVER_TEMP,        "degC");
    float_row("A664_SW_SHARED_TRANSCEIVER_TEMP",sw->A664_SW_SHARED_TRANSCEIVER_TEMP, "degC");
    u16_row("A664_SW_DEV_ID",                   sw->A664_SW_DEV_ID);
    u8_row ("A664_SW_PORT_COUNT",               sw->A664_SW_PORT_COUNT);
    u8_row ("A664_SW_TOKEN_BUCKET",             sw->A664_SW_TOKEN_BUCKET);
    u8_row ("A664_SW_MODE",                     sw->A664_SW_MODE);
    u8_row ("A664_SW_BE_MAC_UPDATE",            sw->A664_SW_BE_MAC_UPDATE);
    u8_row ("A664_SW_BE_UPSTREAM_MODE",         sw->A664_SW_BE_UPSTREAM_MODE);
    printf("| %-47s| %14u.%u.%u       |\n", "A664_SW_FW_VER (major.minor.bugfix)",
           fw_major, fw_minor, fw_bugfix);
    printf("| %-47s| %14u.%u.%u       |\n", "A664_SW_EMBEDEED_ES_FW_VER (M.m.b)",
           ef_major, ef_minor, ef_bugfix);
    float_row("A664_SW_VOLTAGE",                sw->A664_SW_VOLTAGE,                 "V");
    float_row("A664_SW_TEMPERATURE",            sw->A664_SW_TEMPERATURE,             "degC");
    u16_row("A664_SW_CONFIGURATION_ID",         sw->A664_SW_CONFIGURATION_ID);
    section_footer_wide();

    for (int i = 0; i < 8; i++) {
        print_dtn_sw_port_block(i, &data->dtn_sw_monitoring_st.port[i]);
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
