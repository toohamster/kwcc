# 编辑控件实施方案 — ui.textbox（#25）

> 定位：应用级 UI 的生死线——文本输入（表单/设置面板的基础控件）
> 演进轨迹：mu_textbox 桥接（否决：无编辑模型，demo 级）→ 自研编辑核心（否决：复刻 OS 现成能力）→
> **原生委托（本方案）**：编辑委托给 OS 成熟控件，kwcc 只做展示与协议。
> 哲学依据：「IMUI 层零状态 + 编辑状态归独立服务 + 最小契约」（见 design/architecture-boundary-rationale.md
> 与 2026-11-05 讨论）；spec 哲学「委托成熟组件、不自写输入法逻辑」的贯彻。
> **本方案无 spike、无分期**——一个完整功能，单行/多行 = 同一协议的两个配置。

---

## 一、产品级定义（目标）

使用者（JS 开发者）写一行 `ui.textbox(label, value, topic)` 得到一个字段，编辑体验与原生
macOS 应用一致：中文拼音 IME 上屏、光标移动、选区、Cmd+C/V、Enter 提交、Esc 取消。

## 二、架构（三层，接口隔离 = 模式 B）

```
┌────────────────────────────────────────────────────────┐
│ 展示层（GL/microui，无状态视口）                          │
│  非编辑态：GL 绘制字段框 + 值文本（nvgText，CJK 已就位）    │
│  命中：mu_update_control(id=label) + mu_mouse_over       │
│        （公开 API，microui.h:270-271）                   │
│  点击 → 宿主 begin_edit(field_key, rect, value)          │
├──────────── 编辑契约（宿主状态机 + 协议，引擎无关）────────┤
│  kwcc_edit 宿主（进程内全局输入服务，独立于帧循环）         │
│  状态机：closed → editing(field) → committed/cancelled   │
│  焦点唯一权威：同时只允许一个字段处于编辑态                 │
├──────────── kwcc_edit_backend_ops_t（ops 隔离边界）──────┤
│  create_field / set_value / set_rect / set_visible       │
│  focus / blur / destroy — 函数指针表（模式 B 先例：ops）    │
├────────────────────────────────────────────────────────┤
│ macOS AppKit 后端（当前唯一实现）                          │
│  单行 NSTextField / 多行 NSTextView（实例复用，非池）      │
│  远期：WKWebView 富文本后端、各平台原生控件后端              │
│  （自研核心 = 最后手段，无触发条件不做）                    │
└────────────────────────────────────────────────────────┘
```

**IME 问题的结构性消解**（无 spike 的依据）：编辑控件成为 first responder 后，
按键/IME 经 AppKit responder chain 直送字段编辑器（NSText）——**不经过 Sokol 事件管道**。
Sokol 事件链在编辑态静默（这正是期望行为）。IME 是 AppKit 本职，产品级白送。

## 三、macOS 后端实现要点（取证事实 + 实现时验证项）

| 事项 | 方案 | 依据/状态 |
|------|------|----------|
| 窗口获取 | `sapp_macos_get_window()` 官方 API | sokol_app.h:2231 [事实] |
| 控件挂载 | NSWindow contentView `addSubview:`（同窗口 subview，非独立窗口） | 同窗口不触发 spec 禁令（禁的是独立 HWND/子窗口） |
| 坐标映射 | kwcc rect → NSView frame。GL 与 AppKit contentView 均为 bottom-left origin（high_dpi=false，dpi=1.0）→ 预期直映射 | [推断-基于 sokol_analysis] 实现时 L2 验证位置 |
| frame 同步 | begin_edit 时 set frame；窗口 resize 时同步（挂接点：main.m 或编辑前重取） | 工作项 |
| 实例策略 | 单例复用：一个 NSTextField + 一个 NSTextView（microui number_edit 同款先例），按 multiline 选择 | 简单优先 |
| 提交/取消 | Enter → commit（delegate controlTextDidEndEditing）；Esc → cancel（还原原值）；点击其他区域 → 原生默认提交 | 贴近 macOS 原生习惯 |
| 编辑态快捷键 | AppKit 消费按键 → Sokol 收不到 Cmd+S 等。经 responder chain `performKeyEquivalent` 或 validateMenuItem 路由兜底 | 已知风险项，进 L2 验证 |
| 可见性同步 | 编辑态中字段被滚动出视野/被更高 zindex 窗口遮挡 → `set_visible(NO)`（宿主在 begin_frame 后置检查，复用 microui 容器状态） | 已知工作项 |

