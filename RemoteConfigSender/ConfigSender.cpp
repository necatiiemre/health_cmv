#include "ConfigSender.h"
#include "DebugLog.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <thread>
#include <chrono>
#include <cerrno>

// Linux socket
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <poll.h>

ConfigSender::ConfigSender()
    : m_socket(-1)
    , m_raw_socket(-1)
    , m_monitoring_active(false)
    , m_stop_requested(false)
    , m_packet_count(0)
    , m_monitor_timeout(60) {
}

ConfigSender::~ConfigSender() {
    stopMonitoring();
    close();
}

bool ConfigSender::init(const std::string& interface_name) {
    if (m_socket >= 0) return true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket < 0) {
        std::cerr << "[ConfigSender] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Multicast settings
    int ttl = 1;
    if (setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        std::cerr << "[ConfigSender] Failed to set TTL: " << strerror(errno) << std::endl;
    }

    // Bind to specific interface (important for multicast)
    if (!interface_name.empty()) {
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);

        if (setsockopt(m_socket, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            std::cerr << "[ConfigSender] Interface bind error (" << interface_name << "): " << strerror(errno) << std::endl;
        } else {
            DEBUG_LOG("[ConfigSender] Bound to interface: " << interface_name);
        }
    }

    DEBUG_LOG("[ConfigSender] Socket initialized");
    return true;
}

void ConfigSender::close() {
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
    if (m_raw_socket >= 0) {
        ::close(m_raw_socket);
        m_raw_socket = -1;
    }
}

std::string ConfigSender::parseDestIP(const unsigned char* raw_packet) {
    const unsigned char* ip = raw_packet + ETH_HEADER_LEN + 16;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return std::string(buf);
}

uint16_t ConfigSender::parseDestPort(const unsigned char* raw_packet) {
    const unsigned char* udp = raw_packet + ETH_HEADER_LEN + IP_HEADER_LEN;
    return (udp[2] << 8) | udp[3];
}

const unsigned char* ConfigSender::getPayload(const unsigned char* raw_packet) {
    return raw_packet + ETH_HEADER_LEN + IP_HEADER_LEN + UDP_HEADER_LEN;
}

size_t ConfigSender::getPayloadSize(const unsigned char* raw_packet, size_t total_size) {
    if (total_size <= MIN_PACKET_LEN) return 0;
    return total_size - MIN_PACKET_LEN;
}

bool ConfigSender::sendRawPacket(const unsigned char* raw_packet, size_t size) {
    if (m_socket < 0) {
        std::cerr << "[ConfigSender] Socket not initialized!" << std::endl;
        return false;
    }

    if (size < MIN_PACKET_LEN) {
        std::cerr << "[ConfigSender] Packet too short: " << size << " bytes" << std::endl;
        return false;
    }

    std::string dest_ip = parseDestIP(raw_packet);
    uint16_t dest_port = parseDestPort(raw_packet);
    const unsigned char* payload = getPayload(raw_packet);
    size_t payload_size = getPayloadSize(raw_packet, size);

    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);

    if (inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr) <= 0) {
        std::cerr << "[ConfigSender] Invalid IP: " << dest_ip << std::endl;
        return false;
    }

    ssize_t sent = sendto(m_socket, payload, payload_size, 0,
                          (struct sockaddr*)&dest_addr, sizeof(dest_addr));

    if (sent < 0) {
        std::cerr << "[ConfigSender] Send error: " << strerror(errno) << " (errno=" << errno << ")" << std::endl;
        return false;
    }

    DEBUG_LOG("[ConfigSender] " << dest_ip << ":" << dest_port
              << " -> " << sent << " bytes OK");
    return true;
}

bool ConfigSender::sendRawEthernet(const std::string& interface_name,
                                    const unsigned char* raw_packet, size_t size) {
    if (size < ETH_HEADER_LEN) {
        std::cerr << "[ConfigSender] Ethernet frame too short: " << size << " bytes" << std::endl;
        return false;
    }

    int raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (raw_sock < 0) {
        std::cerr << "[ConfigSender] Failed to create raw socket: " << strerror(errno)
                  << " (root privileges required)" << std::endl;
        return false;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);

    if (ioctl(raw_sock, SIOCGIFINDEX, &ifr) < 0) {
        std::cerr << "[ConfigSender] Interface not found: " << interface_name
                  << " - " << strerror(errno) << std::endl;
        ::close(raw_sock);
        return false;
    }

    int ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifindex;
    sll.sll_halen = ETH_ALEN;
    std::memcpy(sll.sll_addr, raw_packet, ETH_ALEN);

    ssize_t sent = sendto(raw_sock, raw_packet, size, 0,
                          (struct sockaddr*)&sll, sizeof(sll));

    ::close(raw_sock);

    if (sent < 0) {
        std::cerr << "[ConfigSender] Raw send error: " << strerror(errno)
                  << " (errno=" << errno << ")" << std::endl;
        return false;
    }

    std::string dest_ip = parseDestIP(raw_packet);
    uint16_t dest_port = parseDestPort(raw_packet);

    DEBUG_LOG("[ConfigSender] L2 Raw -> " << interface_name << " | "
              << dest_ip << ":" << dest_port
              << " | " << sent << " bytes OK");

    return true;
}

