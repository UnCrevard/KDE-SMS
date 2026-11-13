# ==============================================================================
# Makefile for KDEConnect SMS Win32
# Specifically designed for mingw32-make (MinGW-w64) on Windows cmd.exe & MSYS2
# ==============================================================================

CXX ?= g++
WINDRES ?= windres

# Enforce cmd.exe under Windows to prevent sh.exe / cmd.exe mixing and avoid '-p' folder creation
ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  .SHELLFLAGS := /c
  # Native Windows commands without '-p' flag
  # Under cmd.exe: 'mkdir dir' works; 'mkdir -p dir' creates a folder named '-p'
  MKDIR_OBJ = if not exist "obj" mkdir "obj"
  MKDIR_BIN = if not exist "bin" mkdir "bin"
  RM_OBJ    = if exist "obj" rmdir /S /Q "obj"
  RM_BIN    = if exist "bin" rmdir /S /Q "bin"
  RM_EXTRA  = if exist "-p" rmdir /S /Q "-p"
else
  # Fallback for Linux / Unix environments
  MKDIR_OBJ = mkdir -p obj
  MKDIR_BIN = mkdir -p bin
  RM_OBJ    = rm -rf obj
  RM_BIN    = rm -rf bin
  RM_EXTRA  = rm -rf "-p"
endif

# Compiler & Linker Flags
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Iinclude
LDFLAGS_COMMON = -s -lws2_32 -lcrypt32 -lsecur32 -lshlwapi -lole32 -lshell32 -luuid -lcomdlg32 -ladvapi32 -ldwmapi
LDFLAGS_UI = -mwindows -municode -ld2d1 -ldwrite -lwindowscodecs -lcomctl32 $(LDFLAGS_COMMON)
LDFLAGS_DAEMON = -municode $(LDFLAGS_COMMON)

# Directories
SRC_DIR = src
INC_DIR = include
RES_DIR = resources
OBJ_DIR = obj
BIN_DIR = bin

# Target executables
TARGET_UI = $(BIN_DIR)/KDEConnectSMS.exe
TARGET_DAEMON = $(BIN_DIR)/KDEConnectDaemon.exe

# Sources
CORE_SRCS = $(SRC_DIR)/theme_manager.cpp \
            $(SRC_DIR)/kdeconnect_protocol.cpp \
            $(SRC_DIR)/toast_notifier.cpp \
            $(SRC_DIR)/daemon.cpp

UI_SRCS = $(SRC_DIR)/direct2d_renderer.cpp \
          $(SRC_DIR)/main_ui.cpp

DAEMON_SRCS = $(SRC_DIR)/main_daemon.cpp

# Object files
CORE_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CORE_SRCS))
UI_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(UI_SRCS))
DAEMON_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(DAEMON_SRCS))
RES_OBJ = $(OBJ_DIR)/app_res.o

# Phony targets
.PHONY: all clean help dirs

# Default target
all: dirs $(TARGET_UI) $(TARGET_DAEMON)
	@echo ============================================================
	@echo [SUCCESS] Build completed successfully with mingw32-make!
	@echo   GUI Client : $(TARGET_UI)
	@echo   Sync Daemon: $(TARGET_DAEMON)
	@echo ============================================================

# Directory creation without -p flag (guaranteed no '-p' folder under Windows cmd.exe)
dirs:
	@$(MKDIR_OBJ)
	@$(MKDIR_BIN)

# Windows Resource script compilation with windres
$(RES_OBJ): $(RES_DIR)/app.rc $(RES_DIR)/app.manifest | dirs
	@echo [RC] $<
	$(WINDRES) -I $(RES_DIR) -I. -i $< -o $@ --input-format=rc --output-format=coff

# C++ Object compilation with order-only dependency on dirs
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | dirs
	@echo [CXX] $<
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link: Win32 Direct2D GUI Client
$(TARGET_UI): $(UI_OBJS) $(CORE_OBJS) $(RES_OBJ) | dirs
	@echo [LINK] $@
	$(CXX) -o $@ $(UI_OBJS) $(CORE_OBJS) $(RES_OBJ) $(LDFLAGS_UI)

# Link: Headless Background Daemon
$(TARGET_DAEMON): $(DAEMON_OBJS) $(CORE_OBJS) $(RES_OBJ) | dirs
	@echo [LINK] $@
	$(CXX) -o $@ $(DAEMON_OBJS) $(CORE_OBJS) $(RES_OBJ) $(LDFLAGS_DAEMON)

# Clean: removes obj, bin, and any previously created '-p' folder
clean:
	@echo [CLEAN] Cleaning object and binary directories...
	@$(RM_OBJ)
	@$(RM_BIN)
	@$(RM_EXTRA)
	@echo [CLEAN] Done.

help:
	@echo Available targets for mingw32-make:
	@echo   mingw32-make          Build both executables (default)
	@echo   mingw32-make clean    Delete build artifacts and accidental -p folder
	@echo   mingw32-make help     Show this help
