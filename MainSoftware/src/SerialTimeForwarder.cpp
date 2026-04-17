#include "SerialTimeForwarder.h"
#include "Utils.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace serial {

// Static constexpr definitions
constexpr uint8_t SerialTimeForwarder::PACKET_HEADER[];
constexpr uint8_t SerialTimeForwarder::PACKET_TAIL[];

SerialTimeForwarder::SerialTimeForwarder(const std::string& input_device,
                                         const std::string& output_device,
                                         const std::string& verify_device)
    : m_input_port(input_device, 9600)
    , m_output_port(output_device, 38400)
    , m_verify_port(verify_device.empty() ? "/dev/null" : verify_device, 38400)
    , m_verify_enabled(!verify_device.empty())
{
}

SerialTimeForwarder::~SerialTimeForwarder()
{
    stop();
}

bool SerialTimeForwarder::start()
{
    if (m_running.load()) {
        setLastError("Forwarder already running");
        return false;
    }

    // Open input port
    if (!m_input_port.open()) {
        setLastError("Failed to open input port: " + m_input_port.getLastError());
        return false;
    }

    // Open output port
    if (!m_output_port.open()) {
        m_input_port.close();
        setLastError("Failed to open output port: " + m_output_port.getLastError());
        return false;
    }

    // Open verify port (only if enabled)
    if (m_verify_enabled) {
        if (!m_verify_port.open()) {
            m_input_port.close();
            m_output_port.close();
            setLastError("Failed to open verify port: " + m_verify_port.getLastError());
            return false;
        }
    }

    DEBUG_LOG("[TimeForwarder] Ports opened successfully");
    DEBUG_LOG("[TimeForwarder] Input port: 9600 baud");
    DEBUG_LOG("[TimeForwarder] Output port: 38400 baud");
#if DEBUG_PRINT
    if (m_verify_enabled) {
        std::cout << "[TimeForwarder] Verify port: 38400 baud (enabled)" << std::endl;
    } else {
        std::cout << "[TimeForwarder] Verify: disabled" << std::endl;
    }
#endif

    m_running.store(true);
    m_worker_thread = std::thread(&SerialTimeForwarder::workerLoop, this);

    // Start verify thread only if enabled
    if (m_verify_enabled) {
        m_verify_thread = std::thread(&SerialTimeForwarder::verifyLoop, this);
    }

    return true;
}

void SerialTimeForwarder::stop()
{
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }

    if (m_verify_enabled && m_verify_thread.joinable()) {
        m_verify_thread.join();
    }

    m_input_port.close();
    m_output_port.close();
    if (m_verify_enabled) {
        m_verify_port.close();
    }
}

std::string SerialTimeForwarder::getLastError() const
{
    std::lock_guard<std::mutex> lock(m_error_mutex);
    return m_last_error;
}

std::string SerialTimeForwarder::getLastTimeString() const
{
    std::lock_guard<std::mutex> lock(m_time_string_mutex);
    return m_last_time_string;
}

void SerialTimeForwarder::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_error_mutex);
    m_last_error = error;
}