bool ConfigSender::sendAllRaw(const unsigned char** packets, const size_t* sizes,
                              size_t count, uint32_t delay_ms) {
    if (!init()) return false;

    DEBUG_LOG("\n=== Config Transmission Starting ===");
    DEBUG_LOG("Total packets: " << count);

    size_t success = 0;
    for (size_t i = 0; i < count; i++) {
#if DEBUG_PRINT
        std::cout << "[" << (i+1) << "/" << count << "] ";
#endif
        if (sendRawPacket(packets[i], sizes[i])) {
            success++;
        }

        if (i < count - 1 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    DEBUG_LOG("=== Completed: " << success << "/" << count << " ===");
    return (success == count);
}

void ConfigSender::printPacketInfo(const unsigned char* buffer, size_t len, int packet_num) {
#if !DEBUG_PRINT
    (void)buffer; (void)len; (void)packet_num;
    return;
#else
    if (len < 14) return;

    uint16_t ethertype = (buffer[12] << 8) | buffer[13];
    int vlan_id = -1;
    size_t ip_offset = 14;

    // Check for VLAN tag (802.1Q)
    if (ethertype == 0x8100 && len >= 18) {
        uint16_t tci = (buffer[14] << 8) | buffer[15];
        vlan_id = tci & 0x0FFF;  // Lower 12 bits = VLAN ID
        ethertype = (buffer[16] << 8) | buffer[17];  // Real EtherType after VLAN tag
        ip_offset = 18;  // IP starts after VLAN tag
    }

    // Only process IPv4 packets
    if (ethertype != 0x0800 || len < ip_offset + 20) return;

    const unsigned char* ip = buffer + ip_offset;
    uint8_t protocol = ip[9];

    // Skip TCP packets (SSH traffic)
    if (protocol == 6) return;

    // Skip mDNS packets (port 5353)
    size_t udp_offset = ip_offset + 20;
    if (protocol == 17 && len >= udp_offset + 8) {
        const unsigned char* udp = buffer + udp_offset;
        uint16_t dst_port = (udp[2] << 8) | udp[3];
        if (dst_port == 5353) return;  // mDNS
    }

    printf("\n+--- Packet #%d (%zu bytes) ----------------------------\n", packet_num, len);
    printf("| ETH  Src: %02x:%02x:%02x:%02x:%02x:%02x\n",
           buffer[6], buffer[7], buffer[8], buffer[9], buffer[10], buffer[11]);
    printf("|      Dst: %02x:%02x:%02x:%02x:%02x:%02x\n",
           buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5]);

    if (vlan_id >= 0) {
        printf("|      VLAN ID: %d\n", vlan_id);
    }

    printf("|      Type: 0x%04x (IPv4)\n", ethertype);
    printf("| IP   Src: %u.%u.%u.%u\n", ip[12], ip[13], ip[14], ip[15]);
    printf("|      Dst: %u.%u.%u.%u\n", ip[16], ip[17], ip[18], ip[19]);
    printf("|      Proto: %u", protocol);

    if (protocol == 17) {
        printf(" (UDP)\n");

        if (len >= udp_offset + 8) {
            const unsigned char* udp = buffer + udp_offset;
            uint16_t src_port = (udp[0] << 8) | udp[1];
            uint16_t dst_port = (udp[2] << 8) | udp[3];
            uint16_t udp_len = (udp[4] << 8) | udp[5];

            printf("| UDP  Src Port: %u\n", src_port);
            printf("|      Dst Port: %u\n", dst_port);
            printf("|      Length: %u\n", udp_len);

            size_t payload_offset = udp_offset + 8;
            if (len > payload_offset) {
                printf("| DATA ");
                size_t payload_len = len - payload_offset;
                size_t show_len = (payload_len > 32) ? 32 : payload_len;
                for (size_t i = 0; i < show_len; i++) {
                    printf("%02x ", buffer[payload_offset + i]);
                }
                if (payload_len > 32) printf("...");
                printf("\n");
            }
        }
    } else if (protocol == 1) {
        printf(" (ICMP)\n");
    } else {
        printf("\n");
    }
    printf("+-------------------------------------------------------\n");
    fflush(stdout);
#endif
}

// ==================== Background Monitoring ====================

bool ConfigSender::startMonitoringAsync(const std::string& interface_name, int timeout_seconds) {
    if (m_monitoring_active) {
        std::cerr << "[Monitor] Already monitoring!" << std::endl;
        return false;
    }

    m_monitor_interface = interface_name;
    m_monitor_timeout = timeout_seconds;
    m_stop_requested = false;
    m_packet_count = 0;

    m_monitor_thread = std::thread(&ConfigSender::monitorWorker, this);

    // Wait for monitoring to actually start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return m_monitoring_active;
}

void ConfigSender::stopMonitoring() {
    if (!m_monitoring_active) return;

    m_stop_requested = true;

    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }
}

