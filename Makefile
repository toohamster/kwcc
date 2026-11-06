# kwcc Makefile — macOS 10.14+ (clang)
# mquickjs: two-stage build — host tool generates mqjs_stdlib.h at build time
# All build artifacts go into build/ — deps/ stays clean

CC      = clang
HOST_CC = clang
CFLAGS  = -Wall -Wextra -fobjc-arc -I deps -O0 -D_GNU_SOURCE -fno-math-errno -fno-trapping-math
ifeq ($(DEBUG),1)
CFLAGS += -DKWCC_DEBUG   # 调试构建：诊断插桩（kwcc_diag）编译生效
endif
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

MQJS_SRCS = $(MQJS_CORE) $(DEP_SRCS) src/main.m src/kwcc.c src/kwcc_js.c src/kwcc_js_http.c src/kwcc_ui.c src/kwcc_io.c src/kwcc_bus.c src/kwcc_mempool.c src/kwcc_config.c src/kwcc_ui_bus.c src/kwcc_base.c src/kwcc_http.c src/kwcc_edit.c src/kwcc_edit_appkit.m

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

$(OBJ_DIR)/src/kwcc_js.o: src/kwcc_js.c src/kwcc_js.h src/kwcc_js_http.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_ui.o: src/kwcc_ui.c src/kwcc_ui.h src/kwcc_js.h src/kwcc_base.h src/kwcc_bus.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_bus.o: src/kwcc_bus.c src/kwcc_bus.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_io.o: src/kwcc_io.c src/kwcc_io.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_mempool.o: src/kwcc_mempool.c src/kwcc_mempool.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_config.o: src/kwcc_config.c src/kwcc_config.h src/kwcc_mempool.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_ui_bus.o: src/kwcc_ui_bus.c src/kwcc_ui_bus.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_base.o: src/kwcc_base.c src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_http.o: src/kwcc_http.c src/kwcc_http.h src/kwcc_base.h src/kwcc_bus.h src/kwcc_io.h src/kwcc_config.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_edit.o: src/kwcc_edit.c src/kwcc_edit.h src/kwcc_ui_bus.h | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_edit_appkit.o: src/kwcc_edit_appkit.m src/kwcc_edit_appkit.h src/kwcc_edit.h | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_js_http.o: src/kwcc_js_http.c src/kwcc_js_http.h src/kwcc_js.h src/kwcc_http.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
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

# ── Tests（链接命令唯一来源；TESTING_*.md 与此同步）──────────────
TEST_BIN = tests/bin
MQJS_TEST_OBJS = $(OBJ_DIR)/deps/mquickjs/mquickjs.o $(OBJ_DIR)/deps/mquickjs/cutils.o \
                 $(OBJ_DIR)/deps/mquickjs/dtoa.o $(OBJ_DIR)/deps/mquickjs/libm.o
UI_TEST_OBJS = $(OBJ_DIR)/src/kwcc.o $(OBJ_DIR)/src/kwcc_js.o $(OBJ_DIR)/src/kwcc_js_http.o \
               $(OBJ_DIR)/src/kwcc_ui.o $(OBJ_DIR)/src/kwcc_edit.o $(OBJ_DIR)/src/kwcc_io.o \
               $(OBJ_DIR)/src/kwcc_bus.o $(OBJ_DIR)/src/kwcc_mempool.o $(OBJ_DIR)/src/kwcc_config.o \
               $(OBJ_DIR)/src/kwcc_ui_bus.o $(OBJ_DIR)/src/kwcc_base.o $(OBJ_DIR)/src/kwcc_http.o
FULL_DEP_OBJS = $(OBJ_DIR)/deps/nanovg/nanovg.o $(OBJ_DIR)/deps/microui/microui.o \
                $(OBJ_DIR)/deps/log/log.o $(OBJ_DIR)/deps/picohttpparser/picohttpparser.o

$(TEST_BIN)/test_edit_host: tests/test_edit_host.c $(OBJ_DIR)/src/kwcc_edit.o \
    $(OBJ_DIR)/src/kwcc_ui_bus.o $(OBJ_DIR)/src/kwcc_base.o $(MQJS_TEST_OBJS) $(OBJ_DIR)/deps/log/log.o
	$(CC) $(CFLAGS) -I. -Ideps -o $@ $^

$(TEST_BIN)/test_edit_appkit: tests/test_edit_appkit.m $(OBJ_DIR)/src/kwcc_edit.o \
    $(OBJ_DIR)/src/kwcc_edit_appkit.o $(OBJ_DIR)/src/kwcc_ui_bus.o $(OBJ_DIR)/src/kwcc_base.o \
    $(MQJS_TEST_OBJS) $(OBJ_DIR)/deps/log/log.o
	$(CC) $(CFLAGS) -I. -Isrc -Ideps -o $@ $^ -framework Cocoa

$(TEST_BIN)/test_edit_js: tests/test_edit_js.c $(UI_TEST_OBJS) $(MQJS_TEST_OBJS) $(FULL_DEP_OBJS)
	$(CC) $(CFLAGS) -I. -Isrc -Ideps -DCONFIG_KWCC -o $@ $^ $(LDFLAGS)

$(TEST_BIN)/test_edit_display: tests/test_edit_display.c $(UI_TEST_OBJS) $(MQJS_TEST_OBJS) $(FULL_DEP_OBJS)
	$(CC) $(CFLAGS) -I. -Isrc -Ideps -DCONFIG_KWCC -o $@ $^ $(LDFLAGS)

$(TEST_BIN)/test_window_close: tests/test_window_close.c $(UI_TEST_OBJS) $(MQJS_TEST_OBJS) $(FULL_DEP_OBJS)
	$(CC) $(CFLAGS) -I. -Isrc -Ideps -DCONFIG_KWCC -o $@ $^ $(LDFLAGS)

$(TEST_BIN)/test_http_cancel: tests/test_http_cancel.c $(OBJ_DIR)/src/kwcc_mempool.o \
    $(OBJ_DIR)/src/kwcc_config.o $(OBJ_DIR)/src/kwcc_bus.o $(OBJ_DIR)/src/kwcc_io.o \
    $(OBJ_DIR)/src/kwcc_base.o $(OBJ_DIR)/src/kwcc_http.o $(OBJ_DIR)/deps/log/log.o \
    $(OBJ_DIR)/deps/picohttpparser/picohttpparser.o
	$(CC) $(CFLAGS) -I. -Ideps -o $@ $^

.PHONY: test-l1
test-l1: $(TEST_BIN)/test_edit_host $(TEST_BIN)/test_edit_appkit $(TEST_BIN)/test_edit_js \
         $(TEST_BIN)/test_edit_display $(TEST_BIN)/test_window_close $(TEST_BIN)/test_http_cancel
	@for t in test_edit_host test_edit_appkit test_edit_js test_edit_display test_window_close test_http_cancel; do ./$(TEST_BIN)/$$t 2>&1 | tail -1; done

$(TEST_BIN):
	mkdir -p $(TEST_BIN)

# ── Debug build（诊断插桩生效；切换需 clean 重建）───────────────
debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1

# ── Clean ───────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) $(BIN)

# ── Run ─────────────────────────────────────────────────────────
run: $(BIN)
	./$(BIN)

.PHONY: all clean run test-l1 debug
