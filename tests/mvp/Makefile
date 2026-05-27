# mvp Makefile — minimal C function registration test
# Only tests: CONFIG_KWCC stdlib generation + custom C function callable from JS
# No Sokol, no microui, no nanovg

CC       = clang
HOST_CC  = clang
CFLAGS   = -Wall -Wextra -I deps -I src -O0 -D_GNU_SOURCE
HOST_CFLAGS = -Wall -Wextra -I deps/mquickjs -O2 -D_GNU_SOURCE -DCONFIG_KWCC

# mquickjs core (4 files — same as main project)
MQJS_CORE = deps/mquickjs/mquickjs.c \
            deps/mquickjs/cutils.c \
            deps/mquickjs/dtoa.c \
            deps/mquickjs/libm.c

# stub functions referenced by stdlib (js_kwcc_ui, js_gc, js_load, etc.)
STUB_SRCS = src/mquickjs_stubs.c

BUILD_DIR = build
HOST_DIR  = $(BUILD_DIR)/host
OBJ_DIR   = $(BUILD_DIR)/obj

MQJS_OBJS = $(patsubst deps/mquickjs/%.c,$(OBJ_DIR)/%.o,$(MQJS_CORE))
MQJS_OBJS += $(OBJ_DIR)/mvp.o
MQJS_OBJS += $(OBJ_DIR)/mquickjs_stubs.o
MQJS_OBJS += $(OBJ_DIR)/log.o

HOST_SRCS = deps/mquickjs/mquickjs_build.c deps/mquickjs/mqjs_stdlib.c
HOST_OBJS = $(HOST_DIR)/mquickjs_build.host.o $(HOST_DIR)/mqjs_stdlib.host.o
HOST_TOOL = $(HOST_DIR)/_build_stdlib

MQJS_HEADERS = deps/mquickjs/mqjs_stdlib.h deps/mquickjs/mquickjs_atom.h

BIN = mvp_test

.PHONY: all clean run

all: $(BIN)

# ── Stage 1: generate stdlib headers ───────────────────────────
$(HOST_TOOL): $(HOST_OBJS) | $(HOST_DIR)
	$(HOST_CC) -o $@ $^

$(MQJS_HEADERS): $(HOST_TOOL)
	cd deps/mquickjs && ../../$(HOST_TOOL) > mqjs_stdlib.h
	cd deps/mquickjs && ../../$(HOST_TOOL) -a > mquickjs_atom.h

$(HOST_OBJS): $(HOST_DIR)/%.host.o: deps/mquickjs/%.c | $(HOST_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# ── Stage 2: build MVP binary ─────────────────────────────────
$(BIN): $(MQJS_OBJS) $(MQJS_HEADERS)
	$(CC) $(MQJS_OBJS) -o $@

$(OBJ_DIR)/%.o: deps/mquickjs/%.c $(MQJS_HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mvp.o: tests/mvp/mvp.c $(MQJS_HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/mquickjs_stubs.o: src/mquickjs_stubs.c $(MQJS_HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/log.o: deps/log/log.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(HOST_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN)

run: $(BIN)
	./$(BIN) tests/mvp/test.js
