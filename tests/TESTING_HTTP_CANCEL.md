# TESTING_HTTP_CANCEL — HTTP cancel 逻辑放弃 / 自治 reap / shutdown（方案一）

> 测试类型：纯 C 层测试（无 mquickjs / UI 依赖）
> 测试文件：`tests/test_http_cancel.c`
> 运行：`./tests/bin/test_http_cancel`（约 4 秒，含 fake 脚本 2 秒 sleep + 1 秒等待余量）
> 编译：
> ```bash
> clang -Wall -Wextra -I. -Ideps -O0 -D_GNU_SOURCE \
>   -o tests/bin/test_http_cancel tests/test_http_cancel.c \
>   build/obj/src/kwcc_mempool.o build/obj/src/kwcc_config.o build/obj/src/kwcc_bus.o \
>   build/obj/src/kwcc_io.o build/obj/src/kwcc_base.o build/obj/src/kwcc_http.o \
>   build/obj/deps/log/log.o build/obj/deps/picohttpparser/picohttpparser.o
> ```

## 背景（方案一降级，见 implementation plan 演进记录）

原 v2 计划的 lifecycle_shutdown 服务暂缓（单客户世界，立项校验规则应用于服务自身），
HTTP 侧改动全部实施：cancel 逻辑放弃（只关 pipe）、check_progress 自治 reap、
unregister(-1) 修复、on_read 防御、kwcc_http_shutdown（幂等 + 拒新请求）、
main.m cleanup/信号路径直接调用（日志关闭移末尾）。

## Hermetic 手段

fake curl 脚本（`/tmp/kwcc_test_fake_curl.sh`：sleep 2 后输出合法 HTTP 响应），
经 core TLV config 注入 bin_path——子进程确定性存活 2 秒（cancel 窗口）后自然退出（reap 窗口），零网络依赖。

slot 占用状态通过 `kwcc_http_get_result()` 观测：in_use 期间返回 0，cleanup 后返回 -1
（memset 清 in_use → req_find miss）。旧代码 cancel 立即 cleanup → get_result==-1；
新代码 cancel 后保持 0 直至子进程退出被 reap——此差异即新旧行为判据。

## 测试点

| # | 测试点 | 对应 H 系列 | 状态 |
|---|--------|------------|------|
| T0 | config TLV 注入 fake bin_path 生效 | — | ✅ |
| T1 | request 经 fake curl 启动 | — | ✅ |
| T2 | cancel 后 slot 保持占用（延迟回收，pid 保留） | H1 | ✅ |
| T3 | 连续 cancel 安全（pipe_read_fd<0 防御） | H6 | ✅ |
| T4 | cancel 窗口内无 end/error dispatch（仅 cancel 事件） | H4 | ✅ |
| T5-T7 | 子进程自然退出 → check_progress 静默回收（slot 释放） | H2 | ✅ |
| T8 | reap 不产生任何 bus 事件 | H4 | ✅ |
| T9 | 释放的 slot 可复用 | — | ✅ |
| T10 | shutdown 全量回收（活跃子进程 SIGTERM + waitpid 阻塞） | H9 | ✅ |
| T11 | shutdown 后 request 返回 NULL（g_shutdown 拒新） | H7 | ✅ |

**L1 结果（2026-11-04）：11 passed, 0 failed**

## 实机冒烟（kill 信号路径，E2）

5 个活跃请求在飞（httpbin.org）时 `kill -TERM`：
- `received signal 15, forcing http shutdown` → `http: shutdown complete`（日志关闭移末尾，shutdown 日志落盘）
- 进程退出，`ps` 无残留 curl ✓

## 已知未覆盖

- H2 变体「cancelled 子进程在 transfer 阶段 SIGPIPE 自然退出」：fake 脚本场景子进程在 sleep（不写 pipe），
  真实 transfer 阶段行为由实机冒烟的 live 请求覆盖过（B4 cancel + delay/5 完成）
- H8（is_exiting 运行期防御）：随服务壳取消，作废
- 服务测试 S1-S6：服务暂缓，作废（泛化蓝图保留在 spec v2）
