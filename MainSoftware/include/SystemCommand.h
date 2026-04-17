#ifndef SYSTEM_COMMAND_H
#define SYSTEM_COMMAND_H

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <map>
#include <stdexcept>

/**
 * @brief System command execution result
 */
struct CommandResult {
    int exit_code;          // Command exit code (0 = success)
    std::string output;     // stdout output
    std::string error;      // stderr output
    bool success;           // Was command successful?

    CommandResult() : exit_code(-1), success(false) {}

    explicit operator bool() const { return success; }
};

/**
 * @brief System command exception
 */
class CommandException : public std::runtime_error {
public:
    explicit CommandException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief System command manager class
 *
 * Used to programmatically execute terminal commands (server_on, server_off, etc.).
 * Implements the Singleton pattern.
 *
 * Usage example:
 * @code
 *   auto& cmd = SystemCommandExecutor::getInstance();
 *
 *   // Simple command execution
 *   auto result = cmd.execute("server_on");
 *   if (result.success) {
 *       std::cout << "Server started" << std::endl;
 *   }
 *
 *   // Pre-registered command execution
 *   cmd.registerCommand("start_server", "server_on");
 *   cmd.registerCommand("stop_server", "server_off");
 *   cmd.run("start_server");
 * @endcode
 */
class SystemCommandExecutor {
public:
    /**
     * @brief Returns singleton instance
     */
    static SystemCommandExecutor& getInstance();

    // Singleton - copy and move prohibited
    SystemCommandExecutor(const SystemCommandExecutor&) = delete;
    SystemCommandExecutor& operator=(const SystemCommandExecutor&) = delete;
    SystemCommandExecutor(SystemCommandExecutor&&) = delete;
    SystemCommandExecutor& operator=(SystemCommandExecutor&&) = delete;

    /**
     * @brief Executes raw system command
     * @param command Command to execute (e.g., "server_on", "ls -la")
     * @param timeout_ms Timeout duration (milliseconds), 0 = unlimited
     * @return CommandResult Command result
     */
    CommandResult execute(const std::string& command, int timeout_ms = 0);

    /**
     * @brief Executes a pre-registered command
     * @param command_name Registered command name
     * @return CommandResult Command result
     * @throws CommandException If command not found
     */
    CommandResult run(const std::string& command_name);

    /**
     * @brief Registers a new command
     * @param name Command name (user-friendly)
     * @param command Actual system command
     */
    void registerCommand(const std::string& name, const std::string& command);

    /**
     * @brief Registers multiple commands at once
     * @param commands Command name -> command mapping
     */
    void registerCommands(const std::map<std::string, std::string>& commands);

    /**
     * @brief Checks if registered command exists
     * @param name Command name
     * @return true if command is registered
     */
    bool hasCommand(const std::string& name) const;

    /**
     * @brief Unregisters a command
     * @param name Command name to remove
     */
    void unregisterCommand(const std::string& name);

    /**
     * @brief Clears all registered commands
     */
    void clearCommands();

    /**
     * @brief Returns list of registered commands
     * @return List of command names
     */
    std::vector<std::string> getRegisteredCommands() const;

    /**
     * @brief Sets pre-execute callback
     * @param callback Function to call (command name passed as parameter)
     */
    void setPreExecuteCallback(std::function<void(const std::string&)> callback);

    /**
     * @brief Sets post-execute callback
     * @param callback Function to call (command name and result passed as parameters)
     */
    void setPostExecuteCallback(std::function<void(const std::string&, const CommandResult&)> callback);

    /**
     * @brief Sets working directory
     * @param path New working directory
     */
    void setWorkingDirectory(const std::string& path);

    /**
     * @brief Returns current working directory
     * @return Working directory path
     */
    std::string getWorkingDirectory() const;

private:
    SystemCommandExecutor();
    ~SystemCommandExecutor() = default;

    std::map<std::string, std::string> m_commands;      // Registered commands
    std::string m_working_directory;                     // Working directory

    std::function<void(const std::string&)> m_pre_execute_callback;
    std::function<void(const std::string&, const CommandResult&)> m_post_execute_callback;

    /**
     * @brief Executes command with popen and captures output
     */
    CommandResult executeInternal(const std::string& command);
};

// Global access shortcut
extern SystemCommandExecutor& g_systemCommand;

#endif // SYSTEM_COMMAND_H
