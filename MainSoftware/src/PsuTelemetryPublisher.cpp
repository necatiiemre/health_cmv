#include "PsuTelemetryPublisher.h"
#include "PsuTelemetry.h"
#include "DeviceManager.h"
#include "Utils.h"
#include "ErrorPrinter.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <utility>

namespace {

// Resolve "host" into a sockaddr_in; accepts both dotted-IP and DNS names.
// On success fills *out and returns true.
bool resolveHost(const std::string& host, uint16_t port, sockaddr_in* out) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);

    // Fast path: dotted IP literal.
    if (inet_pton(AF_INET, host.c_str(), &out->sin_addr) == 1) {
        return true;
    }

    // Fallback: getaddrinfo for hostnames.
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    auto* a = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out->sin_addr = a->sin_addr;
    freeaddrinfo(res);
    return true;
}

uint64_t nowUnixNs() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

PsuTelemetryPublisher::PsuTelemetryPublisher(Device device,
                                             std::string dst_host,
                                             uint16_t dst_port,
                                             uint32_t interval_ms)
    : m_device(device),
      m_dst_host(std::move(dst_host)),
      m_dst_port(dst_port),
      m_interval_ms(interval_ms == 0 ? 1000 : interval_ms),
      m_sockfd(-1),
      m_running(false),
      m_packets_sent(0),
      m_errors(0),
      m_seq(0) {
    std::memset(&m_dst_addr, 0, sizeof(m_dst_addr));
}

PsuTelemetryPublisher::~PsuTelemetryPublisher() {
    stop();
}

bool PsuTelemetryPublisher::start() {
    if (m_running.load()) return true;

    if (!resolveHost(m_dst_host, m_dst_port, &m_dst_addr)) {
        ErrorPrinter::warn("PSU-TELEM",
            "Failed to resolve destination host: " + m_dst_host);
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ErrorPrinter::warn("PSU-TELEM",
            std::string("socket() failed: ") + std::strerror(errno));
        return false;
    }

    // NOTE: We intentionally do NOT connect() this socket. On a connected
    // UDP socket the kernel remembers the last ICMP error (e.g. host
    // unreachable during a transient ARP failure) and reports it on the
    // NEXT send() as EHOSTUNREACH/ECONNREFUSED - which then keeps firing
    // until the ICMP error cache expires, wedging publishing for minutes.
    // With an unconnected socket + sendto() each send makes its own
    // routing/ARP decision and a transient failure only drops one packet.

    m_sockfd = fd;
    m_running.store(true);
    m_thread = std::thread(&PsuTelemetryPublisher::run, this);

    // Always-visible (not DEBUG_LOG) so we can confirm start/host/port from
    // the console without rebuilding with DEBUG_PRINT=1.
    {
        char dotted[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &m_dst_addr.sin_addr, dotted, sizeof(dotted));
        ErrorPrinter::info("PSU-TELEM",
            std::string("publisher started -> ") + dotted + ":" +
            std::to_string(m_dst_port) + " every " +
            std::to_string(m_interval_ms) + "ms (unconnected sendto)");
    }
    return true;
}

void PsuTelemetryPublisher::stop() {
    if (!m_running.exchange(false)) {
        // Not running (or already stopped). Still clean up socket in case
        // start() partially succeeded.
        if (m_sockfd >= 0) {
            ::close(m_sockfd);
            m_sockfd = -1;
        }
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_sockfd >= 0) {
        ::close(m_sockfd);
        m_sockfd = -1;
    }
    ErrorPrinter::info("PSU-TELEM",
        "publisher stopped. sent=" + std::to_string(m_packets_sent.load()) +
        " errors=" + std::to_string(m_errors.load()));
}

