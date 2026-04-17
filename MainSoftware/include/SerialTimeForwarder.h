#ifndef SERIAL_TIME_FORWARDER_H
#define SERIAL_TIME_FORWARDER_H

#include "SerialPort.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace serial {

/**
 * @brief Forwards time data from MicroChip SyncServer to another serial port
 *
 * Reads time data from input port (format: YYYY DDD HH:MM:SS DZZ),
 * converts to Unix timestamp, and sends as binary packet to output port.
 *
 * Runs in a separate thread, non-blocking to main application.
 *
 * Usage:
 *   SerialTimeForwarder forwarder("/dev/ttyUSB0", "/dev/ttyUSB1");
 *   forwarder.start();
 *   // ... main application runs ...
 *   forwarder.stop();
 */
class SerialTimeForwarder {
public:
    /**
     * @brief Constructor
     * @param input_device  Input serial port (default: /dev/ttyUSB0, 9600 baud)
     * @param output_device Output serial port (default: /dev/ttyUSB1, 38400 baud)
     * @param verify_device Verification port to read back sent data (optional, empty = disabled)
     */
    SerialTimeForwarder(const std::string& input_device = "/dev/ttyUSB0",
                        const std::string& output_device = "/dev/ttyUSB1",
                        const std::string& verify_device = "");

    /**
     * @brief Destructor - stops thread if running
     */
    ~SerialTimeForwarder();

    // Non-copyable
    SerialTimeForwarder(const SerialTimeForwarder&) = delete;
    SerialTimeForwarder& operator=(const SerialTimeForwarder&) = delete;

    /**
     * @brief Starts the forwarder thread
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stops the forwarder thread
     */
    void stop();

    /**
     * @brief Checks if forwarder is running
     */
    bool isRunning() const { return m_running.load(); }

    /**
     * @brief Returns number of packets sent
     */
    uint64_t getPacketsSent() const { return m_packets_sent.load(); }

    /**
     * @brief Returns last Unix timestamp sent
     */
    uint32_t getLastTimestamp() const { return m_last_timestamp.load(); }

    /**
     * @brief Returns last received time string from SyncServer
     */
    std::string getLastTimeString() const;

    /**
     * @brief Returns last error message
     */
    std::string getLastError() const;

private:
    // Serial ports
    SerialPort m_input_port;
    SerialPort m_output_port;
    SerialPort m_verify_port;

    // Thread control
    std::thread m_worker_thread;
    std::thread m_verify_thread;
    std::atomic<bool> m_running{false};
    bool m_verify_enabled{false};  // True if verify device is specified

    // Statistics
    std::atomic<uint64_t> m_packets_sent{0};
    std::atomic<uint32_t> m_last_timestamp{0};

    // Timing for end-to-end latency measurement
    std::atomic<int64_t> m_send_time_us{0};  // Time when packet was sent (microseconds since epoch)

    // Error tracking
    mutable std::mutex m_error_mutex;
    std::string m_last_error;

    // Last received time string
    mutable std::mutex m_time_string_mutex;
    std::string m_last_time_string;

    // Packet constants
    static constexpr size_t PACKET_SIZE = 20;
    static constexpr size_t TS_OFFSET = 8;

    // Packet template (header + placeholder + tail)
    static constexpr uint8_t PACKET_HEADER[] = {0xCA, 0xE1, 0x10, 0x44,0x01, 0x02, 0x03, 0x04};
    static constexpr uint8_t PACKET_TAIL[] = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

    /**
     * @brief Worker thread main loop
     */
    void workerLoop();

    /**
     * @brief Verification thread - reads packets from verify port
     */
    void verifyLoop();

    /**
     * @brief Parses time string and converts to Unix timestamp
     * @param time_str Time string in format "YYYY DDD HH:MM:SS"
     * @return Unix timestamp, or 0 on parse error
     */
    uint32_t parseTimeString(const std::string& time_str);

    /**
     * @brief Converts day of year to month and day
     * @param year Year (for leap year calculation)
     * @param doy Day of year (1-366)
     * @param month Output: month (1-12)
     * @param day Output: day of month (1-31)
     * @return true if valid
     */
    bool dayOfYearToDate(int year, int doy, int& month, int& day);

    /**
     * @brief Checks if year is a leap year
     */
    bool isLeapYear(int year);

    /**
     * @brief Builds output packet with timestamp
     * @param buffer Output buffer (must be PACKET_SIZE bytes)
     * @param timestamp Unix timestamp (big endian)
     */
    void buildPacket(uint8_t* buffer, uint32_t timestamp);

    /**
     * @brief Sets last error message
     */
    void setLastError(const std::string& error);
};

} // namespace serial

#endif // SERIAL_TIME_FORWARDER_H
