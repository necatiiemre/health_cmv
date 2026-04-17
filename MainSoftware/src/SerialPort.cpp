#include "SerialPort.h"

#include <fcntl.h>       // open(), O_RDWR, O_NOCTTY, O_NONBLOCK
#include <termios.h>     // termios, tcgetattr, tcsetattr, cfsetispeed, cfsetospeed
#include <unistd.h>      // write(), read(), close()
#include <cstring>       // memset, strerror
#include <cerrno>        // errno
#include <sys/select.h>  // select(), fd_set
#include <sys/time.h>    // timeval
#include <sys/ioctl.h>   // ioctl, TIOCMGET, TIOCMSET
#include <linux/serial.h> // serial_struct, TIOCGSERIAL, TIOCSSERIAL
#include <cstdlib>       // system()
#include <iostream>      // std::cout
#include <chrono>        // high_resolution_clock
#include "Utils.h"

// RS-422 port type for setserial
#define SERIAL_TYPE_RS422 0x02

namespace serial {

// ============ SerialPort Class Implementation ============

SerialPort::SerialPort(const std::string& device, int baud_rate)
    : m_device(device)
    , m_baud_rate(baud_rate)
    , m_fd(-1)
    , m_last_error("")
{
}

SerialPort::~SerialPort()
{
    if (isOpen()) {
        close();
    }
}

bool SerialPort::open()
{
    if (isOpen()) {
        m_last_error = "Port already open";
        return false;
    }

    // Configure port as RS-422 using setserial before opening
    std::string setserial_cmd = "setserial " + m_device + " port 0x02 2>/dev/null";
    int ret = system(setserial_cmd.c_str());
    if (ret == 0) {
        DEBUG_LOG("[SerialPort] setserial RS-422 (0x02) configured: " << m_device);
    }

    // Open serial port
    m_fd = ::open(m_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (m_fd < 0) {
        m_last_error = "Failed to open port: " + std::string(strerror(errno));
        return false;
    }

    // Configure port settings
    if (!configurePort()) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    return true;
}

void SerialPort::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool SerialPort::isOpen() const
{
    return m_fd >= 0;
}

bool SerialPort::sendCommand(const std::string& command, bool add_newline)
{
    if (!isOpen()) {
        m_last_error = "Port not open";
        return false;
    }

    std::string data = command;
    if (add_newline) {
        data += "\n";
    }

    ssize_t bytes_written = ::write(m_fd, data.c_str(), data.length());

    if (bytes_written < 0) {
        m_last_error = "Write error: " + std::string(strerror(errno));
        return false;
    }

    if (static_cast<size_t>(bytes_written) != data.length()) {
        m_last_error = "Failed to send all data";
        return false;
    }

    // Ensure data is completely sent
    tcdrain(m_fd);

    return true;
}

std::string SerialPort::readResponse(int timeout_ms)
{
    if (!isOpen()) {
        m_last_error = "Port not open";
        return "";
    }

    std::string response;
    char buffer[256];

    fd_set read_set;
    struct timeval timeout;
    struct timeval start_time, current_time;

    // Get start time for total timeout
    gettimeofday(&start_time, nullptr);

    // Read until newline or timeout
    while (true) {
        // Calculate remaining timeout
        gettimeofday(&current_time, nullptr);
        long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                          (current_time.tv_usec - start_time.tv_usec) / 1000;
        long remaining_ms = timeout_ms - elapsed_ms;

        if (remaining_ms <= 0) {
            if (response.empty()) {
                m_last_error = "Timeout - no response received";
            }
            break;
        }

        // Set timeout for select (use shorter timeout for polling)
        timeout.tv_sec = remaining_ms / 1000;
        timeout.tv_usec = (remaining_ms % 1000) * 1000;

        FD_ZERO(&read_set);
        FD_SET(m_fd, &read_set);

        // Wait for data
        int select_result = select(m_fd + 1, &read_set, nullptr, nullptr, &timeout);

        if (select_result < 0) {
            m_last_error = "Select error: " + std::string(strerror(errno));
            return "";
        }

        if (select_result == 0) {
            // Timeout - check if we have partial data
            if (response.empty()) {
                m_last_error = "Timeout - no response received";
            }
            break;
        }

        // Read available data
        ssize_t bytes_read = ::read(m_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            response += buffer;

            // Check if newline received - complete line
            if (response.find('\n') != std::string::npos) {
                break;
            }
        } else if (bytes_read < 0 && errno != EAGAIN) {
            m_last_error = "Read error: " + std::string(strerror(errno));
            break;
        }
        // If EAGAIN, continue loop to wait for more data
    }

    // Remove trailing newline characters
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
        response.pop_back();
    }

    return response;
}

std::string SerialPort::sendAndReceive(const std::string& command, int timeout_ms)
{
    if (!sendCommand(command)) {
        return "";
    }

    return readResponse(timeout_ms);
}

bool SerialPort::sendRawData(const uint8_t* data, size_t length)
{
    if (!isOpen()) {
        m_last_error = "Port not open";
        return false;
    }

    if (data == nullptr || length == 0) {
        m_last_error = "Invalid data";
        return false;
    }

    // Write all data with retry
    size_t total_written = 0;
    int retry_count = 0;
    const int max_retries = 3;

    while (total_written < length && retry_count < max_retries) {
        ssize_t bytes_written = ::write(m_fd, data + total_written, length - total_written);

        if (bytes_written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking, wait a bit and retry
                usleep(1000);
                retry_count++;
                continue;
            }
            m_last_error = "Write error: " + std::string(strerror(errno));
            return false;
        }

        total_written += bytes_written;
        retry_count = 0;  // Reset retry count on successful write
    }

