#include "SSHDeployer.h"
#include "SystemCommand.h"
#include "Utils.h"
#include <iostream>
#include <filesystem>
#include <unistd.h>

// ==================== Global Instances ====================

// Server deployer (10.1.33.2)
SSHDeployer g_ssh_deployer_server("10.1.33.2", "user", "q", "/home/user/Desktop", "Server");

// Cumulus switch deployer
SSHDeployer g_ssh_deployer_cumulus("10.1.33.3", "cumulus", "%T86Ovk7RCH%h@CC", "", "Cumulus");

// Legacy alias for backward compatibility (points to server deployer)
SSHDeployer& g_Deployer = g_ssh_deployer_server;

// ==================== Constructors ====================

SSHDeployer::SSHDeployer()
    : m_host("")
    , m_username("")
    , m_password("")
    , m_remote_directory("")
    , m_name("SSHDeployer") {
}

SSHDeployer::SSHDeployer(const SSHConfig& config)
    : m_host(config.host)
    , m_username(config.username)
    , m_password(config.password)
    , m_remote_directory(config.remote_directory)
    , m_name(config.name.empty() ? "SSHDeployer" : config.name) {
}

SSHDeployer::SSHDeployer(const std::string& host,
                         const std::string& username,
                         const std::string& password,
                         const std::string& remote_directory,
                         const std::string& name)
    : m_host(host)
    , m_username(username)
    , m_password(password)
    , m_remote_directory(remote_directory)
    , m_name(name.empty() ? "SSHDeployer" : name) {
}

// ==================== Configuration ====================

void SSHDeployer::configure(const SSHConfig& config) {
    m_host = config.host;
    m_username = config.username;
    m_password = config.password;
    m_remote_directory = config.remote_directory;
    if (!config.name.empty()) {
        m_name = config.name;
    }
}

void SSHDeployer::setCredentials(const std::string& host,
                                  const std::string& username,
                                  const std::string& password) {
    m_host = host;
    m_username = username;
    m_password = password;
}

void SSHDeployer::setHost(const std::string& host) {
    m_host = host;
}

std::string SSHDeployer::getHost() const {
    return m_host;
}

void SSHDeployer::setUsername(const std::string& username) {
    m_username = username;
}

std::string SSHDeployer::getUsername() const {
    return m_username;
}

void SSHDeployer::setPassword(const std::string& password) {
    m_password = password;
}

void SSHDeployer::setRemoteDirectory(const std::string& path) {
    m_remote_directory = path;
}

std::string SSHDeployer::getRemoteDirectory() const {
    return m_remote_directory;
}

void SSHDeployer::setName(const std::string& name) {
    m_name = name;
}

std::string SSHDeployer::getName() const {
    return m_name;
}

bool SSHDeployer::isConfigured() const {
    return !m_host.empty() && !m_username.empty() && !m_password.empty();
}

std::string SSHDeployer::getExecutableDir() {
    // Linux: /proc/self/exe gives the full path to the executable
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe");
    return exe_path.parent_path().string();
}

std::string SSHDeployer::getSourceRoot() {
    std::filesystem::path exe_dir = getExecutableDir();

    // Dev mode: MainSoftware/build/bin/exe → 3 levels up to project root
    std::filesystem::path dev_root = std::filesystem::canonical(exe_dir / ".." / ".." / "..");
    if (std::filesystem::exists(dev_root / "MainSoftware" / "CMakeLists.txt")) {
        return dev_root.string();
    }

    // Portable mode: MainSoftware/bin/exe → 2 levels up to MMUComputerTestSoftware root
    std::filesystem::path portable_root = std::filesystem::canonical(exe_dir / ".." / "..");
    return portable_root.string();
}

// ==================== Internal Helpers ====================

std::string SSHDeployer::getLogPrefix() const {
    return "[" + m_name + "]";
}

std::string SSHDeployer::buildSSHCommand(const std::string& remote_command) const {
    return "sshpass -p '" + m_password + "' "
           "ssh -o StrictHostKeyChecking=no "
           "-o ConnectTimeout=10 "
           + m_username + "@" + m_host + " "
           "\"" + remote_command + "\"";
}

std::string SSHDeployer::buildSCPCommand(const std::string& local_path,
                                          const std::string& remote_path) const {
    return "sshpass -p '" + m_password + "' "
           "scp -o StrictHostKeyChecking=no "
           "-o ConnectTimeout=10 "
           + local_path + " "
           + m_username + "@" + m_host + ":" + remote_path;
}

// ==================== Utilities ====================

bool SSHDeployer::testConnection() {
    DEBUG_LOG(getLogPrefix() << " Testing connection to " << m_host << "...");

    std::string cmd = buildSSHCommand("echo 'Connection OK'");
    auto result = g_systemCommand.execute(cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " Connection successful!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " Connection failed: " << result.error << std::endl;
        return false;
    }
}

// ==================== File Operations ====================

bool SSHDeployer::copyFile(const std::string& local_path) {
    // Check if file exists
    if (!std::filesystem::exists(local_path)) {
        std::cerr << getLogPrefix() << " Local file not found: " << local_path << std::endl;
        return false;
    }

    std::string filename = std::filesystem::path(local_path).filename().string();
    std::string remote_path = m_remote_directory + "/" + filename;

    DEBUG_LOG(getLogPrefix() << " Copying " << local_path << " -> " << remote_path);

    // Create target directory first
    std::string mkdir_cmd = buildSSHCommand("mkdir -p " + m_remote_directory);
    g_systemCommand.execute(mkdir_cmd);

    // Copy file
    std::string scp_cmd = buildSCPCommand(local_path, remote_path);
    auto result = g_systemCommand.execute(scp_cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " File copied successfully!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " Copy failed: " << result.error << std::endl;
        return false;
    }
}

bool SSHDeployer::deploy(const std::string& local_path) {
    // Copy file
    if (!copyFile(local_path)) {
        return false;
    }

    // Make executable
    std::string filename = std::filesystem::path(local_path).filename().string();
    std::string remote_path = m_remote_directory + "/" + filename;

    DEBUG_LOG(getLogPrefix() << " Making executable: " << remote_path);

    std::string chmod_cmd = buildSSHCommand("chmod +x " + remote_path);
    auto result = g_systemCommand.execute(chmod_cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " Deploy completed!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " chmod failed: " << result.error << std::endl;
        return false;
    }
}

