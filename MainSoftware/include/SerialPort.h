#ifndef SERIAL_H
#define SERIAL_H

#include <string>
#include <cstdint>

namespace serial {

/**
 * @brief Class for serial port communication
 *
 * Usage example:
 *   serial::SerialPort port("/dev/ttyACM0", 9600);
 *   if (port.open()) {
 *       port.sendCommand("ID 1");
 *       std::string response = port.readResponse();
 *       port.close();
 *   }
 */
class SerialPort {
public:
    /**
     * @brief Constructor
     * @param device Serial port device (e.g., "/dev/ttyACM0")
     * @param baudRate Baud rate (default: 9600)
     */
    SerialPort(const std::string& device, int baud_rate = 9600);

    /**
     * @brief Destructor - automatically closes port
     */
    ~SerialPort();

    /**
     * @brief Opens serial port
     * @return true on success
     */
    bool open();

    /**
     * @brief Closes serial port
     */
    void close();

    /**
     * @brief Checks if port is open
     * @return true if open
     */
    bool isOpen() const;

    /**
     * @brief Sends command to serial port
     * @param command Command to send
     * @param addNewline Add newline at end of command (default: true)
     * @return true on success
     */
    bool sendCommand(const std::string& command, bool addNewline = true);

    /**
     * @brief Reads response from serial port
     * @param timeoutMs Timeout in milliseconds (default: 1000ms)
     * @return Response read
     */
    std::string readResponse(int timeout_ms = 1000);

    /**
     * @brief Sends command and waits for response
     * @param command Command to send
     * @param timeoutMs Timeout for response
     * @return Response received
     */
    std::string sendAndReceive(const std::string& command, int timeout_ms = 1000);

    /**
     * @brief Sends raw binary data to serial port
     * @param data Pointer to data buffer
     * @param length Number of bytes to send
     * @return true on success
     */
    bool sendRawData(const uint8_t* data, size_t length);

    /**
     * @brief Sends raw binary data with detailed timing measurement
     * @param data Pointer to data buffer
     * @param length Number of bytes to send
     * @param write_us Output: time for write() syscall in microseconds
     * @param drain_us Output: time for tcdrain() in microseconds
     * @return true on success
     */
    bool sendRawDataTimed(const uint8_t* data, size_t length, int64_t& write_us, int64_t& drain_us);

    /**
     * @brief Reads raw binary data from serial port
     * @param buffer Buffer to store data
     * @param max_length Maximum bytes to read
     * @param timeout_ms Timeout in milliseconds
     * @return Number of bytes read, or -1 on error
     */
    int readRawData(uint8_t* buffer, size_t max_length, int timeout_ms = 1000);

    /**
     * @brief Returns last error message
     * @return Error message
     */
    std::string getLastError() const;

private:
    std::string m_device;
    int m_baud_rate;
    int m_fd;  // File descriptor
    std::string m_last_error;

    /**
     * @brief Converts baud rate value to termios constant
     */
    int getBaudRateConstant(int baud_rate);

    /**
     * @brief Configures serial port settings
     */
    bool configurePort();
};

// ============ Simple functions (without using class) ============

/**
 * @brief Sends one-time command
 * @param device Serial port device (e.g., "/dev/ttyACM0")
 * @param command Command to send
 * @param baudRate Baud rate (default: 9600)
 * @return true on success
 */
bool sendSerialCommand(const std::string& device,
                       const std::string& command,
                       int baud_rate = 9600);

/**
 * @brief Sends command and gets response
 * @param device Serial port device
 * @param command Command to send
 * @param baud_rate Baud rate
 * @param timeout_ms Timeout duration
 * @return Response received (empty string on error)
 */
std::string sendSerialCommandWithResponse(const std::string& device,
                                          const std::string& command,
                                          int baud_rate = 9600,
                                          int timeout_ms = 1000);

} // namespace serial

#endif // SERIAL_H