    if (total_written != length) {
        m_last_error = "Failed to send all data (wrote " + std::to_string(total_written) + "/" + std::to_string(length) + ")";
        return false;
    }

    // Wait for all data to be transmitted
    tcdrain(m_fd);

    return true;
}

bool SerialPort::sendRawDataTimed(const uint8_t* data, size_t length, int64_t& write_us, int64_t& drain_us)
{
    write_us = 0;
    drain_us = 0;

    if (!isOpen()) {
        m_last_error = "Port not open";
        return false;
    }

    if (data == nullptr || length == 0) {
        m_last_error = "Invalid data";
        return false;
    }

    // Measure write() time
    auto write_start = std::chrono::high_resolution_clock::now();

    // Write all data with retry
    size_t total_written = 0;
    int retry_count = 0;
    const int max_retries = 3;

    while (total_written < length && retry_count < max_retries) {
        ssize_t bytes_written = ::write(m_fd, data + total_written, length - total_written);

        if (bytes_written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                retry_count++;
                continue;
            }
            m_last_error = "Write error: " + std::string(strerror(errno));
            return false;
        }

        total_written += bytes_written;
        retry_count = 0;
    }

    auto write_end = std::chrono::high_resolution_clock::now();
    write_us = std::chrono::duration_cast<std::chrono::microseconds>(write_end - write_start).count();

    if (total_written != length) {
        m_last_error = "Failed to send all data (wrote " + std::to_string(total_written) + "/" + std::to_string(length) + ")";
        return false;
    }

    // Measure tcdrain() time
    auto drain_start = std::chrono::high_resolution_clock::now();
    tcdrain(m_fd);
    auto drain_end = std::chrono::high_resolution_clock::now();
    drain_us = std::chrono::duration_cast<std::chrono::microseconds>(drain_end - drain_start).count();

    return true;
}

