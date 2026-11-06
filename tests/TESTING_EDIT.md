# TESTING_EDIT — 编辑控件（ui.textbox，#25 方案 S1-S5）

> 方案：`requirements/edit-control-implementation-plan.md`
> 测试文件：`tests/test_edit_host.c`（S1 mock）/ `tests/test_edit_appkit.m`（S2 真实控件冒烟）/ `tests/test_edit_js.c`（S3 JS 桥全链路）
> 示例：`app/modules/examples/form/`（单行 + 多行 + store 闭环）

## 编译

**链接命令唯一来源 = Makefile（`make test-l1` 重建全部并运行）。**
手拼命令被禁止（2026-11-06 批量链接拼装事故立法，见 charter 第 1.5 条）。

```bash
make build/obj/src/kwcc_edit.o build/obj/src/kwcc_edit_appkit.o   # 被测对象
make test-l1                                                      # 重建 6 个测试 + 全量运行
# 单个运行
./tests/bin/test_edit_host
./tests/bin/test_edit_appkit
./tests/bin/test_edit_js
./tests/bin/test_edit_display
```

## L1 结果（2026-11-05，S1-S5 实施后）

| 套件 | 结果 | 覆盖 |
|------|------|------|
| test_edit_host | **20/20** | L1.1 begin 序列 / L1.2 commit 注册表 / L1.3 cancel / L1.4 焦点唯一权威 / L1.5 可见性同步 / L1.6 幂等 / 缺报自动 commit / 多行独立实例 |
| test_edit_appkit | **11/11** | L1.7 真实控件 ops 冒烟（单行+多行）；含 Bio 事故回归断言 3 条：multi_handle_is_scrollview / multi_visible_on_container / multi_rect_on_container（句柄=容器 NSScrollView）；含翻转容器坐标回归 3 条：container_flipped / single_rect_on_flipped_container / multi_rect_on_container（flipped 时 y = 容器高 - y - h，编辑框上浮事故回归）；键入行为 → L2 |
| test_edit_js | **7/7** | L1.8 全链路：点击→begin_edit→commit→bus change→JS handler→ui.textboxValue 交付 |
| 回归 test_window_close | 7/7 | X 关闭修复无回归 |
| 回归 test_http_cancel | 11/11 | HTTP 方案一无回归 |
| 启动冒烟 | ✓ | form 模块加载、165 命令渲染；日志整读：唯一 ERROR = A15 负向用例（`kwcc_js_call_c('unknown')` 容错路径）的预期日志，相邻行 `PASS: A15` 自证；**该路径用 ERROR 级打日志是否降级待用户决策** |

## 实现边界说明（含 Bio 事故修复记录）

- **Bio 事故（2026-11-05）**：多行句柄误返回 NSTextView（挂载对象是 NSScrollView），set_rect/set_visible 作用错对象 → 容器永远 hidden → 无法输入。修复：句柄=容器 + `edit_text_view()` 解析 documentView；回归断言 3 条已入 appkit 冒烟。教训已立法（章程 Gate-4 句柄消费者清单 / Gate-5 ops 全函数语义断言）
- 键入/IME/选区/剪贴板走 AppKit responder chain（first responder），**不经 Sokol 事件管道**——L1 无法 headless 模拟（headless 进程 window 不进入 key 状态），归 L2 真实 GUI 验证
- 多行命令拦截走 `textView:doCommandBySelector:`（NSTextViewDelegate），单行走 `control:textView:doCommandBySelector:`——首版漏掉后者签名差异，复查修正
- AppKit 后端对 sokol 零依赖（window 句柄经宿主 init 注入，依赖倒置）

## L2 用户验收清单（Gate-5，make run + Form Demo 窗口）

1. 点击 Name 字段 → 就地编辑（无面板感），输入英文
2. **中文拼音 IME**：组合预览、候选窗跟随、空格上屏 [关键项]
3. 光标移动（←→/Home/End）、拖拽选区、Cmd+C/V/X、Cmd+Z undo
4. Enter → 提交（store: name=... 标签更新）；Esc → 还原
5. 编辑 Name 时点击 Bio → Name 提交、Bio 进入编辑（焦点切换）
6. Bio 多行：Enter 换行、Cmd+Enter 提交
7. 编辑态滚动窗口 → 字段滚出视野编辑框消失、滚回恢复
8. 窗口 resize → 编辑框跟随
9. 回归：test/calc/svg 窗口、X 关闭正常

## L2 结论（2026-11-06，用户验收）

通过项：位置（拖动后点击字段原位 ✓）、中文 IME ✓、多行输入 ✓、**空行保留 ✓**（edit-diag-value 实测提交值 `\n\n` 完整 + 行级绘制渲染空隙）、Name/Bio 焦点切换 ✓。

A15 决策：`dispatch unknown module` 容错日志改为 `kwcc_diag`（诊断级，仅 KWCC_DEBUG 构建可见）——release 构建零噪音。

## 已知未覆盖（自动化）

- IME 组合态细节（候选窗位置/方向键选词）——L2 人工
- 编辑态应用快捷键（Cmd+S）路由——responder chain 兜底方案未实现，L2 观察后决定
- 多行 NSTextView 的 `textView:doCommandBySelector:` 命令拦截无自动化断言（依赖真实 GUI 事件）
