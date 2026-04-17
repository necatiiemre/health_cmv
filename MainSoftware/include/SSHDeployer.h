#ifndef SSH_DEPLOYER_H
#define SSH_DEPLOYER_H

#include <string>

/**
 * @brief SSH connection configuration structure
 */
struct SSHConfig {
    std::string host;
    std::string username;
    std::string password;
    std::string remote_directory;
    std::string name;  // Deployer name for logging (e.g., "Server", "Cumulus")

    SSHConfig() = default;

    SSHConfig(const std::string& h, const std::string& u,
              const std::string& p, const std::string& dir,
              const std::string& n = "")
        : host(h), username(u), password(p), remote_directory(dir), name(n) {}
};

/**
 * @brief Predefined SSH targets enum
 */
enum class SSHTarget {
    SERVER = 1,
    CUMULUS = 2
};

/**
 * @brief Build system type enum
 */
enum class BuildSystem {
    AUTO,       // Auto-detect: CMakeLists.txt -> CMAKE, Makefile -> MAKEFILE
    CMAKE,      // Use cmake + make
    MAKEFILE    // Use make directly (for DPDK-style projects)
};

/**
 * @brief SSH file deployment class for remote server operations
 *
 * Generic class - create multiple instances for different targets.
 *
 * Usage:
 * @code
 *   // Use predefined global instances
 *   g_ssh_deployer_server.deployAndBuild("/path/to/source", "app_name");
 *   g_ssh_deployer_cumulus.execute("ls -la");
 *
 *   // Or create custom instance
 *   SSHConfig config("192.168.1.100", "admin", "pass", "/home/admin", "CustomDevice");
 *   SSHDeployer customDeployer(config);
 *   customDeployer.testConnection();
 * @endcode
 */
class SSHDeployer {
public:
    /**
     * @brief Default constructor with empty configuration
     */
    SSHDeployer();

    /**
     * @brief Constructor with SSHConfig
     * @param config SSH configuration structure
     */
    explicit SSHDeployer(const SSHConfig& config);

    /**
     * @brief Constructor with individual parameters
     * @param host Server IP address
     * @param username SSH username
     * @param password SSH password
     * @param remote_directory Target directory on remote server
     * @param name Deployer name for logging
     */
    SSHDeployer(const std::string& host,
                const std::string& username,
                const std::string& password,
                const std::string& remote_directory,
                const std::string& name = "");

    ~SSHDeployer() = default;

    // Copy/move allowed for flexibility
    SSHDeployer(const SSHDeployer&) = default;
    SSHDeployer& operator=(const SSHDeployer&) = default;
    SSHDeployer(SSHDeployer&&) = default;
    SSHDeployer& operator=(SSHDeployer&&) = default;

    // ==================== Configuration ====================

    /**
     * @brief Configure deployer with SSHConfig
     * @param config SSH configuration structure
     */
    void configure(const SSHConfig& config);

    /**
     * @brief Set SSH connection credentials
     * @param host Server IP address
     * @param username SSH username
     * @param password SSH password
     */
    void setCredentials(const std::string& host,
                        const std::string& username,
                        const std::string& password);

    void setHost(const std::string& host);
    std::string getHost() const;

    void setUsername(const std::string& username);
    std::string getUsername() const;

    void setPassword(const std::string& password);

    /**
     * @brief Set target directory on remote server
     * @param path Remote directory path (default: /home/user)
     */
    void setRemoteDirectory(const std::string& path);
    std::string getRemoteDirectory() const;

    /**
     * @brief Set deployer name for logging
     * @param name Name to identify this deployer in logs
     */
    void setName(const std::string& name);
    std::string getName() const;

    // ==================== File Operations ====================

    /**
     * @brief Copy file to remote server (SCP)
     * @param local_path Local file path
     * @return true on success
     */
    bool copyFile(const std::string& local_path);

    /**
     * @brief Copy file and make it executable
     * @param local_path Local file path
     * @return true on success
     */
    bool deploy(const std::string& local_path);

    /**
     * @brief Copy directory to remote server (SCP -r)
     * @param local_dir Local directory path
     * @param remote_name Remote directory name (optional, uses same name if empty)
     * @return true on success
     */
    bool copyDirectory(const std::string& local_dir, const std::string& remote_name = "");

    /**
     * @brief Copy file to specific remote path (with sudo support)
     * @param local_path Local file path
     * @param remote_path Full remote path (e.g., /etc/network/interfaces)
     * @param use_sudo Use sudo for copying to protected directories
     * @return true on success
     *
     * Note: For sudo mode, file is first copied to /tmp then moved with sudo
     */
    bool copyFileToPath(const std::string& local_path, const std::string& remote_path, bool use_sudo = false);