void SerialTimeForwarder::workerLoop()
{
    uint8_t packet[PACKET_SIZE];

    DEBUG_LOG("[TimeForwarder] Worker thread started");

    while (m_running.load()) {
        // Read line from input port (2 second timeout)
        std::string line = m_input_port.readResponse(2000);

        // Record time immediately after receiving data (last byte from USB0)
        auto recv_time = std::chrono::high_resolution_clock::now();

        if (line.empty()) {
            continue;
        }

        // Trim leading whitespace (CR, LF, spaces)
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            line = line.substr(start);
        }

        // Trim trailing whitespace
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            line = line.substr(0, end + 1);
        }

        // Check if line starts with digit (valid time data)
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]))) {
            continue;
        }

        // Store raw time string
        {
            std::lock_guard<std::mutex> lock(m_time_string_mutex);
            m_last_time_string = line;
        }

        // Parse time string to Unix timestamp
        uint32_t timestamp = parseTimeString(line);

        if (timestamp == 0) {
            continue;
        }

        // Build packet
        buildPacket(packet, timestamp);

        // Record time just before sending
        auto send_start = std::chrono::high_resolution_clock::now();

        // Send to output port with detailed timing
        int64_t write_us = 0, drain_us = 0;
        if (m_output_port.sendRawDataTimed(packet, PACKET_SIZE, write_us, drain_us)) {
            // Record time after sending completes
            auto send_end = std::chrono::high_resolution_clock::now();

            // Store send time for end-to-end latency measurement (verify thread will use this)
            auto send_time_since_epoch = std::chrono::duration_cast<std::chrono::microseconds>(
                send_end.time_since_epoch()).count();
            m_send_time_us.store(send_time_since_epoch);

            // Calculate latencies
            auto process_us = std::chrono::duration_cast<std::chrono::microseconds>(send_start - recv_time).count();
            auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(send_end - recv_time).count();

            m_packets_sent.fetch_add(1);
            m_last_timestamp.store(timestamp);

            // // Print packet in hex
            // std::cout << "[TimeForwarder] ========== TX PACKET ==========" << std::endl;
            // std::cout << "[TimeForwarder] TX Data: ";
            // for (size_t i = 0; i < PACKET_SIZE; i++) {
            //     std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
            //               << static_cast<int>(packet[i]) << " ";
            // }
            // std::cout << std::dec << std::endl;

            // // Print timing breakdown
            // std::cout << "[TimeForwarder] TX Timing (USB0 recv -> USB1 send):" << std::endl;
            // std::cout << "  [1] Process (parse+build): " << process_us << " us" << std::endl;
            // std::cout << "  [2] Write (to kernel):     " << write_us << " us" << std::endl;
            // std::cout << "  [3] Drain (to hardware):   " << drain_us << " us" << std::endl;
            // std::cout << "  [=] Total TX latency:      " << total_us << " us" << std::endl;
        } else {
            DEBUG_LOG("[TimeForwarder] Send failed: " << m_output_port.getLastError());
        }
    }

    DEBUG_LOG("[TimeForwarder] Worker thread stopped");
}

void SerialTimeForwarder::verifyLoop()
{
    uint8_t buffer[64];
    int timeout_count = 0;

    DEBUG_LOG("[TimeForwarder] Verify thread started (reading from USB2)");
    DEBUG_LOG("[TimeForwarder] Verify port open: " << (m_verify_port.isOpen() ? "YES" : "NO"));

    while (m_running.load()) {
        // Read raw bytes from verify port
        int bytes_read = m_verify_port.readRawData(buffer, sizeof(buffer), 1000);

        // Record time immediately when data is received (first byte from USB2)
        auto recv_time = std::chrono::high_resolution_clock::now();
        auto recv_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            recv_time.time_since_epoch()).count();

        if (bytes_read < 0) {
            DEBUG_LOG("[TimeForwarder] USB2 Read error: " << m_verify_port.getLastError());
            continue;
        }

        if (bytes_read == 0) {
            timeout_count++;
            // Print every 5 timeouts to show the thread is alive
            if (timeout_count % 5 == 0) {
                DEBUG_LOG("[TimeForwarder] USB2 waiting... (timeouts: " << timeout_count << ")");
            }
            continue;
        }

        // Reset timeout counter when we get data
        timeout_count = 0;

        // Get the send time from worker thread
        int64_t send_time_us = m_send_time_us.load();

        // Calculate end-to-end latency (USB1 send -> USB2 receive)
        int64_t wire_latency_us = recv_time_us - send_time_us;

#if DEBUG_PRINT
        // Print detailed RX info
        std::cout << "[TimeForwarder] ========== RX PACKET (USB2) ==========" << std::endl;
        std::cout << "[TimeForwarder] RX Data (" << bytes_read << " bytes): ";
        for (int i = 0; i < bytes_read; i++) {
            std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                      << static_cast<int>(buffer[i]) << " ";
        }
        std::cout << std::dec << std::endl;

        // If we got enough bytes, try to decode timestamp
        if (bytes_read >= static_cast<int>(PACKET_SIZE)) {
            // Extract timestamp (bytes 7-10, big endian)
            uint32_t ts = (static_cast<uint32_t>(buffer[7]) << 24) |
                          (static_cast<uint32_t>(buffer[8]) << 16) |
                          (static_cast<uint32_t>(buffer[9]) << 8) |
                          static_cast<uint32_t>(buffer[10]);
            std::cout << "[TimeForwarder] RX Decoded timestamp: " << ts << std::endl;
        }

        // Print wire latency
        std::cout << "[TimeForwarder] RX Timing (USB1 send -> USB2 first byte):" << std::endl;
        std::cout << "  [4] Wire latency:          " << wire_latency_us << " us";
        if (wire_latency_us > 1000) {
            std::cout << " (" << (wire_latency_us / 1000.0) << " ms)";
        }
        std::cout << std::endl;

        // Calculate theoretical transmission time
        // 38400 baud, 8N1 = 3840 bytes/sec = 260.4 us/byte
        double theoretical_us = bytes_read * (1000000.0 / 3840.0);
        std::cout << "  [*] Theoretical TX time:   " << static_cast<int64_t>(theoretical_us) << " us ("
                  << bytes_read << " bytes @ 38400 baud)" << std::endl;

        // Overhead = actual - theoretical
        int64_t overhead_us = wire_latency_us - static_cast<int64_t>(theoretical_us);
        std::cout << "  [*] USB/Buffer overhead:   " << overhead_us << " us" << std::endl;
        std::cout << "[TimeForwarder] ======================================" << std::endl;
#endif
    }

    DEBUG_LOG("[TimeForwarder] Verify thread stopped");
}

