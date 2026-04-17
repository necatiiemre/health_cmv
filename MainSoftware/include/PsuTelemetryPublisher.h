/**
 * @file PsuTelemetryPublisher.h
 * @brief Periodically samples PSU voltage/current/power and pushes them as
 *        UDP datagrams to the DPDK process running on the server.
 *
 * Usage (RAII):
 *   {
 *       PsuTelemetryPublisher pub(PSUG30, "10.1.33.2", PSU_TELEM_PORT);
 *       pub.start();
 *       // ... long-running test ...
 *   } // destructor stops the thread on any exit path
 *
 * The publisher loop also doubles as an application-level heartbeat for the
 * PSU's TCP connection. Because we issue SCPI every second, the connection
 * never sits idle long enough for NAT/keepalive to drop it mid-test, which
 * is the root cause of the "after 2h the PSU doesn't turn off" bug.
 */
#ifndef PSU_TELEMETRY_PUBLISHER_H
#define PSU_TELEMETRY_PUBLISHER_H

#include "Device.h"
#include <netinet/in.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class PsuTelemetryPublisher {
public:
    /**
     * @param device       PSU to poll (e.g. PSUG30)
     * @param dst_host     destination IP or hostname (the DPDK server)
     * @param dst_port     destination UDP port (default: PSU_TELEM_PORT = 20050)
     * @param interval_ms  poll/publish interval in milliseconds (default: 1000)
     */
    PsuTelemetryPublisher(Device device,
                          std::string dst_host,
                          uint16_t dst_port,
                          uint32_t interval_ms = 1000);

    // Non-copyable / non-movable - owns a thread.
    PsuTelemetryPublisher(const PsuTelemetryPublisher&) = delete;
    PsuTelemetryPublisher& operator=(const PsuTelemetryPublisher&) = delete;

    // RAII: destructor stops the thread if still running.
    ~PsuTelemetryPublisher();

    /** Start the background publisher. Returns false if socket setup failed. */
    bool start();

    /** Stop the background publisher and join. Idempotent. */
    void stop();

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }
    uint32_t packetsSent() const { return m_packets_sent.load(std::memory_order_relaxed); }
    uint32_t errors() const { return m_errors.load(std::memory_order_relaxed); }

private:
    void run();

    Device        m_device;
    std::string   m_dst_host;
    uint16_t      m_dst_port;
    uint32_t      m_interval_ms;

    int           m_sockfd;  // UDP socket (-1 when closed); NOT connect()ed -
                             // we use sendto() so each send does its own
                             // routing/ARP and cached ICMP errors on a
                             // connected socket don't wedge subsequent sends.
    sockaddr_in   m_dst_addr;
    std::thread   m_thread;
    std::atomic<bool>     m_running;
    std::atomic<uint32_t> m_packets_sent;
    std::atomic<uint32_t> m_errors;
    uint32_t      m_seq;
};

#endif // PSU_TELEMETRY_PUBLISHER_H