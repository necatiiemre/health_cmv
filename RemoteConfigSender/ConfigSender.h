#ifndef CONFIG_SENDER_H
#define CONFIG_SENDER_H

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>

/**
 * @brief Raw packet array to UDP config sender + interface monitoring
 *
 * Array format: Ethernet Header (14) + IP Header (20) + UDP Header (8) + Payload
 * All information is contained within the array, no extra parameters needed.
 */
class ConfigSender {
public:
    ConfigSender();
    ~ConfigSender();

    /**
     * @brief Initialize UDP socket
     * @param interface_name Interface to use for sending (optional)
     * @return true on success
     */
    bool init(const std::string& interface_name = "");

    /**
     * @brief Close all sockets
     */
    void close();

    /**
     * @brief Send packet from raw array (Layer 3 UDP)
     * IP, port and payload are automatically parsed from the array
     * @param raw_packet Raw array containing Ethernet+IP+UDP+Payload
     * @param size Array size
     * @return true on success
     */
    bool sendRawPacket(const unsigned char* raw_packet, size_t size);

    /**
     * @brief Send raw Ethernet frame via AF_PACKET (Layer 2)
     * For sending through interfaces without IP addresses
     * @param interface_name Interface to send from (e.g., "ens1f0np0")
     * @param raw_packet Complete Ethernet frame (ETH+IP+UDP+Payload)
     * @param size Frame size
     * @return true on success
     */
    bool sendRawEthernet(const std::string& interface_name, const unsigned char* raw_packet, size_t size);

    /**
     * @brief Send multiple raw packets
     * @param packets Array of packet pointers
     * @param sizes Size of each packet
     * @param count Number of packets
     * @param delay_ms Delay between packets (ms)
     * @return true if all packets sent successfully
     */
    bool sendAllRaw(const unsigned char** packets, const size_t* sizes, size_t count, uint32_t delay_ms = 10);

    /**
     * @brief Monitor interface for incoming packets (blocking)
     * @param interface_name Interface name (e.g., "ens1f0np0", "eth0.34")
     * @param timeout_seconds Monitoring duration in seconds
     * @return Number of packets received, -1 on error
     */
    int monitorInterface(const std::string& interface_name, int timeout_seconds);

    /**
     * @brief Start monitoring in background thread
     * @param interface_name Interface to monitor
     * @param timeout_seconds Maximum monitoring duration
     * @return true if started successfully
     */
    bool startMonitoringAsync(const std::string& interface_name, int timeout_seconds);

    /**
     * @brief Stop background monitoring
     */
    void stopMonitoring();

    /**
     * @brief Wait for monitoring to complete
     * @return Number of packets received
     */
    int waitForMonitoring();

    /**
     * @brief Check if monitoring is active
     */
    bool isMonitoring() const;

    /**
     * @brief Get current packet count (thread-safe)
     */
    int getPacketCount() const;

private:
    int m_socket;
    int m_raw_socket;  // Raw socket for monitoring

    // Background monitoring
    std::thread m_monitor_thread;
    std::atomic<bool> m_monitoring_active;
    std::atomic<bool> m_stop_requested;
    std::atomic<int> m_packet_count;
    std::string m_monitor_interface;
    int m_monitor_timeout;

    // Header offsets
    static const size_t ETH_HEADER_LEN = 14;
    static const size_t IP_HEADER_LEN = 20;
    static const size_t UDP_HEADER_LEN = 8;
    static const size_t MIN_PACKET_LEN = ETH_HEADER_LEN + IP_HEADER_LEN + UDP_HEADER_LEN;

    // Parse helpers
    std::string parseDestIP(const unsigned char* raw_packet);
    uint16_t parseDestPort(const unsigned char* raw_packet);
    const unsigned char* getPayload(const unsigned char* raw_packet);
    size_t getPayloadSize(const unsigned char* raw_packet, size_t total_size);

    // Packet analysis
    void printPacketInfo(const unsigned char* buffer, size_t len, int packet_num);

    // Background monitoring worker
    void monitorWorker();
};

#endif // CONFIG_SENDER_H