bool SSHDeployer::copyDirectory(const std::string& local_dir, const std::string& remote_name) {
    // Check if directory exists
    if (!std::filesystem::exists(local_dir) || !std::filesystem::is_directory(local_dir)) {
        std::cerr << getLogPrefix() << " Local directory not found: " << local_dir << std::endl;
        return false;
    }

    std::string dir_name = remote_name.empty()
        ? std::filesystem::path(local_dir).filename().string()
        : remote_name;
    std::string remote_path = m_remote_directory + "/" + dir_name;

    DEBUG_LOG(getLogPrefix() << " Copying directory " << local_dir << " -> " << remote_path);

    // Create target directory first
    std::string mkdir_cmd = buildSSHCommand("mkdir -p " + m_remote_directory);
    g_systemCommand.execute(mkdir_cmd);

    // Remove old directory (if exists)
    std::string rm_cmd = buildSSHCommand("rm -rf " + remote_path);
    g_systemCommand.execute(rm_cmd);

    // Copy directory (scp -r)
    std::string scp_cmd = "sshpass -p '" + m_password + "' "
                          "scp -r -o StrictHostKeyChecking=no "
                          "-o ConnectTimeout=30 "
                          + local_dir + " "
                          + m_username + "@" + m_host + ":" + m_remote_directory + "/";

    auto result = g_systemCommand.execute(scp_cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " Directory copied successfully!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " Directory copy failed: " << result.error << std::endl;
        return false;
    }
}

bool SSHDeployer::copyFileToPath(const std::string& local_path, const std::string& remote_path, bool use_sudo) {
    // Check if file exists
    if (!std::filesystem::exists(local_path)) {
        std::cerr << getLogPrefix() << " Local file not found: " << local_path << std::endl;
        return false;
    }

    std::string filename = std::filesystem::path(local_path).filename().string();

    if (use_sudo) {
        DEBUG_LOG(getLogPrefix() << " Copying " << local_path << " -> " << remote_path << " (with sudo)");
    } else {
        DEBUG_LOG(getLogPrefix() << " Copying " << local_path << " -> " << remote_path);
    }

    if (use_sudo) {
        // For protected directories: copy to /tmp first, then sudo mv
        std::string tmp_path = "/tmp/" + filename;

        // Step 1: Copy to /tmp
        std::string scp_cmd = buildSCPCommand(local_path, tmp_path);
        auto result = g_systemCommand.execute(scp_cmd);

        if (!result.success) {
            std::cerr << getLogPrefix() << " Copy to /tmp failed: " << result.error << std::endl;
            return false;
        }

        // Step 2: Get remote directory path
        std::string remote_dir = std::filesystem::path(remote_path).parent_path().string();

        // Step 3: Create remote directory if needed (with sudo)
        std::string mkdir_cmd = "echo '" + m_password + "' | sudo -S mkdir -p " + remote_dir;
        std::string ssh_mkdir = buildSSHCommand(mkdir_cmd);
        g_systemCommand.execute(ssh_mkdir);

        // Step 4: Move file with sudo
        std::string mv_cmd = "echo '" + m_password + "' | sudo -S mv " + tmp_path + " " + remote_path;
        std::string ssh_mv = buildSSHCommand(mv_cmd);
        result = g_systemCommand.execute(ssh_mv);

        if (result.success) {
            DEBUG_LOG(getLogPrefix() << " File copied successfully (sudo)!");
            return true;
        } else {
            std::cerr << getLogPrefix() << " sudo mv failed: " << result.error << std::endl;
            return false;
        }
    } else {
        // Direct copy without sudo
        std::string remote_dir = std::filesystem::path(remote_path).parent_path().string();

        // Create target directory first
        std::string mkdir_cmd = buildSSHCommand("mkdir -p " + remote_dir);
        g_systemCommand.execute(mkdir_cmd);

        // Copy file
        std::string scp_cmd = buildSCPCommand(local_path, remote_path);
        auto result = g_systemCommand.execute(scp_cmd);

        if (result.success) {
            DEBUG_LOG(getLogPrefix() << " File copied successfully!");
            return true;
        } else {
            std::cerr << getLogPrefix() << " Copy failed: " << result.error << std::endl;
            return false;
        }
    }
}

bool SSHDeployer::fetchFile(const std::string& remote_path, const std::string& local_path) {
    DEBUG_LOG(getLogPrefix() << " Fetching " << remote_path << " -> " << local_path);

    // Create local directory if needed
    std::filesystem::path local_dir = std::filesystem::path(local_path).parent_path();
    if (!local_dir.empty() && !std::filesystem::exists(local_dir)) {
        std::filesystem::create_directories(local_dir);
        DEBUG_LOG(getLogPrefix() << " Created local directory: " << local_dir.string());
    }

    // Build SCP command to fetch from remote
    std::string scp_cmd = "sshpass -p '" + m_password + "' "
                          "scp -o StrictHostKeyChecking=no "
                          "-o ConnectTimeout=30 "
                          + m_username + "@" + m_host + ":" + remote_path + " "
                          + local_path;

    auto result = g_systemCommand.execute(scp_cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " File fetched successfully!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " File fetch failed: " << result.error << std::endl;
        return false;
    }
}

