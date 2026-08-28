# mini-git Makefile (Windows / MinGW + Linux / GNU make)
#
# Windows:
#   mingw32-make
#   mingw32-make test
#
# Linux:
#   make
#   make test
#
# Override compiler/flags in the usual Make way, e.g.:
#   make CC=clang

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g

BUILD_DIR = build

ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
BIN = $(BUILD_DIR)/mgit.exe
HTTP_SRC = src/base/http.c
LDFLAGS = -lz -lwinhttp
PYTHON = python
else
SHELL := /bin/sh
BIN = $(BUILD_DIR)/mgit
HTTP_SRC = src/base/http_curl.c
LDFLAGS = -lz -lcurl
PYTHON = python3
endif

# Git semantics are shared across platforms. Only the HTTP backend and
# build/test shell differ.
SRCS = \
    src/main.c \
    src/base/hash.c \
    src/base/zlib_util.c \
    src/base/file.c \
    $(HTTP_SRC) \
    src/core/object.c \
    src/core/ref.c \
    src/core/tree.c \
    src/core/commit.c \
    src/core/graph.c \
    src/core/index.c \
    src/core/linemerge.c \
    src/core/ignore.c \
    src/core/remote.c \
    src/core/pack.c \
    src/core/pack_index.c \
    src/core/transport.c \
    src/commands/cmd_init.c \
    src/commands/cmd_hash_object.c \
    src/commands/cmd_cat_file.c \
    src/commands/cmd_write_tree.c \
    src/commands/cmd_commit_tree.c \
    src/commands/cmd_ls_tree.c \
    src/commands/cmd_log.c \
    src/commands/cmd_add.c \
    src/commands/cmd_commit.c \
    src/commands/cmd_status.c \
    src/commands/cmd_branch.c \
    src/commands/cmd_checkout.c \
    src/commands/cmd_reset.c \
    src/commands/cmd_tag.c \
    src/commands/cmd_diff.c \
    src/commands/cmd_merge.c \
    src/commands/cmd_stash.c \
    src/commands/cmd_reflog.c \
    src/commands/cmd_revert.c \
    src/commands/cmd_remote.c \
    src/commands/cmd_push.c \
    src/commands/cmd_fetch.c \
    src/commands/cmd_pull.c \
    src/commands/cmd_clone.c \
    src/commands/cmd_cherry_pick.c \
    src/commands/cmd_rebase.c \
    src/commands/cmd_gc.c \
    src/commands/cmd_count_objects.c

OBJS = $(SRCS:src/%.c=$(BUILD_DIR)/%.o)

all: $(BIN)

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	@if not exist "$(BUILD_DIR)\base" mkdir "$(BUILD_DIR)\base"
	@if not exist "$(BUILD_DIR)\core" mkdir "$(BUILD_DIR)\core"
	@if not exist "$(BUILD_DIR)\commands" mkdir "$(BUILD_DIR)\commands"
else
	@mkdir -p "$(BUILD_DIR)/base" "$(BUILD_DIR)/core" "$(BUILD_DIR)/commands"
endif

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $(BIN) $(LDFLAGS)
	@echo "Built: $(BIN)"

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BIN)
	$(PYTHON) tests/run_all.py

# Temporary safety net while the old Windows suites are migrated to Python.
test-legacy: $(BIN)
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1
else
	@echo "legacy PowerShell suite is Windows-only"
endif

clean:
ifeq ($(OS),Windows_NT)
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
else
	@rm -rf "$(BUILD_DIR)"
endif

install: $(BIN)
	@echo "Binary available at: $(BIN)"

.PHONY: all clean test test-legacy install
