#!/bin/bash
#
# prepare_release.sh - One-time binary preparation script
#
# This script compiles all components and collects binaries into MMUComputerTestSoftware/
# For server-side components (dpdk, RemoteConfigSender), it:
#   1. Copies source code to server via SCP
#   2. Builds on server (where DPDK libraries are installed)
#   3. Fetches compiled binary back to local MMUComputerTestSoftware/ directory
#
# The resulting MMUComputerTestSoftware/ directory is fully portable -
# it can be copied anywhere and will work independently.
#
# Usage:
#   ./prepare_release.sh              # Prepare all components
#   ./prepare_release.sh dpdk         # Prepare only DPDK
#   ./prepare_release.sh dpdk_vmc     # Prepare only DPDK VMC
#   ./prepare_release.sh rcs          # Prepare only RemoteConfigSender
#   ./prepare_release.sh main         # Prepare only MainSoftware
#   ./prepare_release.sh pdf          # Prepare only PdfReportGenerator
#   ./prepare_release.sh assets       # Copy runtime assets only (config files)
#

set -e

# ==================== Configuration ====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREBUILT_DIR="$SCRIPT_DIR/MMUComputerTestSoftware"

# Server SSH configuration (must match SSHDeployer settings)
SERVER_HOST="10.1.33.2"
SERVER_USER="user"
SERVER_PASS="q"
SERVER_DIR="/home/user/Desktop"

# SSH/SCP common flags
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"
SSH_CMD="sshpass -p '$SERVER_PASS' ssh $SSH_OPTS $SERVER_USER@$SERVER_HOST"
SCP_CMD="sshpass -p '$SERVER_PASS' scp $SSH_OPTS"
SCP_R_CMD="sshpass -p '$SERVER_PASS' scp -r $SSH_OPTS"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# ==================== Helper Functions ====================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo ""
    echo "========================================"
    echo -e "${GREEN}$1${NC}"
    echo "========================================"
}

ssh_exec() {
    eval $SSH_CMD "\"$1\""
}

test_server_connection() {
    log_info "Testing connection to $SERVER_HOST..."
    if ssh_exec "echo 'Connection OK'" > /dev/null 2>&1; then
        log_info "Server connection successful!"
        return 0
    else
        log_error "Cannot connect to server $SERVER_HOST"
        return 1
    fi
}

# ==================== Component Builders ====================

prepare_dpdk() {
    log_step "Preparing DPDK (compile on server, fetch binary)"

    # Step 1: Copy source to server
    log_info "Copying DPDK source to server..."
    ssh_exec "rm -rf $SERVER_DIR/dpdk"
    eval $SCP_R_CMD "$SCRIPT_DIR/dpdk" "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/"

    # Step 2: Build on server (standard build, not static - to keep DPDK dynamic linking)
    log_info "Building DPDK on server..."
    ssh_exec "cd $SERVER_DIR/dpdk && make clean 2>/dev/null || true"
    ssh_exec "cd $SERVER_DIR/dpdk && make -j\$(nproc)"

    # Step 3: Create prebuilt directory
    mkdir -p "$PREBUILT_DIR/dpdk/AteCumulus/AteTestMode"

    # Step 4: Fetch compiled binary
    log_info "Fetching compiled dpdk_app binary..."
    eval $SCP_CMD "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/dpdk/dpdk_app" "$PREBUILT_DIR/dpdk/dpdk_app"
    chmod +x "$PREBUILT_DIR/dpdk/dpdk_app"

    # Step 5: Copy runtime files
    cp -r "$SCRIPT_DIR/dpdk/AteCumulus/AteTestMode/interfaces" "$PREBUILT_DIR/dpdk/AteCumulus/AteTestMode/"

    log_info "DPDK prepared successfully: $PREBUILT_DIR/dpdk/dpdk_app"
}