## 四、协议规范

### 宿主 ↔ 后端（ops 表）

```c
typedef struct kwcc_edit_backend_ops {
    void *(*create_field)(void *window, int multiline);
    void  (*destroy_field)(void *field);
    void  (*set_value)(void *field, const char *text);
    void  (*set_rect)(void *field, float x, float y, float w, float h);
    void  (*set_visible)(void *field, int visible);
    void  (*focus)(void *field);
    void  (*blur)(void *field);       /* 提交/取消后收焦 */
    const char *(*get_value)(void *field, char *buf, int bufsz);
    void  (*set_delegate)(void *field, void *host);  /* 回调挂接 */
} kwcc_edit_backend_ops_t;
```

### 宿主 ↔ JS

| API | 语义 |
|-----|------|
| `ui.textbox(label, value, topic, multiline)` | 非编辑态：绘制字段+值；点击 → begin_edit；返回值：无（值主权在 store） |
| `ui.textboxValue(label)` | 在 change 事件 handler 内取回刚提交的值（宿主注册表，分发期间有效） |
| `$bus` topic `topic` action `"change"` | commit 后宿主经 kwcc_ui_bus_dispatch_event 通知（复用现有 UI→JS 通道） |
| `$bus` topic `topic` action `"cancel"` | Esc 取消通知 |

**数据主权**：value 由 JS store 持有；编辑态的实时文本归黑盒（NSTextField 现场值）；
commit 后经注册表交付 JS → store → 下帧 GL 重绘。宿主/IMUI 层不持久文本副本（每帧注册表随
kwcc_begin_frame 同步重置语义）。

## 五、改动清单 ↔ 验收映射（Gate-2）

| # | 改动 | 文件 | 验收项 |
|---|------|------|--------|
| C1 | kwcc_edit 宿主（状态机/注册表/协议/焦点仲裁） | `src/kwcc_edit.h/c`（新） | L1.1-L1.6（mock backend 单测） |
| C2 | AppKit 后端 | `src/kwcc_edit_appkit.m`（新） | L1.7 + L2 全部 |
| C3 | ui.textbox 展示/命中/begin_edit + ui.textboxValue | `src/kwcc_ui.c`（js_ui_dispatch + methods_js） | L1.8 + L2 |
| C4 | Makefile：新增 kwcc_edit.o / kwcc_edit_appkit.o 规则 | `Makefile` | 编译通过 |
| C5 | umbrella include | `src/kwcc.h` | 编译通过 |
| C6 | 示例模块（表单 demo，含单行+多行） | `app/modules/examples/form/`（新） | L2 场景载体 |

## 六、验收标准（Gate-3 先行定义）

### L1 — 自动验收（我执行，贴实际输出）