int ConfigSender::waitForMonitoring() {
    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }
    return m_packet_count;
}

bool ConfigSender::isMonitoring() const {
    return m_monitoring_active;
}

int ConfigSender::getPacketCount() const {
    return m_packet_count;
}

void ConfigSender::monitorWorker() {
    DEBUG_LOG("\n=== Background Monitoring Starting ===");
    DEBUG_LOG("Interface: " << m_monitor_interface);
    DEBUG_LOG("Timeout: " << m_monitor_timeout << " seconds");

    // Create raw socket
    int raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (raw_sock < 0) {
        std::cerr << "[Monitor] Failed to create raw socket! (root privileges required)" << std::endl;
        return;
    }

    // Get interface index
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, m_monitor_interface.c_str(), IFNAMSIZ - 1);

    if (ioctl(raw_sock, SIOCGIFINDEX, &ifr) < 0) {
        std::cerr << "[Monitor] Interface not found: " << m_monitor_interface << std::endl;
        ::close(raw_sock);
        return;
    }

    // Bind socket to interface
    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(raw_sock, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        std::cerr << "[Monitor] Failed to bind to interface!" << std::endl;
        ::close(raw_sock);
        return;
    }

    DEBUG_LOG("[Monitor] Listening on " << m_monitor_interface << "...");
    m_monitoring_active = true;

    unsigned char buffer[65536];
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(m_monitor_timeout);

    struct pollfd pfd;
    pfd.fd = raw_sock;
    pfd.events = POLLIN;

    while (!m_stop_requested) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= timeout) {
            DEBUG_LOG("\n[Monitor] Timeout - " << m_monitor_timeout << " seconds elapsed");
            break;
        }

        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed).count();
        int poll_timeout = (remaining_ms > 500) ? 500 : remaining_ms;

        int ret = poll(&pfd, 1, poll_timeout);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = recv(raw_sock, buffer, sizeof(buffer), 0);
            if (len > 0) {
                m_packet_count++;
                printPacketInfo(buffer, len, m_packet_count);
            }
        }
    }

    DEBUG_LOG("\n=== Monitoring Complete ===");
    DEBUG_LOG("Total packets received: " << m_packet_count.load());
    DEBUG_LOG("============================");

    ::close(raw_sock);
    m_monitoring_active = false;
}

int ConfigSender::monitorInterface(const std::string& interface_name, int timeout_seconds) {
    DEBUG_LOG("\n=== Interface Monitoring Starting ===");
    DEBUG_LOG("Interface: " << interface_name);
    DEBUG_LOG("Timeout: " << timeout_seconds << " seconds");

    m_raw_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (m_raw_socket < 0) {
        std::cerr << "[Monitor] Failed to create raw socket! (root privileges required)" << std::endl;
        return -1;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);

    if (ioctl(m_raw_socket, SIOCGIFINDEX, &ifr) < 0) {
        std::cerr << "[Monitor] Interface not found: " << interface_name << std::endl;
        ::close(m_raw_socket);
        m_raw_socket = -1;
        return -1;
    }

    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(m_raw_socket, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        std::cerr << "[Monitor] Failed to bind to interface!" << std::endl;
        ::close(m_raw_socket);
        m_raw_socket = -1;
        return -1;
    }

    DEBUG_LOG("[Monitor] Listening on " << interface_name << "...");

    int packet_count = 0;
    unsigned char buffer[65536];
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(timeout_seconds);

    struct pollfd pfd;
    pfd.fd = m_raw_socket;
    pfd.events = POLLIN;

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= timeout) {
            DEBUG_LOG("[Monitor] Timeout - " << timeout_seconds << " seconds elapsed");
            break;
        }

        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed).count();
        int poll_timeout = (remaining_ms > 1000) ? 1000 : remaining_ms;

        int ret = poll(&pfd, 1, poll_timeout);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = recv(m_raw_socket, buffer, sizeof(buffer), 0);
            if (len > 0) {
                packet_count++;
                printPacketInfo(buffer, len, packet_count);
            }
        }
    }

    DEBUG_LOG("\n=== Monitoring Complete ===");
    DEBUG_LOG("Total packets received: " << packet_count);
    DEBUG_LOG("============================");

    ::close(m_raw_socket);
    m_raw_socket = -1;

    return packet_count;
}