uint32_t SerialTimeForwarder::parseTimeString(const std::string& time_str)
{
    // Format: "YYYY DDD HH:MM:SS DZZ" or "YYYY DDD HH:MM:SS"
    // Example: "2025 365 23:59:59 +00"

    std::istringstream iss(time_str);

    int year, doy;
    char colon1, colon2;
    int hour, minute, second;

    // Parse year and day of year
    if (!(iss >> year >> doy)) {
        return 0;
    }

    // Parse time (HH:MM:SS)
    if (!(iss >> hour >> colon1 >> minute >> colon2 >> second)) {
        // Try alternative format without spaces
        std::string time_part;
        iss.clear();
        iss.str(time_str);
        iss >> year >> doy >> time_part;

        if (time_part.length() >= 8) {
            // Parse HH:MM:SS
            if (sscanf(time_part.c_str(), "%d:%d:%d", &hour, &minute, &second) != 3) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    // Validate ranges
    if (year < 1970 || year > 2100) return 0;
    if (doy < 1 || doy > 366) return 0;
    if (hour < 0 || hour > 23) return 0;
    if (minute < 0 || minute > 59) return 0;
    if (second < 0 || second > 59) return 0;

    // Convert day of year to month/day
    int month, day;
    if (!dayOfYearToDate(year, doy, month, day)) {
        return 0;
    }

    // Build struct tm
    struct tm tm_time;
    memset(&tm_time, 0, sizeof(tm_time));
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = month - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = minute;
    tm_time.tm_sec = second;

    // Convert to Unix timestamp (UTC)
    time_t unix_time = timegm(&tm_time);

    if (unix_time == -1) {
        return 0;
    }

    return static_cast<uint32_t>(unix_time);
}

bool SerialTimeForwarder::isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

bool SerialTimeForwarder::dayOfYearToDate(int year, int doy, int& month, int& day)
{
    // Days in each month (non-leap year)
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    bool leap = isLeapYear(year);
    int max_doy = leap ? 366 : 365;

    if (doy < 1 || doy > max_doy) {
        return false;
    }

    int remaining = doy;

    for (int m = 0; m < 12; m++) {
        int days = days_in_month[m];

        // February adjustment for leap year
        if (m == 1 && leap) {
            days = 29;
        }

        if (remaining <= days) {
            month = m + 1;
            day = remaining;
            return true;
        }

        remaining -= days;
    }

    return false;
}

void SerialTimeForwarder::buildPacket(uint8_t* buffer, uint32_t timestamp)
{
    // Copy header
    memcpy(buffer, PACKET_HEADER, sizeof(PACKET_HEADER));

    // Insert timestamp (Big Endian)
    buffer[TS_OFFSET + 0] = (timestamp >> 24) & 0xFF;
    buffer[TS_OFFSET + 1] = (timestamp >> 16) & 0xFF;
    buffer[TS_OFFSET + 2] = (timestamp >> 8) & 0xFF;
    buffer[TS_OFFSET + 3] = timestamp & 0xFF;

    // Copy tail
    memcpy(buffer + TS_OFFSET + 4, PACKET_TAIL, sizeof(PACKET_TAIL));
}

} // namespace serial