    /**
     * @brief Fetch (download) file from remote server to local path
     * @param remote_path Full remote file path
     * @param local_path Local destination path
     * @return true on success
     */
    bool fetchFile(const std::string& remote_path, const std::string& local_path);

    /**
     * @brief Deploy, build, run application and fetch its log file
     * @param local_source_dir Local source directory
     * @param app_name Application name
     * @param run_args Arguments to pass when running the application
     * @param local_log_path Local path to save the log file
     * @param timeout_seconds Maximum time to wait for application to complete (0 = no timeout)
     * @return true on success
     *
     * This function:
     * 1. Copies source to remote server
     * 2. Builds the application
     * 3. Runs it (foreground, waits for completion)
     * 4. Fetches the log file back to local machine
     */
    bool deployBuildRunAndFetchLog(const std::string& local_source_dir,
                                    const std::string& app_name,
                                    const std::string& run_args,
                                    const std::string& local_log_path,
                                    int timeout_seconds = 120);

    // ==================== Command Execution ====================

    /**
     * @brief Execute command on remote server
     * @param command Command to execute
     * @param output Command output (optional)
     * @param use_sudo Run with sudo (optional)
     * @return true on success
     */
    bool execute(const std::string& command, std::string* output = nullptr, bool use_sudo = false, bool silent = false);

    /**
     * @brief Execute command in background on remote server (nohup)
     * @param command Command to execute
     * @return true on success
     */
    bool executeBackground(const std::string& command);

    /**
     * @brief Execute command interactively on remote server
     *
     * Uses system() with SSH -t flag to allocate a pseudo-terminal.
     * Allows user to provide input (stdin) directly from the terminal.
     * Blocks until the remote command completes.
     *
     * Use this for applications that require user interaction (y/n prompts, etc.)
     *
     * @param command Command to execute
     * @param use_sudo Run with sudo (optional)
     * @return true on success (exit code 0)
     */
    bool executeInteractive(const std::string& command, bool use_sudo = false);

    /**
     * @brief Run deployed application
     * @param app_name Application name
     * @param args Arguments (optional)
     * @return true on success
     */
    bool run(const std::string& app_name, const std::string& args = "");

    // ==================== Build & Deploy ====================

    /**
     * @brief Build project on remote server (auto-detects build system)
     * @param project_dir Project directory (inside remote_directory)
     * @param output_name Output executable name
     * @param build_system Build system to use (AUTO, CMAKE, or MAKEFILE)
     * @param make_args Additional arguments for make (e.g., "NUM_TX_CORES=4")
     * @return true on successful build
     */
    bool build(const std::string& project_dir,
               const std::string& output_name = "",
               BuildSystem build_system = BuildSystem::AUTO,
               const std::string& make_args = "");

    /**
     * @brief Copy source code, build, and run (full pipeline)
     * @param local_source_dir Local source directory
     * @param app_name Application name
     * @param run_after_build Run application after build?
     * @param use_sudo Run with sudo (required for raw sockets)
     * @param build_system Build system to use (AUTO, CMAKE, or MAKEFILE)
     * @param run_args Arguments to pass when running the application
     * @param make_args Additional arguments for make (e.g., "NUM_TX_CORES=4")
     * @param run_in_background Run application in background (for long-running apps like DPDK)
     * @return true on success
     */
    bool deployAndBuild(const std::string& local_source_dir,
                        const std::string& app_name,
                        bool run_after_build = true,
                        bool use_sudo = false,
                        BuildSystem build_system = BuildSystem::AUTO,
                        const std::string& run_args = "",
                        const std::string& make_args = "",
                        bool run_in_background = false);

    /**
     * @brief Stop a running application on remote server
     * @param app_name Application name to stop
     * @param use_sudo Use sudo for killing the process
     * @return true on success
     */
    bool stopApplication(const std::string& app_name, bool use_sudo = false);

    /**
     * @brief Check if an application is running on remote server
     * @param app_name Application name to check
     * @return true if running
     */
    bool isApplicationRunning(const std::string& app_name);

    // ==================== Prebuilt Binary Deploy ====================

    /**
     * @brief Get prebuilt binaries root directory
     * @return Path to prebuilt/ directory (inside project root)
     */
    static std::string getPrebuiltRoot();

