# kwcc Makefile — macOS 10.14+ (clang)
# mquickjs: two-stage build — host tool generates mqjs_stdlib.h at build time
# All build artifacts go into build/ — deps/ stays clean

CC      = clang
HOST_CC = clang
CFLAGS  = -Wall -Wextra -fobjc-arc -I deps -O0 -D_GNU_SOURCE -fno-math-errno -fno-trapping-math
HOST_CFLAGS = -Wall -Wextra -I deps/mquickjs -I src -O2 -D_GNU_SOURCE -fno-math-errno -fno-trapping-math -DCONFIG_KWCC
LDFLAGS = -framework Cocoa -framework OpenGL -framework IOKit -framework QuartzCore

# Source files
MQJS_CORE = deps/mquickjs/mquickjs.c \
            deps/mquickjs/cutils.c \
            deps/mquickjs/dtoa.c \
            deps/mquickjs/libm.c

DEP_SRCS = deps/nanovg/nanovg.c \
           deps/microui/microui.c \
           deps/log/log.c \
           deps/picohttpparser/picohttpparser.c

MQJS_SRCS = $(MQJS_CORE) $(DEP_SRCS) src/main.m src/kwcc.c src/kwcc_js.c src/kwcc_ui.c src/kwcc_io.c src/kwcc_bus.c

# Build directories
BUILD_DIR = build
HOST_DIR  = $(BUILD_DIR)/host
OBJ_DIR   = $(BUILD_DIR)/obj

# Object files — preserve source directory structure under build/obj/
MQJS_OBJS = $(patsubst deps/%.c,$(OBJ_DIR)/deps/%.o,$(MQJS_SRCS))
MQJS_OBJS := $(patsubst src/%.m,$(OBJ_DIR)/src/%.o,$(MQJS_OBJS))
MQJS_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(MQJS_OBJS))

# Host build tool (stage 1) — flatten names to avoid subdirectory issues
HOST_SRCS = deps/mquickjs/mquickjs_build.c deps/mquickjs/mqjs_stdlib.c
HOST_OBJS = $(HOST_DIR)/mquickjs_build.host.o $(HOST_DIR)/mqjs_stdlib.host.o
HOST_TOOL = $(HOST_DIR)/_build_stdlib

MQJS_HEADERS = deps/mquickjs/mqjs_stdlib.h deps/mquickjs/mquickjs_atom.h

BIN  = kwcc

# ── Default target ──────────────────────────────────────────────
all: $(BIN)

# ── Stage 1: build host tool to generate stdlib headers ─────────
$(HOST_TOOL): $(HOST_OBJS) | $(HOST_DIR)
	$(HOST_CC) -o $@ $^

$(MQJS_HEADERS): $(HOST_TOOL)
	cd deps/mquickjs && ../../$(HOST_TOOL) > mqjs_stdlib.h
	cd deps/mquickjs && ../../$(HOST_TOOL) -a > mquickjs_atom.h

$(HOST_OBJS): $(HOST_DIR)/%.host.o: deps/mquickjs/%.c | $(HOST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# ── Stage 2: build main binary ──────────────────────────────────
$(BIN): $(MQJS_OBJS) $(MQJS_HEADERS)
	$(CC) $(MQJS_OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/deps/mquickjs/%.o: deps/mquickjs/%.c $(MQJS_HEADERS) | $(OBJ_DIR)/deps/mquickjs
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/deps/nanovg/%.o: deps/nanovg/%.c | $(OBJ_DIR)/deps/nanovg
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/deps/microui/%.o: deps/microui/%.c | $(OBJ_DIR)/deps/microui
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/deps/log/%.o: deps/log/%.c | $(OBJ_DIR)/deps/log
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/deps/picohttpparser/%.o: deps/picohttpparser/%.c | $(OBJ_DIR)/deps/picohttpparser
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/main.o: src/main.m $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc.o: src/kwcc.c $(MQJS_HEADERS) src/kwcc_base.h | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_js.o: src/kwcc_js.c src/kwcc_js.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_ui.o: src/kwcc_ui.c src/kwcc_ui.h src/kwcc_js.h src/kwcc_base.h src/kwcc_bus.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_bus.o: src/kwcc_bus.c src/kwcc_bus.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_io.o: src/kwcc_io.c src/kwcc_io.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

# ── Create build directories ────────────────────────────────────
$(HOST_DIR) \
$(OBJ_DIR)/deps/mquickjs \
$(OBJ_DIR)/deps/nanovg \
$(OBJ_DIR)/deps/microui \
$(OBJ_DIR)/deps/log \
$(OBJ_DIR)/deps/picohttpparser \
$(OBJ_DIR)/src:
	mkdir -p $@

# ── Clean ───────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) $(BIN)

# ── Run ─────────────────────────────────────────────────────────
run: $(BIN)
	./$(BIN)

.PHONY: all clean run
