#ifndef HM_REQUEST_SENDER_H
#define HM_REQUEST_SENDER_H

#include <stdbool.h>
#include <stdint.h>

// PBIT request paketleri DPDK Port 2 (ör. ens1f0np0) üzerinden,
// tx_worker'ların kullanmadığı dedicated bir TX queue ile gönderilir.
// Bu queue tek üreticili (sadece bu modül) olduğundan rte_eth_tx_burst
// çağrısı lock'suz güvenlidir.
#define HM_REQUEST_TX_PORT_ID  2
#define HM_REQUEST_TX_QUEUE_ID NUM_TX_CORES  // tx_worker'lar 0..NUM_TX_CORES-1 kullanır

// vmp_cmsw_header_t.message_identifier değeri (request yönü)
#define HM_REQUEST_MSG_IDENTIFIER 50

// 1 Hz gönderim periyodu
#define HM_REQUEST_PERIOD_MS 1000

// Port 2 mbuf pool'unu bulur ve iç durumu hazırlar. Port init + start_txrx_workers
// tamamlandıktan sonra çağrılmalı (pool'un oluşmuş olması gerekir).
int hm_request_sender_init(void);

// 1 Hz pthread'i başlatır. force_quit durdurma sinyali olarak dinlenir.
int hm_request_sender_start(volatile bool *force_quit);

// Thread'i durdurur ve join eder. Idempotent.
void hm_request_sender_stop(void);

// Diagnostic sayaçları
uint64_t hm_request_sender_tx_count(void);
uint64_t hm_request_sender_tx_drops(void);

#endif /* HM_REQUEST_SENDER_H */