void PsuTelemetryPublisher::run() {
    using clock = std::chrono::steady_clock;

    // Consecutive SCPI failures before we warn loudly (still keep running).
    int consec_errors = 0;

    while (m_running.load(std::memory_order_relaxed)) {
        auto tick_start = clock::now();

        psu_telem_pkt_t pkt{};
        pkt.magic      = PSU_TELEM_MAGIC;
        pkt.version    = PSU_TELEM_VERSION;
        pkt.seq        = ++m_seq;
        pkt.ts_unix_ns = nowUnixNs();
        pkt.model      = (m_device == PSUG30) ? PSU_TELEM_MODEL_PSU30
                                              : PSU_TELEM_MODEL_PSU300;

        bool ok = true;

        try {
            // Measured values.
            double v = g_DeviceManager.measureVoltage(m_device);
            double i = g_DeviceManager.measureCurrent(m_device);
            if (v < 0.0 || i < 0.0) {
                // DeviceManager returns -1.0 on internal failure.
                ok = false;
            } else {
                pkt.voltage_v = static_cast<float>(v);
                pkt.current_a = static_cast<float>(i);
                pkt.power_w   = static_cast<float>(v * i);
            }

            // Setpoints (cheap, low-churn - OK to query every tick).
            double vs = g_DeviceManager.getVoltage(m_device);
            double is = g_DeviceManager.getCurrent(m_device);
            if (vs >= 0.0) pkt.set_voltage_v = static_cast<float>(vs);
            if (is >= 0.0) pkt.set_current_a = static_cast<float>(is);

            // Output state.
            if (g_DeviceManager.isOutputEnabled(m_device)) {
                pkt.flags |= PSU_TELEM_FLAG_OUTPUT_ON;
            }
        } catch (...) {
            // DeviceManager wrappers already swallow exceptions, but be
            // defensive: the publisher thread must never throw upward.
            ok = false;
        }

        if (!ok) {
            pkt.flags |= PSU_TELEM_FLAG_PSU_ERROR;
            consec_errors++;
            if (consec_errors == 5) {
                ErrorPrinter::warn("PSU-TELEM",
                    "PSU query failing for 5 consecutive ticks - "
                    "continuing, values will show as error flagged.");
            }
        } else {
            if (consec_errors > 0) {
                DEBUG_LOG("PSU-TELEM: PSU recovered after "
                          << consec_errors << " failed ticks");
                pkt.flags |= PSU_TELEM_FLAG_RECONNECTED;
            }
            consec_errors = 0;
        }

        // sendto() (not send()) on an unconnected socket. Non-blocking and
        // best-effort - UDP loss is acceptable, the receiver handles
        // staleness via the seq + timestamp fields.
        if (m_sockfd >= 0) {
            ssize_t n = ::sendto(m_sockfd, &pkt, sizeof(pkt), MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr*>(&m_dst_addr),
                                 sizeof(m_dst_addr));
            if (n == static_cast<ssize_t>(sizeof(pkt))) {
                uint32_t before = m_packets_sent.fetch_add(1, std::memory_order_relaxed);
                // Announce first successful send so the operator knows the
                // pipe is actually live without needing to tcpdump.
                if (before == 0) {
                    ErrorPrinter::info("PSU-TELEM",
                        "first packet sent successfully (" +
                        std::to_string(sizeof(pkt)) + " bytes)");
                }
            } else {
                int saved_errno = errno;
                uint32_t e = m_errors.fetch_add(1, std::memory_order_relaxed) + 1;

                // Log the first few errors to surface the root cause; after
                // that log one summary line per minute so a 2h test doesn't
                // spam the console but operator still knows it's ongoing.
                if (e <= 3 || (e % 60 == 0)) {
                    ErrorPrinter::warn("PSU-TELEM",
                        std::string("sendto failed (errno=") +
                        std::to_string(saved_errno) + " " +
                        std::strerror(saved_errno) +
                        ", total_errors=" + std::to_string(e) + ")");
                }

                // Transient routing/ARP failures (EHOSTUNREACH, ENETUNREACH)
                // can pin a previously-learned bad state on the socket;
                // recreate it so the next sendto() starts fresh.
                if (saved_errno == EHOSTUNREACH || saved_errno == ENETUNREACH ||
                    saved_errno == ENOTCONN || saved_errno == EPIPE) {
                    ::close(m_sockfd);
                    int nfd = ::socket(AF_INET, SOCK_DGRAM, 0);
                    if (nfd >= 0) {
                        m_sockfd = nfd;
                    } else {
                        m_sockfd = -1;
                        ErrorPrinter::warn("PSU-TELEM",
                            std::string("socket re-create failed: ") +
                            std::strerror(errno));
                    }
                }
            }
        }

        // Sleep the remainder of the tick without oversleeping if a query
        // took a long time (e.g., SCPI timeout consumed most of the second).
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           clock::now() - tick_start).count();
        auto remaining_ms = static_cast<int64_t>(m_interval_ms) - elapsed;
        if (remaining_ms > 0) {
            // Break up the sleep so stop() is responsive within ~100 ms.
            const int64_t slice = 100;
            while (remaining_ms > 0 && m_running.load(std::memory_order_relaxed)) {
                auto chunk = remaining_ms > slice ? slice : remaining_ms;
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                remaining_ms -= chunk;
            }
        }
    }
}