    /**
     * @brief Deploy pre-built binary directory and run (NO compilation on server)
     *
     * Pipeline: test connection -> copy prebuilt dir -> chmod +x -> run
     *
     * @param prebuilt_dir Directory name inside prebuilt/ (e.g., "dpdk", "RemoteConfigSender")
     * @param app_name Executable name (auto-detect from dir name if empty)
     * @param run_after_deploy Run application after deploying?
     * @param use_sudo Run with sudo (required for raw sockets/DPDK)
     * @param run_args Arguments to pass when running the application
     * @param run_in_background Run in background (for long-running apps like DPDK)
     * @return true on success
     */
    bool deployPrebuilt(const std::string& prebuilt_dir,
                        const std::string& app_name = "",
                        bool run_after_deploy = true,
                        bool use_sudo = false,
                        const std::string& run_args = "",
                        bool run_in_background = false);

    /**
     * @brief Deploy pre-built binary, run and fetch log (NO compilation on server)
     *
     * Pipeline: test connection -> copy prebuilt dir -> run -> fetch log
     *
     * @param prebuilt_dir Directory name inside prebuilt/
     * @param app_name Executable name
     * @param run_args Arguments to pass when running
     * @param local_log_path Local path to save the fetched log file
     * @param timeout_seconds Maximum time to wait for application (0 = default 120s)
     * @return true on success
     */
    bool deployPrebuiltRunAndFetchLog(const std::string& prebuilt_dir,
                                       const std::string& app_name,
                                       const std::string& run_args,
                                       const std::string& local_log_path,
                                       int timeout_seconds = 120);

    /**
     * @brief Prepare pre-built binary: deploy source to server, build there, fetch binary back
     *
     * This is a ONE-TIME operation to prepare binaries for future prebuilt deployments.
     * Pipeline: copy source -> build on server -> fetch compiled binary -> save to prebuilt/
     *
     * @param source_dir Source directory name (e.g., "dpdk", "RemoteConfigSender")
     * @param app_name Application name to fetch back (auto-detect if empty)
     * @param build_system Build system to use (AUTO, CMAKE, or MAKEFILE)
     * @param make_args Additional make arguments (e.g., "NUM_TX_CORES=4")
     * @return true on success
     */
    bool preparePrebuilt(const std::string& source_dir,
                         const std::string& app_name = "",
                         BuildSystem build_system = BuildSystem::AUTO,
                         const std::string& make_args = "");

    // ==================== Utilities ====================

    /**
     * @brief Test connection to server
     * @return true if connection successful
     */
    bool testConnection();

    /**
     * @brief Check if credentials are configured
     * @return true if ready
     */
    bool isConfigured() const;

    /**
     * @brief Get executable's directory path
     * @return Path to directory containing the executable
     */
    static std::string getExecutableDir();

    /**
     * @brief Get source root directory (parent of build directory)
     * @return Path to source root directory
     */
    static std::string getSourceRoot();

private:
    std::string m_host;
    std::string m_username;
    std::string m_password;
    std::string m_remote_directory;
    std::string m_name;  // Deployer name for logging

    /**
     * @brief Get log prefix with deployer name
     */
    std::string getLogPrefix() const;

    /**
     * @brief Build SSH command string
     */
    std::string buildSSHCommand(const std::string& remote_command) const;

    /**
     * @brief Build SCP command string
     */
    std::string buildSCPCommand(const std::string& local_path,
                                 const std::string& remote_path) const;

    /**
     * @brief Detect build system type for a remote project
     * @param project_path Full path to project on remote server
     * @return Detected BuildSystem type
     */
    BuildSystem detectBuildSystem(const std::string& project_path);

    /**
     * @brief Build using CMake
     */
    bool buildWithCMake(const std::string& project_path, const std::string& output_name);

    /**
     * @brief Build using Makefile (for DPDK-style projects)
     */
    bool buildWithMakefile(const std::string& project_path,
                           const std::string& output_name,
                           const std::string& make_args = "");
};

// ==================== Global Instances ====================

/**
 * @brief SSH Deployer for Server (10.1.33.2)
 * Default credentials: user/q, directory: /home/user/Desktop
 */
extern SSHDeployer g_ssh_deployer_server;

/**
 * @brief SSH Deployer for Cumulus switch
 * Default credentials: cumulus/cumulus, directory: /home/cumulus
 */
extern SSHDeployer g_ssh_deployer_cumulus;

// Legacy alias for backward compatibility
extern SSHDeployer& g_Deployer;

#endif // SSH_DEPLOYER_H
