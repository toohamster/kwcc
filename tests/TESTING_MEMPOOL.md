# 测试记录

## Phase 1-3: mempool + config + TLV（纯 C）

| # | 测试点 | 测试类型 | 测试文件 | 状态 | 备注 |
|---|--------|---------|---------|------|------|
| 1 | mempool init + L0-L7 分配 | 纯 C | `tests/test_mempool.c` | ✅ | 46 tests |
| 2 | alloc/set/get | 纯 C | `tests/test_mempool.c` | ✅ | |
| 3 | const table 匹配 | 纯 C | `tests/test_mempool.c` | ✅ | |
| 4 | ref_count=1 初始值 | 纯 C | `tests/test_mempool.c` | ✅ | |
| 5 | GC ref_count 降到 0 回收 | 纯 C | `tests/test_mempool.c` | ✅ | |
| 6 | TLV build/iter/get_path | 纯 C | `tests/test_mempool.c` | ✅ | |
| 7 | TLV to_json + JSON 转义 | 纯 C | `tests/test_mempool.c` | ✅ | |
| 8 | L7 动态分配 | 纯 C | `tests/test_mempool.c` | ✅ | |
| 9 | config set/get string/int/bool | 纯 C | `tests/test_config.c` | ✅ | 19 tests |
| 10 | config TLV | 纯 C | `tests/test_config.c` | ✅ | |
| 11 | config release/release_prefix | 纯 C | `tests/test_config.c` | ✅ | |
| 12 | config set_max_pools | 纯 C | `tests/test_config.c` | ✅ | |
| 13 | config default value | 纯 C | `tests/test_config.c` | ✅ | |
| 14 | config 跨域隔离（a. vs c.） | 纯 C | `tests/test_config.c` | ✅ | |

## Phase 4: $config JS API

| # | 测试点 | 测试类型 | 测试文件 | 状态 | 备注 |
|---|--------|---------|---------|------|------|
| 1 | appSetInt → 常量表匹配 | C handler | `tests/test_config_js.c` | ✅ | slot type=INT32 |
| 2 | appSetString → 正常存储 | C handler | `tests/test_config_js.c` | ✅ | |
| 3 | appSetBool(true) → 常量表 | C handler | `tests/test_config_js.c` | ✅ | slot type=CONST |
| 4 | appSetBool(false) → 常量表 | C handler | `tests/test_config_js.c` | ✅ | 返回 "0" |
| 5 | appSetTlv flat → TLV 存储 | C handler | `tests/test_config_js.c` | ✅ | slot type=TLV |
| 6 | appSetTlv nested → TLV 存储 | C handler | `tests/test_config_js.c` | ✅ | |
| 7 | appGetTlv(path) flat | C handler | `tests/test_config_js.c` | ✅ | 返回 "8080" |
| 8 | appGetTlv(path) nested | C handler | `tests/test_config_js.c` | ✅ | `timeout/user` → "v1" |
| 9 | appGetTlv(no path) → JSON | C handler | `tests/test_config_js.c` | ✅ | JSON 字符串 |
| 10 | appGet(key) → 返回值 | C handler | `tests/test_config_js.c` | ✅ | |
| 11 | appGet(nonexist, default) | C handler | `tests/test_config_js.c` | ✅ | 返回 default |
| 12 | appRelease → 释放单个 key | C handler | `tests/test_config_js.c` | ✅ | ref_count-- |
| 13 | appReleasePrefix → 释放前缀 | C handler | `tests/test_config_js.c` | ✅ | |
| 14 | coreSetTlv → c. 前缀 | C handler | `tests/test_config_js.c` | ✅ | |
| 15 | C 模块直接读 Core TLV | C handler | `tests/test_config_js.c` | ✅ | `kwcc_mempool_get("c.xxx")` |
| 16 | setMaxPools → 运行时调整 | C handler | `tests/test_config_js.c` | ✅ | |
| 17 | JS wrapper → C handler 端到端 | JS 集成 | `tests/test_config_js.js` + `app/main.js` | ✅ | 11/11 通过 |
| 18 | TLV 转换（js_object_keys） | C handler | `tests/test_config_js.c` | ✅ | 修复 ClassID=0 问题 |
| 19 | 编译通过 | 构建 | `make clean && make` | ✅ | |
| 20 | 运行无错误 | 运行时 | `./kwcc` | ✅ | `kwcc.log` 无错误 |
| 21 | appSetJson → type=JSON → get 自动 parse 返回对象 | C handler + JS 集成 | `tests/test_config_js.c/.js` | ✅ | |
| 22 | appSetJsonString → type=STRING → get 返回字符串 → JS parse | C handler + JS 集成 | `tests/test_config_js.c/.js` | ✅ | |

## 图例

| 状态 | 含义 |
|------|------|
| ✅ | 测试通过 |
| ❌ | 测试失败 |
| ⏳ | 待测试 |
| 🔧 | 修复中 |

## 运行测试

```bash
# 纯 C 测试
gcc -I. -Ideps -D_GNU_SOURCE -o tests/bin/test_mempool tests/test_mempool.c build/obj/src/kwcc_mempool.o build/obj/deps/log/log.o && ./tests/bin/test_mempool
gcc -I. -Ideps -D_GNU_SOURCE -o tests/bin/test_config tests/test_config.c build/obj/src/kwcc_mempool.o build/obj/src/kwcc_config.o build/obj/deps/log/log.o && ./tests/bin/test_config

# C handler 测试
make clean && make
gcc -I. -Ideps -D_GNU_SOURCE -DCONFIG_KWCC -o tests/bin/test_config_js tests/test_config_js.c build/obj/src/kwcc_mempool.o build/obj/src/kwcc_config.o build/obj/src/kwcc_js.o build/obj/src/kwcc_bus.o build/obj/src/kwcc.o build/obj/deps/log/log.o build/obj/deps/mquickjs/mquickjs.o build/obj/deps/mquickjs/cutils.o build/obj/deps/mquickjs/dtoa.o build/obj/deps/mquickjs/libm.o && ./tests/bin/test_config_js

# 完整程序
./kwcc
```
