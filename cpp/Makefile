# ==============================================================================
# Makefile for KDEConnect SMS Win32
# Specifically designed for mingw32-make (MinGW-w64) on Windows cmd.exe & MSYS2
# ==============================================================================

CXX ?= g++
WINDRES ?= windres

# Windows cmd.exe vs Unix shell detection
ifeq ($(OS),Windows_NT)
  # Check if running under sh.exe (MSYS2 / Git Bash) or native Windows cmd.exe
  ifdef SHELL
    ifneq ($(findstring sh.exe,$(SHELL)),)
      UNIX_SHELL = 1
    endif
    ifneq ($(findstring /sh,$(SHELL)),)
      UNIX_SHELL = 1
    endif
  endif

  ifeq ($(UNIX_SHELL),1)
    MKDIR = mkdir -p
    RM = rm -rf
    SEP = /
  else
    # Native Windows cmd.exe (default for mingw32-make in Command Prompt / PowerShell)
    MKDIR = @if not exist $(subst /,\,$(1)) mkdir $(subst /,\,$(1))
    RM = @if exist $(subst /,\,$(1)) rmdir /S /Q $(subst /,\,$(1))
    SEP = \\
  endif
else
  MKDIR = mkdir -p
  RM = rm -rf
  SEP = /
endif

# Compiler & Linker Flags
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
.PHONY: all clean help

# Default target
all: dirs $(TARGET_UI) $(TARGET_DAEMON)
	@echo.
	@echo ============================================================
	@echo [SUCCESS] Build completed successfully with mingw32-make!
	@echo   GUI Client : $(TARGET_UI)
	@echo   Sync Daemon: $(TARGET_DAEMON)
	@echo ============================================================

dirs:
	$(call MKDIR,$(OBJ_DIR))
	$(call MKDIR,$(BIN_DIR))

# Windows Resource script compilation with windres
$(RES_OBJ): $(RES_DIR)/app.rc $(RES_DIR)/app.manifest
	@echo [RC] $<
	$(WINDRES) -I $(RES_DIR) -I. -i $< -o $@ --input-format=rc --output-format=coff

# C++ Object compilation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo [CXX] $<
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link: Win32 Direct2D GUI Client
$(TARGET_UI): $(UI_OBJS) $(CORE_OBJS) $(RES_OBJ)
	@echo [LINK] $@
	$(CXX) -o $@ $(UI_OBJS) $(CORE_OBJS) $(RES_OBJ) $(LDFLAGS_UI)

# Link: Headless Background Daemon
$(TARGET_DAEMON): $(DAEMON_OBJS) $(CORE_OBJS) $(RES_OBJ)
	@echo [LINK] $@
	$(CXX) -o $@ $(DAEMON_OBJS) $(CORE_OBJS) $(RES_OBJ) $(LDFLAGS_DAEMON)

# Clean
clean:
	@echo [CLEAN] Cleaning object and binary directories...
	$(call RM,$(OBJ_DIR))
	$(call RM,$(BIN_DIR))
	@echo [CLEAN] Done.

help:
	@echo Available targets for mingw32-make:
	@echo   mingw32-make          Build both executables (default)
	@echo   mingw32-make clean    Delete build artifacts (obj and bin)
	@echo   mingw32-make help     Show this help