int SerialPort::readRawData(uint8_t* buffer, size_t max_length, int timeout_ms)
{
    if (!isOpen()) {
        m_last_error = "Port not open";
        return -1;
    }

    if (buffer == nullptr || max_length == 0) {
        m_last_error = "Invalid buffer";
        return -1;
    }

    fd_set read_set;
    struct timeval timeout;
    struct timeval start_time, current_time;
    size_t total_read = 0;

    // Get start time
    gettimeofday(&start_time, nullptr);

    // Try to read as much data as possible within timeout
    while (total_read < max_length) {
        // Calculate remaining timeout
        gettimeofday(&current_time, nullptr);
        long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                          (current_time.tv_usec - start_time.tv_usec) / 1000;
        long remaining_ms = timeout_ms - elapsed_ms;

        if (remaining_ms <= 0) {
            break;  // Timeout reached
        }

        timeout.tv_sec = remaining_ms / 1000;
        timeout.tv_usec = (remaining_ms % 1000) * 1000;

        FD_ZERO(&read_set);
        FD_SET(m_fd, &read_set);

        int select_result = select(m_fd + 1, &read_set, nullptr, nullptr, &timeout);

        if (select_result < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted, try again
            }
            m_last_error = "Select error: " + std::string(strerror(errno));
            return -1;
        }

        if (select_result == 0) {
            break;  // Timeout - no more data
        }

        ssize_t bytes_read = ::read(m_fd, buffer + total_read, max_length - total_read);

        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // No data available right now
            }
            if (errno == EINTR) {
                continue;  // Interrupted, try again
            }
            m_last_error = "Read error: " + std::string(strerror(errno));
            return -1;
        }

        if (bytes_read == 0) {
            break;  // EOF or no more data
        }

        total_read += bytes_read;

        // If we got some data, wait a bit more for additional data
        if (total_read > 0 && remaining_ms > 50) {
            usleep(10000);  // 10ms wait for more data
        }
    }

    return static_cast<int>(total_read);
}

std::string SerialPort::getLastError() const
{
    return m_last_error;
}

int SerialPort::getBaudRateConstant(int baud_rate)
{
    switch (baud_rate) {
        case 300:    return B300;
        case 600:    return B600;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B9600;
    }
}

bool SerialPort::configurePort()
{
    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    // Get current settings
    if (tcgetattr(m_fd, &tty) != 0) {
        m_last_error = "Failed to get port settings: " + std::string(strerror(errno));
        return false;
    }

    // Set baud rate
    int baud_constant = getBaudRateConstant(m_baud_rate);
    cfsetispeed(&tty, baud_constant);
    cfsetospeed(&tty, baud_constant);

    // 8N1 (8 data bits, no parity, 1 stop bit)
    tty.c_cflag &= ~PARENB;        // No parity
    tty.c_cflag &= ~CSTOPB;        // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 data bits

    // Hardware flow control disabled
    tty.c_cflag &= ~CRTSCTS;

    // Local mode, read enable
    tty.c_cflag |= CREAD | CLOCAL;

    // Raw input mode
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);

    // Software flow control disabled
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // Raw output mode
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;

    // Read settings
    tty.c_cc[VTIME] = 10;  // 1 second timeout
    tty.c_cc[VMIN] = 0;    // Non-blocking read

    // Apply settings
    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        m_last_error = "Failed to apply port settings: " + std::string(strerror(errno));
        return false;
    }

    // Flush buffers
    tcflush(m_fd, TCIOFLUSH);

    // Set DTR and RTS signals (some USB adapters need this)
    int modem_flags;
    if (ioctl(m_fd, TIOCMGET, &modem_flags) == 0) {
        modem_flags |= TIOCM_DTR | TIOCM_RTS;
        ioctl(m_fd, TIOCMSET, &modem_flags);
    }

    // Configure serial port type using ioctl (RS-422 = 0x02)
    struct serial_struct serial_info;
    if (ioctl(m_fd, TIOCGSERIAL, &serial_info) == 0) {
        serial_info.type = SERIAL_TYPE_RS422;
        serial_info.flags |= ASYNC_LOW_LATENCY;  // Low latency mode
        ioctl(m_fd, TIOCSSERIAL, &serial_info);
    }

    return true;
}

// ============ Simple Functions ============

bool sendSerialCommand(const std::string& device,
                       const std::string& command,
                       int baud_rate)
{
    SerialPort port(device, baud_rate);

    if (!port.open()) {
        return false;
    }

    bool result = port.sendCommand(command);
    port.close();

    return result;
}

std::string sendSerialCommandWithResponse(const std::string& device,
                                          const std::string& command,
                                          int baud_rate,
                                          int timeout_ms)
{
    SerialPort port(device, baud_rate);

    if (!port.open()) {
        return "";
    }

    std::string response = port.sendAndReceive(command, timeout_ms);
    port.close();

    return response;
}

} // namespace serial