bool SSHDeployer::deployBuildRunAndFetchLog(const std::string& local_source_dir,
                                             const std::string& app_name,
                                             const std::string& run_args,
                                             const std::string& local_log_path,
                                             int timeout_seconds) {
    DEBUG_LOG(getLogPrefix() << " ========================================");
    DEBUG_LOG(getLogPrefix() << " Deploy, Build, Run & Fetch Log Pipeline");
    DEBUG_LOG(getLogPrefix() << " ========================================");

    // Step 1: Resolve source path
    std::string source_path;
    if (std::filesystem::exists(local_source_dir)) {
        source_path = local_source_dir;
    } else {
        // Try from source root
        source_path = getSourceRoot() + "/" + local_source_dir;
        if (!std::filesystem::exists(source_path)) {
            std::cerr << getLogPrefix() << " Source directory not found: " << local_source_dir << std::endl;
            return false;
        }
    }

    std::string folder_name = std::filesystem::path(source_path).filename().string();
    std::string executable_name = app_name.empty() ? folder_name : app_name;
    std::string remote_project_path = m_remote_directory + "/" + folder_name;
    std::string remote_log_path = "/tmp/" + executable_name + ".log";

    DEBUG_LOG(getLogPrefix() << " Source: " << source_path);
    DEBUG_LOG(getLogPrefix() << " Remote path: " << remote_project_path);
    DEBUG_LOG(getLogPrefix() << " Executable: " << executable_name);
    DEBUG_LOG(getLogPrefix() << " Remote log: " << remote_log_path);
    DEBUG_LOG(getLogPrefix() << " Local log: " << local_log_path);

    // Step 2: Test connection
    DEBUG_LOG(getLogPrefix() << " Step 1/5: Testing connection...");
    if (!testConnection()) {
        return false;
    }

    // Step 3: Copy source directory
    DEBUG_LOG(getLogPrefix() << " Step 2/5: Copying source code...");
    if (!copyDirectory(source_path)) {
        std::cerr << getLogPrefix() << " Failed to copy source directory" << std::endl;
        return false;
    }

    // Step 4: Build
    DEBUG_LOG(getLogPrefix() << " Step 3/5: Building on remote server...");
    if (!build(folder_name, executable_name, BuildSystem::AUTO)) {
        std::cerr << getLogPrefix() << " Build failed" << std::endl;
        return false;
    }

    // Step 5: Run with output redirected to log file (foreground, wait for completion)
    DEBUG_LOG(getLogPrefix() << " Step 4/5: Running application...");

    // Find executable
    std::string check_cmd = "test -f " + remote_project_path + "/" + executable_name + " && echo 'found'";
    std::string ssh_check = buildSSHCommand(check_cmd);
    auto check_result = g_systemCommand.execute(ssh_check);

    std::string executable_path;
    if (check_result.output.find("found") != std::string::npos) {
        executable_path = remote_project_path + "/" + executable_name;
    } else {
        // Try with _app suffix
        check_cmd = "test -f " + remote_project_path + "/" + folder_name + "_app && echo 'found'";
        ssh_check = buildSSHCommand(check_cmd);
        check_result = g_systemCommand.execute(ssh_check);
        if (check_result.output.find("found") != std::string::npos) {
            executable_path = remote_project_path + "/" + folder_name + "_app";
            executable_name = folder_name + "_app";
            remote_log_path = "/tmp/" + executable_name + ".log";
        } else {
            std::cerr << getLogPrefix() << " Executable not found!" << std::endl;
            return false;
        }
    }

    // Build run command with sudo and output to log file
    std::string run_command = "cd " + remote_project_path + " && "
                              "echo '" + m_password + "' | sudo -S " + executable_path;
    if (!run_args.empty()) {
        run_command += " " + run_args;
    }
    run_command += " 2>&1 | tee " + remote_log_path;

    std::string ssh_run = buildSSHCommand(run_command);

    // Execute with timeout (convert seconds to milliseconds)
    int timeout_ms = timeout_seconds > 0 ? timeout_seconds * 1000 : 120000;
    auto run_result = g_systemCommand.execute(ssh_run, timeout_ms);

    // Show output (always visible - shows actual test results)
    if (!run_result.output.empty()) {
        std::cout << getLogPrefix() << " Application output:\n" << run_result.output << std::endl;
    }

    if (!run_result.success) {
        std::cerr << getLogPrefix() << " Application execution had issues: " << run_result.error << std::endl;
        // Continue to fetch log even if there were issues
    }

    // Step 6: Fetch log file
    DEBUG_LOG(getLogPrefix() << " Step 5/5: Fetching log file...");
    if (!fetchFile(remote_log_path, local_log_path)) {
        std::cerr << getLogPrefix() << " Warning: Could not fetch log file" << std::endl;
        // Not a fatal error - application may have completed successfully
    }

    DEBUG_LOG(getLogPrefix() << " ========================================");
    DEBUG_LOG(getLogPrefix() << " Pipeline completed!");
    DEBUG_LOG(getLogPrefix() << " Log saved to: " << local_log_path);
    DEBUG_LOG(getLogPrefix() << " ========================================");

    return true;
}

// ==================== Command Execution ====================

bool SSHDeployer::execute(const std::string& command, std::string* output, bool use_sudo, bool silent) {
    std::string actual_command = command;
    if (use_sudo) {
        // echo password | sudo -S command
        actual_command = "echo '" + m_password + "' | sudo -S " + command;
    }

    if (!silent) {
        if (use_sudo) {
            DEBUG_LOG(getLogPrefix() << " Executing: " << command << " (with sudo)");
        } else {
            DEBUG_LOG(getLogPrefix() << " Executing: " << command);
        }
    }

    std::string ssh_cmd = buildSSHCommand(actual_command);
    auto result = g_systemCommand.execute(ssh_cmd, 120000); // 2 minute timeout

    if (output) {
        *output = result.output;
    }

    if (result.success) {
        if (!silent && !result.output.empty()) {
            DEBUG_LOG(getLogPrefix() << " Output:\n" << result.output);
        }
        return true;
    } else {
        // Always show errors even in silent mode
        if (!result.output.empty()) {
            std::cerr << getLogPrefix() << " Output:\n" << result.output << std::endl;
        }
        if (!result.error.empty()) {
            std::cerr << getLogPrefix() << " Error: " << result.error << std::endl;
        }
        return false;
    }
}

bool SSHDeployer::executeBackground(const std::string& command) {
    DEBUG_LOG(getLogPrefix() << " Executing in background: " << command);

    std::string bg_command = "nohup " + command + " > /dev/null 2>&1 &";
    std::string ssh_cmd = buildSSHCommand(bg_command);
    auto result = g_systemCommand.execute(ssh_cmd);

    if (result.success) {
        DEBUG_LOG(getLogPrefix() << " Background process started!");
        return true;
    } else {
        std::cerr << getLogPrefix() << " Failed to start background process" << std::endl;
        return false;
    }
}

