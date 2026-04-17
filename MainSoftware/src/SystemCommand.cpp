#include "SystemCommand.h"
#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

// Global instance
SystemCommandExecutor& g_systemCommand = SystemCommandExecutor::getInstance();

SystemCommandExecutor& SystemCommandExecutor::getInstance() {
    static SystemCommandExecutor instance;
    return instance;
}

SystemCommandExecutor::SystemCommandExecutor()
    : m_working_directory("")
    , m_pre_execute_callback(nullptr)
    , m_post_execute_callback(nullptr) {
}

CommandResult SystemCommandExecutor::execute(const std::string& command, int timeout_ms) {
    (void)timeout_ms; // Timeout not supported yet, may be added later

    if (m_pre_execute_callback) {
        m_pre_execute_callback(command);
    }

    CommandResult result = executeInternal(command);

    if (m_post_execute_callback) {
        m_post_execute_callback(command, result);
    }

    return result;
}

CommandResult SystemCommandExecutor::run(const std::string& command_name) {
    auto it = m_commands.find(command_name);
    if (it == m_commands.end()) {
        throw CommandException("Command not found: " + command_name);
    }
    return execute(it->second);
}

void SystemCommandExecutor::registerCommand(const std::string& name, const std::string& command) {
    m_commands[name] = command;
}

void SystemCommandExecutor::registerCommands(const std::map<std::string, std::string>& commands) {
    for (const auto& pair : commands) {
        m_commands[pair.first] = pair.second;
    }
}

bool SystemCommandExecutor::hasCommand(const std::string& name) const {
    return m_commands.find(name) != m_commands.end();
}

void SystemCommandExecutor::unregisterCommand(const std::string& name) {
    m_commands.erase(name);
}

void SystemCommandExecutor::clearCommands() {
    m_commands.clear();
}

std::vector<std::string> SystemCommandExecutor::getRegisteredCommands() const {
    std::vector<std::string> result;
    result.reserve(m_commands.size());
    for (const auto& pair : m_commands) {
        result.push_back(pair.first);
    }
    return result;
}

void SystemCommandExecutor::setPreExecuteCallback(std::function<void(const std::string&)> callback) {
    m_pre_execute_callback = std::move(callback);
}

void SystemCommandExecutor::setPostExecuteCallback(
    std::function<void(const std::string&, const CommandResult&)> callback) {
    m_post_execute_callback = std::move(callback);
}

void SystemCommandExecutor::setWorkingDirectory(const std::string& path) {
    m_working_directory = path;
}

std::string SystemCommandExecutor::getWorkingDirectory() const {
    return m_working_directory;
}

CommandResult SystemCommandExecutor::executeInternal(const std::string& command) {
    CommandResult result;
    std::string full_command = command;

    // If working directory is specified, execute command in that directory
    if (!m_working_directory.empty()) {
        full_command = "cd \"" + m_working_directory + "\" && " + command;
    }

    // Add 2>&1 to capture stderr as well
    full_command += " 2>&1";

    // Execute command with popen
    std::array<char, 4096> buffer;
    std::stringstream output_stream;

    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        result.error = "Failed to execute command: popen error";
        result.exit_code = -1;
        result.success = false;
        return result;
    }

    // Read output
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output_stream << buffer.data();
    }

    // Get command exit code
    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = -1;
    }

    result.output = output_stream.str();
    result.success = (result.exit_code == 0);

    return result;
}
