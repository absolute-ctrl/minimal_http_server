# c-httpd - pure C HTTP/1.1 server
#
# Windows (MSYS2 CLANG64 shell) and Linux/macOS are both supported:
#   make             debug build    -> build/app(.exe)
#   make release     optimized build (statically linked on Windows)
#   make run         build (debug) and serve ./public on http://localhost:8080
#   make sanitize    debug build with AddressSanitizer + UBSan (Linux/macOS)
#   make clean       remove build/
#
# Variables can be overridden on the command line, e.g.
#   make CC=gcc CSTD=-std=c2x      (GCC 13 spells C23 as c2x)
#   make run ARGS="-p 9000 -r ."

CC      := clang
TARGET  := app
SRC_DIR := src
BLD_DIR := build
OBJ_DIR := $(BLD_DIR)/obj
ARGS    ?=

# ---- platform detection -------------------------------------------------
ifeq ($(OS),Windows_NT)
  EXE           := .exe
  PLATFORM_LIBS := -lws2_32
  STATIC        := -static
else
  EXE           :=
  PLATFORM_LIBS :=
  STATIC        :=
endif

BIN := $(BLD_DIR)/$(TARGET)$(EXE)

# All .c files under src/, at any depth
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

CSTD     := -std=c23
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion
CFLAGS   := $(CSTD) $(WARNINGS) -I$(SRC_DIR) -pthread -MMD -MP
LDFLAGS  := -pthread
LDLIBS   := $(PLATFORM_LIBS)

DEBUG_FLAGS    := -g -O0
RELEASE_FLAGS  := -O2 -DNDEBUG
SANITIZE_FLAGS := -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined

.PHONY: all debug release sanitize run clean

all: debug

debug: CFLAGS += $(DEBUG_FLAGS)
debug: $(BIN)

# Release: rebuild everything from scratch with optimizations.
# -static (Windows) avoids needing libwinpthread-1.dll next to the .exe.
release:
	$(MAKE) clean
	$(MAKE) $(BIN) CFLAGS="$(CFLAGS) $(RELEASE_FLAGS)" LDFLAGS="$(LDFLAGS) $(STATIC)"

sanitize:
	$(MAKE) clean
	$(MAKE) $(BIN) CFLAGS="$(CFLAGS) $(SANITIZE_FLAGS)" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: debug
	./$(BIN) $(ARGS)

clean:
	rm -rf $(BLD_DIR)

-include $(DEPS)