prepare_dpdk_vmc() {
    log_step "Preparing DPDK VMC (compile on server, fetch binary)"

    # Step 1: Copy source to server
    log_info "Copying DPDK VMC source to server..."
    ssh_exec "rm -rf $SERVER_DIR/dpdk_vmc"
    eval $SCP_R_CMD "$SCRIPT_DIR/dpdk_vmc" "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/"

    # Step 2: Build on server (standard build, not static - to keep DPDK dynamic linking)
    log_info "Building DPDK VMC on server..."
    ssh_exec "cd $SERVER_DIR/dpdk_vmc && make clean 2>/dev/null || true"
    ssh_exec "cd $SERVER_DIR/dpdk_vmc && make -j\$(nproc)"

    # Step 3: Create prebuilt directory
    mkdir -p "$PREBUILT_DIR/dpdk_vmc/AteCumulus/AteTestMode"

    # Step 4: Fetch compiled binary
    log_info "Fetching compiled dpdk_app binary (VMC)..."
    eval $SCP_CMD "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/dpdk_vmc/dpdk_app" "$PREBUILT_DIR/dpdk_vmc/dpdk_app"
    chmod +x "$PREBUILT_DIR/dpdk_vmc/dpdk_app"

    # Step 5: Copy runtime files
    cp -r "$SCRIPT_DIR/dpdk_vmc/AteCumulus/AteTestMode/interfaces" "$PREBUILT_DIR/dpdk_vmc/AteCumulus/AteTestMode/"

    log_info "DPDK VMC prepared successfully: $PREBUILT_DIR/dpdk_vmc/dpdk_app"
}

prepare_remote_config_sender() {
    log_step "Preparing RemoteConfigSender (compile on server, fetch binary)"

    # Step 1: Copy source to server
    log_info "Copying RemoteConfigSender source to server..."
    ssh_exec "rm -rf $SERVER_DIR/RemoteConfigSender"
    eval $SCP_R_CMD "$SCRIPT_DIR/RemoteConfigSender" "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/"

    # Step 2: Build on server with CMake
    log_info "Building RemoteConfigSender on server..."
    ssh_exec "cd $SERVER_DIR/RemoteConfigSender && mkdir -p build && cd build && cmake .. && make -j\$(nproc)"

    # Step 3: Create prebuilt directory
    mkdir -p "$PREBUILT_DIR/RemoteConfigSender"

    # Step 4: Fetch compiled binary
    log_info "Fetching compiled RemoteConfigSender binary..."
    eval $SCP_CMD "$SERVER_USER@$SERVER_HOST:$SERVER_DIR/RemoteConfigSender/build/RemoteConfigSender" "$PREBUILT_DIR/RemoteConfigSender/RemoteConfigSender"
    chmod +x "$PREBUILT_DIR/RemoteConfigSender/RemoteConfigSender"

    log_info "RemoteConfigSender prepared: $PREBUILT_DIR/RemoteConfigSender/RemoteConfigSender"
}

prepare_main_software() {
    log_step "Preparing MainSoftware (compile locally)"

    # Step 1: Build locally with CMake
    log_info "Building MainSoftware locally..."
    mkdir -p "$SCRIPT_DIR/MainSoftware/build"
    cd "$SCRIPT_DIR/MainSoftware/build"
    cmake ..
    make -j$(nproc)
    cd "$SCRIPT_DIR"

    # Step 2: Create prebuilt directory
    mkdir -p "$PREBUILT_DIR/MainSoftware/bin"

    # Step 3: Copy binary
    log_info "Copying mainSoftware binary to prebuilt..."
    cp "$SCRIPT_DIR/MainSoftware/build/bin/mainSoftware" "$PREBUILT_DIR/MainSoftware/bin/mainSoftware"
    chmod +x "$PREBUILT_DIR/MainSoftware/bin/mainSoftware"

    log_info "MainSoftware prepared: $PREBUILT_DIR/MainSoftware/bin/mainSoftware"
}