bool SSHDeployer::executeInteractive(const std::string& command, bool use_sudo) {
    std::string actual_command = command;
    if (use_sudo) {
        // Use sudo with password via stdin
        actual_command = "echo '" + m_password + "' | sudo -S " + command;
    }

    if (use_sudo) {
        DEBUG_LOG(getLogPrefix() << " Executing interactively: " << command << " (with sudo)");
    } else {
        DEBUG_LOG(getLogPrefix() << " Executing interactively: " << command);
    }

    // Build SSH command with -t flag for pseudo-terminal allocation
    // -t forces PTY allocation even when stdin is not a terminal
    // This allows interactive programs (getchar, fgets, etc.) to work
    std::string ssh_cmd = "sshpass -p '" + m_password + "' "
                          "ssh -t -o StrictHostKeyChecking=no "
                          "-o ConnectTimeout=10 "
                          + m_username + "@" + m_host + " "
                          "\"" + actual_command + "\"";

    // Use system() for true interactive execution
    // This connects stdin/stdout directly to the terminal
    int ret = system(ssh_cmd.c_str());

    if (ret == 0) {
        DEBUG_LOG(getLogPrefix() << " Interactive command completed successfully");
        return true;
    } else {
        std::cerr << getLogPrefix() << " Interactive command failed with exit code: " << ret << std::endl;
        return false;
    }
}

bool SSHDeployer::run(const std::string& app_name, const std::string& args) {
    std::string full_path = m_remote_directory + "/" + app_name;
    std::string command = full_path;

    if (!args.empty()) {
        command += " " + args;
    }

    return execute(command);
}

// ==================== Build & Deploy ====================

BuildSystem SSHDeployer::detectBuildSystem(const std::string& project_path) {
    DEBUG_LOG(getLogPrefix() << " Detecting build system for: " << project_path);

    // Check for CMakeLists.txt
    std::string check_cmake = buildSSHCommand("test -f " + project_path + "/CMakeLists.txt && echo 'CMAKE'");
    auto result = g_systemCommand.execute(check_cmake);
    if (result.success && result.output.find("CMAKE") != std::string::npos) {
        DEBUG_LOG(getLogPrefix() << " Detected: CMake project");
        return BuildSystem::CMAKE;
    }

    // Check for Makefile
    std::string check_makefile = buildSSHCommand("test -f " + project_path + "/Makefile && echo 'MAKEFILE'");
    result = g_systemCommand.execute(check_makefile);
    if (result.success && result.output.find("MAKEFILE") != std::string::npos) {
        DEBUG_LOG(getLogPrefix() << " Detected: Makefile project");
        return BuildSystem::MAKEFILE;
    }

    std::cerr << getLogPrefix() << " No build system detected!" << std::endl;
    return BuildSystem::AUTO;
}

bool SSHDeployer::buildWithCMake(const std::string& project_path, const std::string& output_name) {
    std::string build_dir = project_path + "/build";

    // Create build directory
    DEBUG_LOG(getLogPrefix() << " Creating build directory...");
    std::string mkdir_cmd = buildSSHCommand("mkdir -p " + build_dir);
    auto result = g_systemCommand.execute(mkdir_cmd);
    if (!result.success) {
        std::cerr << getLogPrefix() << " Failed to create build directory" << std::endl;
        return false;
    }

    // Run CMake
    DEBUG_LOG(getLogPrefix() << " Running cmake...");
    std::string cmake_cmd = buildSSHCommand("cd " + build_dir + " && cmake ..");
    result = g_systemCommand.execute(cmake_cmd, 60000); // 60 second timeout
    if (!result.success) {
        std::cerr << getLogPrefix() << " CMake failed: " << result.error << std::endl;
        std::cerr << getLogPrefix() << " Output: " << result.output << std::endl;
        return false;
    }
    DEBUG_LOG(getLogPrefix() << " CMake output:\n" << result.output);

    // Run Make
    DEBUG_LOG(getLogPrefix() << " Running make...");
    std::string make_cmd = buildSSHCommand("cd " + build_dir + " && make -j$(nproc)");
    result = g_systemCommand.execute(make_cmd, 120000); // 120 second timeout
    if (!result.success) {
        std::cerr << getLogPrefix() << " Make failed: " << result.error << std::endl;
        std::cerr << getLogPrefix() << " Output: " << result.output << std::endl;
        return false;
    }
    DEBUG_LOG(getLogPrefix() << " Make output:\n" << result.output);

    return true;
}

bool SSHDeployer::buildWithMakefile(const std::string& project_path,
                                     const std::string& output_name,
                                     const std::string& make_args) {
    // For Makefile projects (like DPDK), build directly in source directory
    DEBUG_LOG(getLogPrefix() << " Building with Makefile...");

    // Clean first (optional but recommended)
    DEBUG_LOG(getLogPrefix() << " Cleaning previous build...");
    std::string clean_cmd = buildSSHCommand("cd " + project_path + " && make clean 2>/dev/null || true");
    g_systemCommand.execute(clean_cmd, 30000);

    // Build with make
    std::string make_cmd = "cd " + project_path + " && make -j$(nproc)";
    if (!make_args.empty()) {
        make_cmd += " " + make_args;
    }

    DEBUG_LOG(getLogPrefix() << " Running make...");
    std::string ssh_make = buildSSHCommand(make_cmd);
    auto result = g_systemCommand.execute(ssh_make, 180000); // 3 minute timeout for DPDK

    if (!result.success) {
        std::cerr << getLogPrefix() << " Make failed: " << result.error << std::endl;
        std::cerr << getLogPrefix() << " Output: " << result.output << std::endl;
        return false;
    }
    DEBUG_LOG(getLogPrefix() << " Make output:\n" << result.output);

    return true;
}

bool SSHDeployer::build(const std::string& project_dir,
                         const std::string& output_name,
                         BuildSystem build_system,
                         const std::string& make_args) {
    std::string full_project_path = m_remote_directory + "/" + project_dir;

    DEBUG_LOG(getLogPrefix() << " Building project: " << full_project_path);

    // Auto-detect build system if needed
    BuildSystem actual_build_system = build_system;
    if (build_system == BuildSystem::AUTO) {
        actual_build_system = detectBuildSystem(full_project_path);
        if (actual_build_system == BuildSystem::AUTO) {
            std::cerr << getLogPrefix() << " Could not detect build system!" << std::endl;
            return false;
        }
    }

    bool success = false;
    switch (actual_build_system) {
        case BuildSystem::CMAKE:
            success = buildWithCMake(full_project_path, output_name);
            break;
        case BuildSystem::MAKEFILE:
            success = buildWithMakefile(full_project_path, output_name, make_args);
            break;
        default:
            std::cerr << getLogPrefix() << " Unknown build system!" << std::endl;
            return false;
    }

    if (success) {
        DEBUG_LOG(getLogPrefix() << " Build completed successfully!");
    }
    return success;
}

