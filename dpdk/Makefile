# DPDK Application Makefile
APP = dpdk_app
CC = gcc

# Directories
SRCDIR = src
INCDIR = include
EMBLATDIR = src/EmbeddedLatency
PTPDIR = src/Ptp
HEALTHDIR = src/HealthMonitor
PSUTELEMDIR = src/PsuTelemetry

NUM_TX_CORES ?= 4
NUM_RX_CORES ?= 4
TARGET_GBPS_FAST ?= 3.74
TARGET_GBPS_MID ?= 3.50
TARGET_GBPS_SLOW ?= 3.69
USE_VLAN ?=1

# Enable raw socket ports (non-DPDK NICs)
ENABLE_RAW_SOCKET_PORTS ?= 1

# DTN port-based statistics mode (1=DTN 34-port table, 0=Server 8-port table)
STATS_MODE_DTN ?= 1

# ATE root directory (auto-detected at build time)
ATE_ROOT := $(shell pwd)/AteCumulus/AteTestMode

# Compiler flags
CFLAGS = -O3 -march=native -flto -ffast-math -funroll-loops -Wextra -I$(INCDIR) -I$(SRCDIR) -DNUM_TX_CORES=$(NUM_TX_CORES) -DNUM_RX_CORES=$(NUM_RX_CORES) -DUSE_VLAN=$(USE_VLAN) -DTARGET_GBPS_FAST=$(TARGET_GBPS_FAST) -DTARGET_GBPS_MID=$(TARGET_GBPS_MID) -DTARGET_GBPS_SLOW=$(TARGET_GBPS_SLOW) -DENABLE_RAW_SOCKET_PORTS=$(ENABLE_RAW_SOCKET_PORTS) -DSTATS_MODE_DTN=$(STATS_MODE_DTN) -DATE_ROOT=\"$(ATE_ROOT)\"
DEBUG_CFLAGS = -g -O3 -DDEBUG -march=native -Wall -Wextra -I$(INCDIR) -I$(SRCDIR) -DENABLE_RAW_SOCKET_PORTS=$(ENABLE_RAW_SOCKET_PORTS) -DATE_ROOT=\"$(ATE_ROOT)\"

# Additional libraries for raw socket ports (pthread for threading)
EXTRA_LIBS = -lpthread

# Source files (include embedded latency, PTP, health monitor, PSU telemetry)
SOURCES = $(wildcard $(SRCDIR)/*.c) $(wildcard $(EMBLATDIR)/*.c) $(wildcard $(PTPDIR)/*.c) $(wildcard $(HEALTHDIR)/*.c) $(wildcard $(PSUTELEMDIR)/*.c)

# DPDK flags
DPDK_FLAGS = $(shell pkg-config --cflags --libs libdpdk)
DPDK_STATIC_FLAGS = $(shell pkg-config --static --cflags --libs libdpdk)

# Check if DPDK is available
DPDK_CHECK := $(shell pkg-config --exists libdpdk && echo "yes" || echo "no")
ifeq ($(DPDK_CHECK), no)
    $(error "DPDK not found! Install DPDK and ensure pkg-config can find it")
endif

# Default target
.PHONY: all clean debug static run run-daemon stop log log-follow info help

all: $(APP)

# Build application
$(APP):
	@echo "Building $(APP)..."
	@echo "Sources: $(SOURCES)"
	@echo "Raw Socket Ports: $(ENABLE_RAW_SOCKET_PORTS)"
	$(CC) $(CFLAGS) $(SOURCES) -o $(APP) $(DPDK_FLAGS) $(EXTRA_LIBS)
	@echo "✓ Build completed: $(APP)"

# Debug build
debug:
	@echo "Building debug version..."
	$(CC) $(DEBUG_CFLAGS) $(SOURCES) -o $(APP)-debug $(DPDK_FLAGS) $(EXTRA_LIBS)
	@echo "✓ Debug build completed: $(APP)-debug"

# Static build
static:
	@echo "Building static version..."
	$(CC) $(CFLAGS) $(SOURCES) -o $(APP)-static $(DPDK_STATIC_FLAGS) $(EXTRA_LIBS)
	@echo "✓ Static build completed: $(APP)-static"

# Clean
clean:
	@echo "Cleaning..."
	@rm -f $(APP) $(APP)-debug $(APP)-static
	@echo "✓ Clean completed"

# Run with basic EAL parameters (foreground mode - for direct server usage)
run: $(APP)
	@echo "Running $(APP) in FOREGROUND mode..."
	@echo "Press Ctrl+C to stop"
	sudo ./$(APP) -l 0-255 -n 16

# Run in daemon/background mode (for remote execution from main PC)
run-daemon: $(APP)
	@echo "Running $(APP) in DAEMON mode..."
	@echo "After latency tests, DPDK will fork to background"
	@echo "Log file: /tmp/dpdk_app.log"
	sudo ./$(APP) --daemon -l 0-255 -n 16

# Stop DPDK if running in background
stop:
	@echo "Stopping $(APP)..."
	@sudo pkill -f $(APP) 2>/dev/null || echo "$(APP) was not running"
	@echo "✓ Stop command sent"

# Show DPDK log (for daemon mode)
log:
	@tail -100 /tmp/dpdk_app.log 2>/dev/null || echo "Log file not found"

# Follow DPDK log in real-time
log-follow:
	@tail -f /tmp/dpdk_app.log

# Show build information
info:
	@echo "=== Build Configuration ==="
	@echo "Application: $(APP)"
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "Sources: $(SOURCES)"
	@echo "Include dir: $(INCDIR)"
	@echo "DPDK available: $(DPDK_CHECK)"

# Help
help:
	@echo "Available targets:"
	@echo "  all        - Build application (default)"
	@echo "  debug      - Build with debug symbols"
	@echo "  static     - Build with static linking"
	@echo "  clean      - Remove build artifacts"
	@echo ""
	@echo "Run targets:"
	@echo "  run        - Run in FOREGROUND (for direct server usage)"
	@echo "  run-daemon - Run in DAEMON mode (forks to background after latency tests)"
	@echo "  stop       - Stop DPDK if running in background"
	@echo ""
	@echo "Log targets (for daemon mode):"
	@echo "  log        - Show last 100 lines of log"
	@echo "  log-follow - Follow log in real-time (tail -f)"
	@echo ""
	@echo "Info:"
	@echo "  info       - Show build configuration"
	@echo "  help       - Show this help"