prepare_pdf_report_generator() {
    log_step "Preparing PdfReportGenerator (compile Python to binary with PyInstaller)"

    local PDF_SRC="$SCRIPT_DIR/PdfReportGenerator"
    local PDF_DST="$PREBUILT_DIR/PdfReportGenerator"

    # Step 1: Check PyInstaller is installed
    if ! command -v pyinstaller &> /dev/null; then
        log_warn "PyInstaller not found, installing..."
        pip3 install pyinstaller
    fi

    # Step 2: Compile with PyInstaller (single file binary)
    log_info "Compiling ReportGenerator.py with PyInstaller..."
    pyinstaller --onefile \
        --name ReportGenerator \
        --distpath "$PDF_SRC/dist" \
        --workpath "$PDF_SRC/build_pyinstaller" \
        --specpath "$PDF_SRC" \
        --clean \
        "$PDF_SRC/ReportGenerator.py"

    # Step 3: Create prebuilt directory and copy binary
    mkdir -p "$PDF_DST/Assets"
    cp "$PDF_SRC/dist/ReportGenerator" "$PDF_DST/ReportGenerator"
    chmod +x "$PDF_DST/ReportGenerator"

    # Step 4: Copy assets (logo etc.)
    cp "$PDF_SRC/Assets/company_logo.png" "$PDF_DST/Assets/"

    # Step 5: Clean PyInstaller build artifacts
    rm -rf "$PDF_SRC/dist" "$PDF_SRC/build_pyinstaller" "$PDF_SRC/ReportGenerator.spec"

    log_info "PdfReportGenerator compiled: $PDF_DST/ReportGenerator"
}

prepare_assets() {
    log_step "Copying runtime config files to prebuilt"

    # CumulusInterfaces
    mkdir -p "$PREBUILT_DIR/CumulusInterfaces/DTNIRSW"
    cp "$SCRIPT_DIR/CumulusInterfaces/DTNIRSW/interfaces" "$PREBUILT_DIR/CumulusInterfaces/DTNIRSW/"
    mkdir -p "$PREBUILT_DIR/CumulusInterfaces/VMC"
    cp "$SCRIPT_DIR/CumulusInterfaces/VMC/interfaces" "$PREBUILT_DIR/CumulusInterfaces/VMC/"
    log_info "CumulusInterfaces config copied"

    # DPDK runtime files
    mkdir -p "$PREBUILT_DIR/dpdk/AteCumulus/AteTestMode"
    cp "$SCRIPT_DIR/dpdk/AteCumulus/AteTestMode/interfaces" "$PREBUILT_DIR/dpdk/AteCumulus/AteTestMode/"
    log_info "DPDK runtime files copied"

    # DPDK VMC runtime files
    mkdir -p "$PREBUILT_DIR/dpdk_vmc/AteCumulus/AteTestMode"
    cp "$SCRIPT_DIR/dpdk_vmc/AteCumulus/AteTestMode/interfaces" "$PREBUILT_DIR/dpdk_vmc/AteCumulus/AteTestMode/"
    log_info "DPDK VMC runtime files copied"
}

# ==================== Main ====================

main() {
    log_step "Prebuilt Binary Preparation"
    log_info "Project root: $SCRIPT_DIR"
    log_info "Prebuilt dir: $PREBUILT_DIR"

    # Create prebuilt root
    mkdir -p "$PREBUILT_DIR"

    # Check which component to prepare
    COMPONENT="${1:-all}"

    case "$COMPONENT" in
        dpdk)
            test_server_connection
            prepare_dpdk
            ;;
        dpdk_vmc)
            test_server_connection
            prepare_dpdk_vmc
            ;;
        rcs|remoteconfig|RemoteConfigSender)
            test_server_connection
            prepare_remote_config_sender
            ;;
        main|MainSoftware)
            prepare_main_software
            ;;
        pdf|PdfReportGenerator)
            prepare_pdf_report_generator
            ;;
        assets)
            prepare_assets
            ;;
        all)
            # For server components, test connection first
            test_server_connection

            prepare_main_software
            prepare_remote_config_sender
            prepare_dpdk
            prepare_dpdk_vmc
            prepare_pdf_report_generator
            prepare_assets
            ;;
        *)
            log_error "Unknown component: $COMPONENT"
            echo "Usage: $0 [all|dpdk|dpdk_vmc|rcs|main|pdf|assets]"
            exit 1
            ;;
    esac

    log_step "Preparation Complete!"
    echo ""
    log_info "Prebuilt directory contents:"
    find "$PREBUILT_DIR" -type f ! -name '.gitkeep' ! -name '.gitignore' | sort | while read -r f; do
        size=$(du -h "$f" | cut -f1)
        echo "  $size  $f"
    done
    echo ""
    log_info "You can now run the software without source code compilation."
    log_info "The prebuilt binaries will be deployed directly to the server."
}

main "$@"
