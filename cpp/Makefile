# ============================================================================
# Makefile for KDEConnect SMS Win32 (MinGW-w64 / MSYS2 / Clang GNU Make)
# ============================================================================

CXX ?= g++
WINDRES ?= windres
MKDIR ?= mkdir -p
RM ?= rm -rf

# Compiler and Linker Flags
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Iinclude
LDFLAGS_COMMON = -lws2_32 -lcrypt32 -lsecur32 -lshlwapi -lole32 -lshell32 -luuid
LDFLAGS_UI = -mwindows -municode -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lcomctl32 $(LDFLAGS_COMMON)
LDFLAGS_DAEMON = -municode $(LDFLAGS_COMMON)

# Directories
SRC_DIR = src
INC_DIR = include
RES_DIR = resources
OBJ_DIR = obj
BIN_DIR = bin

# Output Executables
TARGET_UI = $(BIN_DIR)/KDEConnectSMS.exe
TARGET_DAEMON = $(BIN_DIR)/KDEConnectDaemon.exe

# Source Files
CORE_SRCS = $(SRC_DIR)/theme_manager.cpp \
            $(SRC_DIR)/kdeconnect_protocol.cpp \
            $(SRC_DIR)/toast_notifier.cpp \
            $(SRC_DIR)/daemon.cpp

UI_SRCS = $(SRC_DIR)/direct2d_renderer.cpp \
          $(SRC_DIR)/main_ui.cpp

DAEMON_SRCS = $(SRC_DIR)/main_daemon.cpp

# Object Files
CORE_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CORE_SRCS))
UI_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(UI_SRCS))
DAEMON_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(DAEMON_SRCS))
RES_OBJ = $(OBJ_DIR)/app_res.o

# Default Target
.PHONY: all clean help dirs

all: dirs $(TARGET_UI) $(TARGET_DAEMON)
	@echo "============================================================"
	@echo "[SUCCESS] Build completed successfully!"
	@echo "  -> $(TARGET_UI) (GUI Client with Direct2D)"
	@echo "  -> $(TARGET_DAEMON) (Headless Sync Daemon)"
	@echo "============================================================"

dirs:
	@$(MKDIR) $(OBJ_DIR) $(BIN_DIR)

# Resource Compilation
$(RES_OBJ): $(RES_DIR)/app.rc $(RES_DIR)/app.manifest
	@echo "[RC] $< -> $@"
	$(WINDRES) -i $< -o $@ --input-format=rc --output-format=coff

# C++ Source Compilation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Target: Win32 Direct2D GUI Client
$(TARGET_UI): $(UI_OBJS) $(CORE_OBJS) $(RES_OBJ)
	@echo "[LINK] $@"
	$(CXX) -o $@ $^ $(LDFLAGS_UI)

# Target: Headless Background Daemon
$(TARGET_DAEMON): $(DAEMON_OBJS) $(CORE_OBJS) $(RES_OBJ)
	@echo "[LINK] $@"
	$(CXX) -o $@ $^ $(LDFLAGS_DAEMON)

# Clean Build Artifacts
clean:
	@echo "[CLEAN] Removing object files and binaries..."
	$(RM) $(OBJ_DIR) $(BIN_DIR)

# Help
help:
	@echo "Available make targets:"
	@echo "  all      - Build both KDEConnectSMS.exe and KDEConnectDaemon.exe (default)"
	@echo "  clean    - Remove build artifacts (obj/ and bin/)"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Custom environment options:"
	@echo "  CXX      - C++ compiler (default: g++)"
	@echo "  CXXFLAGS - Custom compiler flags"