bool SSHDeployer::deployAndBuild(const std::string& local_source_dir,
                                  const std::string& app_name,
                                  bool run_after_build,
                                  bool use_sudo,
                                  BuildSystem build_system,
                                  const std::string& run_args,
                                  const std::string& make_args,
                                  bool run_in_background) {
    // Auto-resolve path: if not absolute, prepend source root
    std::string resolved_path = local_source_dir;
    if (!local_source_dir.empty() && local_source_dir[0] != '/') {
        resolved_path = getSourceRoot() + "/" + local_source_dir;
    }

    // Use folder name as app_name if not provided
    std::string actual_app_name = app_name;
    if (actual_app_name.empty()) {
        actual_app_name = std::filesystem::path(resolved_path).filename().string();
    }

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Starting Deploy & Build Pipeline");
    DEBUG_LOG(getLogPrefix() << " Target: " << m_username << "@" << m_host);
    DEBUG_LOG(getLogPrefix() << " Source: " << resolved_path);
    if (use_sudo) DEBUG_LOG(getLogPrefix() << " sudo mode enabled");
    DEBUG_LOG("========================================");

    // 1. Test connection
    DEBUG_LOG("\n[Step 1/4] Testing connection...");
    if (!testConnection()) {
        std::cerr << getLogPrefix() << " Pipeline failed: Connection error" << std::endl;
        return false;
    }

    // 2. Copy source code
    DEBUG_LOG("\n[Step 2/4] Copying source code...");
    if (!copyDirectory(resolved_path, actual_app_name)) {
        std::cerr << getLogPrefix() << " Pipeline failed: Copy error" << std::endl;
        return false;
    }

    // 3. Build - detect build system first for later use
    DEBUG_LOG("\n[Step 3/4] Building on remote server...");
    std::string full_project_path = m_remote_directory + "/" + actual_app_name;

    BuildSystem actual_build_system = build_system;
    if (build_system == BuildSystem::AUTO) {
        actual_build_system = detectBuildSystem(full_project_path);
    }

    if (!build(actual_app_name, actual_app_name, actual_build_system, make_args)) {
        std::cerr << getLogPrefix() << " Pipeline failed: Build error" << std::endl;
        return false;
    }

    // 4. Run (optional)
    if (run_after_build) {
        if (run_in_background) {
            DEBUG_LOG("\n[Step 4/4] Running application (background mode)...");
        } else {
            DEBUG_LOG("\n[Step 4/4] Running application...");
        }

        // Executable path depends on build system:
        // - CMake: project_dir/build/app_name
        // - Makefile: project_dir/app_name (DPDK uses dpdk_app)
        std::string executable_path;
        std::string executable_name = actual_app_name;

        if (actual_build_system == BuildSystem::MAKEFILE) {
            // For DPDK-style Makefile projects, check for dpdk_app or app_name
            std::string check_dpdk = buildSSHCommand("test -f " + full_project_path + "/dpdk_app && echo 'EXISTS'");
            auto result = g_systemCommand.execute(check_dpdk);
            if (result.success && result.output.find("EXISTS") != std::string::npos) {
                executable_name = "dpdk_app";
            }
            executable_path = full_project_path + "/" + executable_name;
        } else {
            // CMake projects have build directory
            executable_path = full_project_path + "/build/" + executable_name;
        }

        // Build run command with arguments
        std::string run_command = executable_path;
        if (!run_args.empty()) {
            run_command += " " + run_args;
        }

        if (run_in_background) {
            // For long-running applications like DPDK, run in background
            std::string bg_command = run_command;
            if (use_sudo) {
                bg_command = "echo '" + m_password + "' | sudo -S nohup " + run_command + " > /tmp/" + executable_name + ".log 2>&1 &";
            } else {
                bg_command = "nohup " + run_command + " > /tmp/" + executable_name + ".log 2>&1 &";
            }

            std::string ssh_cmd = buildSSHCommand(bg_command);
            auto result = g_systemCommand.execute(ssh_cmd);

            if (result.success) {
                DEBUG_LOG(getLogPrefix() << " Application started in background!");
                DEBUG_LOG(getLogPrefix() << " Log file: /tmp/" << executable_name << ".log");
            } else {
                std::cerr << getLogPrefix() << " Failed to start background process" << std::endl;
                return false;
            }
        } else {
            // Run in foreground (blocks until application exits)
            if (!execute(run_command, nullptr, use_sudo)) {
                std::cerr << getLogPrefix() << " Pipeline failed: Execution error" << std::endl;
                return false;
            }
        }
    } else {
        DEBUG_LOG("\n[Step 4/4] Skipping execution (run_after_build=false)");
    }

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Pipeline completed successfully!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool SSHDeployer::stopApplication(const std::string& app_name, bool use_sudo) {
    DEBUG_LOG(getLogPrefix() << " Stopping application: " << app_name);

    // Step 1: Send SIGTERM for graceful shutdown
    std::string term_cmd;
    if (use_sudo) {
        term_cmd = "echo '" + m_password + "' | sudo -S -v 2>/dev/null && "
                   "sudo pkill -TERM -f " + app_name + " 2>/dev/null; "
                   "echo TERM_SENT";
    } else {
        term_cmd = "pkill -TERM -f " + app_name + " 2>/dev/null; "
                   "echo TERM_SENT";
    }

    std::string ssh_cmd = buildSSHCommand(term_cmd);
    DEBUG_LOG(getLogPrefix() << " Sending SIGTERM...");
    auto result = g_systemCommand.execute(ssh_cmd);
    DEBUG_LOG(getLogPrefix() << " SIGTERM result: " << result.output);

    // Step 2: Wait for process to exit gracefully (up to 60 seconds)
    const int max_wait_seconds = 60;
    DEBUG_LOG(getLogPrefix() << " Waiting for graceful shutdown (max "
              << max_wait_seconds << "s)...");

    for (int i = 0; i < max_wait_seconds; i++) {
        usleep(1000000);  // 1 second

        if (!isApplicationRunning(app_name)) {
            DEBUG_LOG(getLogPrefix() << " Application stopped gracefully after "
                      << (i + 1) << " seconds");
            return true;
        }

        // Print progress every 5 seconds
        if ((i + 1) % 5 == 0) {
            DEBUG_LOG(getLogPrefix() << " Still waiting... (" << (i + 1)
                      << "/" << max_wait_seconds << "s)");
        }
    }

    // Step 3: Timeout exceeded - force kill as last resort
    std::cerr << getLogPrefix() << " WARNING: Graceful shutdown timed out after "
              << max_wait_seconds << "s, sending SIGKILL..." << std::endl;

    std::string kill_cmd;
    if (use_sudo) {
        kill_cmd = "echo '" + m_password + "' | sudo -S -v 2>/dev/null && "
                   "sudo pkill -9 -f " + app_name + " 2>/dev/null; "
                   "echo KILL_DONE";
    } else {
        kill_cmd = "pkill -9 -f " + app_name + " 2>/dev/null; "
                   "echo KILL_DONE";
    }

    ssh_cmd = buildSSHCommand(kill_cmd);
    g_systemCommand.execute(ssh_cmd);
    usleep(1000000);  // 1 second

    if (isApplicationRunning(app_name)) {
        std::cerr << getLogPrefix() << " FAILED to stop " << app_name << std::endl;
        return false;
    }

    DEBUG_LOG(getLogPrefix() << " Application force-killed successfully");
    return true;
}

bool SSHDeployer::isApplicationRunning(const std::string& app_name) {
    // Use pgrep to check if process exists, also show PID for debugging
    // Note: pgrep -x matches exact process name (not full cmdline) to avoid
    // matching the pgrep command itself or SSH shell that contains the app name
    std::string check_cmd = "pgrep -x '" + app_name + "' && echo 'PROC_FOUND' || echo 'PROC_NOT_FOUND'";
    std::string ssh_cmd = buildSSHCommand(check_cmd);

    auto result = g_systemCommand.execute(ssh_cmd);

    // Debug: show raw output
    DEBUG_LOG(getLogPrefix() << " [DEBUG] pgrep output: '" << result.output << "'");

    bool running = result.output.find("PROC_FOUND") != std::string::npos
                   && result.output.find("PROC_NOT_FOUND") == std::string::npos;

    DEBUG_LOG(getLogPrefix() << " Application '" << app_name << "' is "
              << (running ? "RUNNING" : "NOT RUNNING"));

    return running;
}

// ==================== Prebuilt Binary Deploy ====================

std::string SSHDeployer::getPrebuiltRoot() {
    std::string source_root = getSourceRoot();
    // Dev mode: MMUComputerTestSoftware/ is a subdirectory of the project root
    std::string sub = source_root + "/MMUComputerTestSoftware";
    if (std::filesystem::exists(sub)) {
        return sub;
    }
    // Portable mode: source_root IS the MMUComputerTestSoftware directory
    return source_root;
}

bool SSHDeployer::deployPrebuilt(const std::string& prebuilt_dir,
                                  const std::string& app_name,
                                  bool run_after_deploy,
                                  bool use_sudo,
                                  const std::string& run_args,
                                  bool run_in_background) {
    // Resolve prebuilt path
    std::string prebuilt_path = getPrebuiltRoot() + "/" + prebuilt_dir;
    if (!std::filesystem::exists(prebuilt_path)) {
        std::cerr << getLogPrefix() << " Prebuilt directory not found: " << prebuilt_path << std::endl;
        std::cerr << getLogPrefix() << " Run preparePrebuilt() first to compile and fetch binaries." << std::endl;
        return false;
    }

    std::string actual_app_name = app_name.empty()
        ? std::filesystem::path(prebuilt_path).filename().string()
        : app_name;

    std::string remote_project_path = m_remote_directory + "/" + prebuilt_dir;

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Starting Prebuilt Deploy Pipeline (NO BUILD)");
    DEBUG_LOG(getLogPrefix() << " Target: " << m_username << "@" << m_host);
    DEBUG_LOG(getLogPrefix() << " Source: " << prebuilt_path);
    DEBUG_LOG(getLogPrefix() << " Remote: " << remote_project_path);
    if (use_sudo) DEBUG_LOG(getLogPrefix() << " sudo mode enabled");
    DEBUG_LOG("========================================");

    // Step 1: Test connection
    DEBUG_LOG("\n[Step 1/3] Testing connection...");
    if (!testConnection()) {
        std::cerr << getLogPrefix() << " Pipeline failed: Connection error" << std::endl;
        return false;
    }

    // Step 2: Copy prebuilt directory (binary + runtime files, no source code)
    DEBUG_LOG("\n[Step 2/3] Copying prebuilt binaries...");
    if (!copyDirectory(prebuilt_path, prebuilt_dir)) {
        std::cerr << getLogPrefix() << " Pipeline failed: Copy error" << std::endl;
        return false;
    }

    // Make all files in the directory executable
    std::string chmod_cmd = buildSSHCommand("find " + remote_project_path + " -type f -exec chmod +x {} \\;");
    g_systemCommand.execute(chmod_cmd);

    // Step 3: Run (optional)
    if (run_after_deploy) {
        if (run_in_background) {
            DEBUG_LOG("\n[Step 3/3] Running application (background mode)...");
        } else {
            DEBUG_LOG("\n[Step 3/3] Running application...");
        }

        // Find executable - check common patterns
        std::string executable_path;
        std::string executable_name = actual_app_name;

        // Check for dpdk_app (DPDK convention)
        std::string check_dpdk = buildSSHCommand("test -f " + remote_project_path + "/dpdk_app && echo 'EXISTS'");
        auto result = g_systemCommand.execute(check_dpdk);
        if (result.success && result.output.find("EXISTS") != std::string::npos) {
            executable_name = "dpdk_app";
        }

        // Check for executable with same name as directory
        std::string check_name = buildSSHCommand("test -f " + remote_project_path + "/" + executable_name + " && echo 'EXISTS'");
        result = g_systemCommand.execute(check_name);
        if (result.success && result.output.find("EXISTS") != std::string::npos) {
            executable_path = remote_project_path + "/" + executable_name;
        } else {
            // Try build/bin/ subdirectory (CMake convention)
            check_name = buildSSHCommand("test -f " + remote_project_path + "/build/bin/" + executable_name + " && echo 'EXISTS'");
            result = g_systemCommand.execute(check_name);
            if (result.success && result.output.find("EXISTS") != std::string::npos) {
                executable_path = remote_project_path + "/build/bin/" + executable_name;
            } else {
                std::cerr << getLogPrefix() << " Executable not found in prebuilt directory!" << std::endl;
                return false;
            }
        }

        // Build and execute run command
        std::string run_command = executable_path;
        if (!run_args.empty()) {
            run_command += " " + run_args;
        }

        if (run_in_background) {
            std::string bg_command;
            if (use_sudo) {
                bg_command = "echo '" + m_password + "' | sudo -S nohup " + run_command + " > /tmp/" + executable_name + ".log 2>&1 &";
            } else {
                bg_command = "nohup " + run_command + " > /tmp/" + executable_name + ".log 2>&1 &";
            }

            std::string ssh_cmd = buildSSHCommand(bg_command);
            result = g_systemCommand.execute(ssh_cmd);

            if (result.success) {
                DEBUG_LOG(getLogPrefix() << " Application started in background!");
                DEBUG_LOG(getLogPrefix() << " Log file: /tmp/" << executable_name << ".log");
            } else {
                std::cerr << getLogPrefix() << " Failed to start background process" << std::endl;
                return false;
            }
        } else {
            if (!execute(run_command, nullptr, use_sudo)) {
                std::cerr << getLogPrefix() << " Pipeline failed: Execution error" << std::endl;
                return false;
            }
        }
    } else {
        DEBUG_LOG("\n[Step 3/3] Skipping execution (run_after_deploy=false)");
    }

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Prebuilt Deploy Pipeline completed!");
    DEBUG_LOG("========================================\n");

    return true;
}

bool SSHDeployer::deployPrebuiltRunAndFetchLog(const std::string& prebuilt_dir,
                                                const std::string& app_name,
                                                const std::string& run_args,
                                                const std::string& local_log_path,
                                                int timeout_seconds) {
    DEBUG_LOG(getLogPrefix() << " ========================================");
    DEBUG_LOG(getLogPrefix() << " Prebuilt Deploy, Run & Fetch Log Pipeline");
    DEBUG_LOG(getLogPrefix() << " ========================================");

    // Step 1: Resolve prebuilt path
    std::string prebuilt_path = getPrebuiltRoot() + "/" + prebuilt_dir;
    if (!std::filesystem::exists(prebuilt_path)) {
        std::cerr << getLogPrefix() << " Prebuilt directory not found: " << prebuilt_path << std::endl;
        return false;
    }

    std::string folder_name = std::filesystem::path(prebuilt_path).filename().string();
    std::string executable_name = app_name.empty() ? folder_name : app_name;
    std::string remote_project_path = m_remote_directory + "/" + folder_name;
    std::string remote_log_path = "/tmp/" + executable_name + ".log";

    DEBUG_LOG(getLogPrefix() << " Source: " << prebuilt_path);
    DEBUG_LOG(getLogPrefix() << " Remote path: " << remote_project_path);
    DEBUG_LOG(getLogPrefix() << " Executable: " << executable_name);
    DEBUG_LOG(getLogPrefix() << " Remote log: " << remote_log_path);
    DEBUG_LOG(getLogPrefix() << " Local log: " << local_log_path);

    // Step 2: Test connection
    DEBUG_LOG(getLogPrefix() << " Step 1/4: Testing connection...");
    if (!testConnection()) {
        return false;
    }

    // Step 3: Copy prebuilt directory (NO BUILD)
    DEBUG_LOG(getLogPrefix() << " Step 2/4: Copying prebuilt binaries...");
    if (!copyDirectory(prebuilt_path)) {
        std::cerr << getLogPrefix() << " Failed to copy prebuilt directory" << std::endl;
        return false;
    }

    // Make executable
    std::string chmod_cmd = buildSSHCommand("find " + remote_project_path + " -type f -exec chmod +x {} \\;");
    g_systemCommand.execute(chmod_cmd);

    // Step 4: Run with output redirected to log file
    DEBUG_LOG(getLogPrefix() << " Step 3/4: Running application...");

    // Find executable
    std::string check_cmd = "test -f " + remote_project_path + "/" + executable_name + " && echo 'found'";
    std::string ssh_check = buildSSHCommand(check_cmd);
    auto check_result = g_systemCommand.execute(ssh_check);

    std::string executable_path;
    if (check_result.output.find("found") != std::string::npos) {
        executable_path = remote_project_path + "/" + executable_name;
    } else {
        // Try dpdk_app naming
        check_cmd = "test -f " + remote_project_path + "/dpdk_app && echo 'found'";
        ssh_check = buildSSHCommand(check_cmd);
        check_result = g_systemCommand.execute(ssh_check);
        if (check_result.output.find("found") != std::string::npos) {
            executable_path = remote_project_path + "/dpdk_app";
            executable_name = "dpdk_app";
            remote_log_path = "/tmp/" + executable_name + ".log";
        } else {
            std::cerr << getLogPrefix() << " Executable not found!" << std::endl;
            return false;
        }
    }

    // Build run command with sudo and output to log file
    std::string run_command = "cd " + remote_project_path + " && "
                              "echo '" + m_password + "' | sudo -S " + executable_path;
    if (!run_args.empty()) {
        run_command += " " + run_args;
    }
    run_command += " 2>&1 | tee " + remote_log_path;

    std::string ssh_run = buildSSHCommand(run_command);

    int timeout_ms = timeout_seconds > 0 ? timeout_seconds * 1000 : 120000;
    auto run_result = g_systemCommand.execute(ssh_run, timeout_ms);

    // Show output (always visible - shows actual test results)
    if (!run_result.output.empty()) {
        std::cout << getLogPrefix() << " Application output:\n" << run_result.output << std::endl;
    }

    if (!run_result.success) {
        std::cerr << getLogPrefix() << " Application execution had issues: " << run_result.error << std::endl;
    }

    // Step 5: Fetch log file
    DEBUG_LOG(getLogPrefix() << " Step 4/4: Fetching log file...");
    if (!fetchFile(remote_log_path, local_log_path)) {
        std::cerr << getLogPrefix() << " Warning: Could not fetch log file" << std::endl;
    }

    DEBUG_LOG(getLogPrefix() << " ========================================");
    DEBUG_LOG(getLogPrefix() << " Prebuilt Pipeline completed!");
    DEBUG_LOG(getLogPrefix() << " Log saved to: " << local_log_path);
    DEBUG_LOG(getLogPrefix() << " ========================================");

    return true;
}

bool SSHDeployer::preparePrebuilt(const std::string& source_dir,
                                   const std::string& app_name,
                                   BuildSystem build_system,
                                   const std::string& make_args) {
    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Prepare Prebuilt Binary");
    DEBUG_LOG(getLogPrefix() << " Source: " << source_dir);
    DEBUG_LOG("========================================");

    // Step 1: Resolve source path
    std::string resolved_path = source_dir;
    if (!source_dir.empty() && source_dir[0] != '/') {
        resolved_path = getSourceRoot() + "/" + source_dir;
    }

    if (!std::filesystem::exists(resolved_path)) {
        std::cerr << getLogPrefix() << " Source directory not found: " << resolved_path << std::endl;
        return false;
    }

    std::string folder_name = std::filesystem::path(resolved_path).filename().string();
    std::string actual_app_name = app_name.empty() ? folder_name : app_name;

    // Step 2: Deploy source and build on server (no run)
    DEBUG_LOG("\n[Step 1/3] Deploying source and building on server...");

    // For DPDK, use static build
    std::string actual_make_args = make_args;
    if (folder_name == "dpdk" && make_args.find("static") == std::string::npos) {
        // We'll build static for prebuilt
    }

    if (!deployAndBuild(source_dir, actual_app_name, false, false, build_system, "", actual_make_args, false)) {
        std::cerr << getLogPrefix() << " Build on server failed!" << std::endl;
        return false;
    }

    // Step 3: Determine remote binary path
    std::string remote_project_path = m_remote_directory + "/" + actual_app_name;
    std::string remote_binary_path;

    BuildSystem actual_build_system = build_system;
    if (build_system == BuildSystem::AUTO) {
        actual_build_system = detectBuildSystem(remote_project_path);
    }

    if (actual_build_system == BuildSystem::CMAKE) {
        // CMake: binary in build/bin/ or build/
        std::string check_bin = buildSSHCommand("test -f " + remote_project_path + "/build/bin/" + actual_app_name + " && echo 'found'");
        auto result = g_systemCommand.execute(check_bin);
        if (result.output.find("found") != std::string::npos) {
            remote_binary_path = remote_project_path + "/build/bin/" + actual_app_name;
        } else {
            check_bin = buildSSHCommand("test -f " + remote_project_path + "/build/" + actual_app_name + " && echo 'found'");
            result = g_systemCommand.execute(check_bin);
            if (result.output.find("found") != std::string::npos) {
                remote_binary_path = remote_project_path + "/build/" + actual_app_name;
            }
        }
    } else if (actual_build_system == BuildSystem::MAKEFILE) {
        // Makefile: binary in project root
        // Check dpdk_app first (DPDK convention)
        std::string check_dpdk = buildSSHCommand("test -f " + remote_project_path + "/dpdk_app && echo 'found'");
        auto result = g_systemCommand.execute(check_dpdk);
        if (result.output.find("found") != std::string::npos) {
            remote_binary_path = remote_project_path + "/dpdk_app";
            actual_app_name = "dpdk_app";
        } else {
            // Check for static version
            std::string check_static = buildSSHCommand("test -f " + remote_project_path + "/dpdk_app-static && echo 'found'");
            result = g_systemCommand.execute(check_static);
            if (result.output.find("found") != std::string::npos) {
                remote_binary_path = remote_project_path + "/dpdk_app-static";
            } else {
                remote_binary_path = remote_project_path + "/" + actual_app_name;
            }
        }
    }

    if (remote_binary_path.empty()) {
        std::cerr << getLogPrefix() << " Could not find compiled binary on server!" << std::endl;
        return false;
    }

    DEBUG_LOG(getLogPrefix() << " Remote binary: " << remote_binary_path);

    // Step 4: Create local prebuilt directory
    DEBUG_LOG("\n[Step 2/3] Creating prebuilt directory...");
    std::string prebuilt_root = getPrebuiltRoot();
    std::string prebuilt_dir = prebuilt_root + "/" + folder_name;

    std::filesystem::create_directories(prebuilt_dir);
    DEBUG_LOG(getLogPrefix() << " Created: " << prebuilt_dir);

    // Step 5: Fetch binary from server
    DEBUG_LOG("\n[Step 3/3] Fetching compiled binary...");
    std::string local_binary_path = prebuilt_dir + "/" + actual_app_name;

    if (!fetchFile(remote_binary_path, local_binary_path)) {
        std::cerr << getLogPrefix() << " Failed to fetch binary!" << std::endl;
        return false;
    }

    // Make local binary executable
    std::filesystem::permissions(local_binary_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);

    // Step 6: Copy runtime files (non-source, non-build files)
    // For DPDK: AteCumulus/AteTestMode/interfaces
    std::string source_ate_dir = resolved_path + "/AteCumulus";
    if (std::filesystem::exists(source_ate_dir)) {
        std::string prebuilt_ate_dir = prebuilt_dir + "/AteCumulus";
        std::filesystem::create_directories(prebuilt_ate_dir);
        std::filesystem::copy(source_ate_dir, prebuilt_ate_dir,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        DEBUG_LOG(getLogPrefix() << " Copied runtime files: AteCumulus/");
    }

    DEBUG_LOG("\n========================================");
    DEBUG_LOG(getLogPrefix() << " Prebuilt binary prepared successfully!");
    DEBUG_LOG(getLogPrefix() << " Location: " << prebuilt_dir);
    DEBUG_LOG("========================================\n");

    return true;
}