| # | 断言 | 验证方式 |
|---|------|---------|
| L1.1 | begin_edit → 状态机进入 editing，mock backend 收到 create/set_value/set_rect/focus 序列 | `tests/test_edit_host.c`（**FAKE backend**——同时证明接口隔离成立） |
| L1.2 | commit → get_value → 注册表更新 → change 事件 dispatch 记录 | 同上 |
| L1.3 | cancel → 原值保留，dispatch cancel 事件，注册表不更新 | 同上 |
| L1.4 | 焦点唯一权威：editing 中第二字段请求 → 排队/拒绝策略（取：拒绝 + log_warn） | 同上 |
| L1.5 | 编辑态字段遮挡/滚出 → set_visible(NO) 被调用；恢复 → set_visible(YES) | 同上（mock 遮挡输入） |
| L1.6 | 幂等：重复 begin_edit 同一字段无害；closed 态 commit 无害 | 同上 |
| L1.7 | AppKit 后端编译 + create/set_value/focus 对真实控件调用无异常（headless 冒烟） | `tests/bin/test_edit_host` 内嵌 |
| L1.8 | ui.textboxValue 经 js_ui_dispatch 返回注册表值 | `tests/test_edit_js.c`（链接 kwcc_ui，同 test_window_close 模式） |

### L2 — 用户验收（你执行，GUI 行为只有人能验）

1. 点击单行字段 → 就地进入编辑（无面板感），输入英文
2. **中文拼音 IME**：组合预览、候选窗跟随、空格上屏 [关键项]
3. 光标移动（←→/Home/End）、选区拖拽、Cmd+C/V/X、undo（Cmd+Z）
4. Enter 提交 → 字段值更新（GL 重绘）；Esc → 还原
5. 编辑 A 时点击字段 B → A 提交、B 进入编辑（焦点切换）
6. 编辑态滚动：字段滚出视野 → 编辑视图消失；滚回 → 处理策略（提交或恢复）确认
7. 窗口 resize → 编辑框跟随
8. 多行字段（textarea 形态）：换行、滚动
9. 既有模块回归（test/calc/svg 窗口、X 关闭）不受影响

## 七、范围边界

- **本方案只做 macOS AppKit 后端**；WKWebView 富文本后端 = ops 接口预留，不实施
- 自研编辑核心：无触发条件（四条件 checklist 均不命中），不做
- IME spike：取消（管道已切换为 AppKit responder chain，疑虑不存在——2026-11-05 结论）

## 八、回退方式

新增文件为主（kwcc_edit.c/h/appkit.m、form 示例），kwcc_ui.c 改动收敛于 js_ui_dispatch 两个
分支 + methods_js 两行——单 revert commit 可整体回退，无共享状态污染。

## 九、执行状态

| Step | 内容 | 状态 |
|------|------|------|
| S1 | kwcc_edit 宿主 + mock backend L1（L1.1-L1.6） | ✅ 20/20 |
| S2 | AppKit 后端（L1.7） | ✅ 11/11（含 Bio 句柄回归 3 条 + 翻转坐标回归 3 条） |
| S3 | kwcc_ui 桥接 + JS wrapper（L1.8） | ✅ 7/7 |
| S4 | Makefile/kwcc.h + form 示例 | ✅（test-l1 = 链接命令唯一来源） |
| S5 | L1 全量跑通贴输出 → L2 用户验收 | ✅ L1 全量 60/60 + L2 通过（位置/中文 IME/多行输入/空行保留） |

## 十、实施期事故与修复记录

- **Bio 无法输入（句柄契约）**：多行句柄误返回 NSTextView（挂载对象是 NSScrollView）→ 容器永远 hidden。修复 + 立法（章程 Gate-4 句柄消费者清单 / Gate-5 ops 全函数语义断言）
- **编辑框镜像错位（坐标系）**：edit-diag 实测（容器 flipped=0 + 直映射产生 name 下移/bio 上移镜像签名）证明 mu 坐标系 y-down → 换算条件反转（非 flipped 容器换算）
- **展示空行"被移除"**：首版修复基于不完整取证（只读分行检测端未读绘制消费端）；补齐取证（nvgText 单行 + fontstash 迭代器无换行处理）后改为 nvgTextBreakLines 应用侧分行 + wheel 滚动；edit-diag-value 实测提交值 `\n\n` 完整保留
- **连接命令拼装事故**：批量循环用通用对象集链接三个专属测试 → 立法（链接命令唯一来源 = Makefile test 目标）
