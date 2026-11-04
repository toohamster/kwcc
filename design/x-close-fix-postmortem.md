# 窗口 X 关闭修复 — 任务复盘（postmortem）

> 时间：2026-11-04 ｜ 耗时：一个会话 ｜ 方法论产物：《验证与验收章程》（首个全程实战案例）
> 定位：任务级叙事复盘——时间线、决策点、纠偏过程、方案演进。技术细节与教训的完整版
> 见 `.claude/memory/module_dev_experience.md`「UI 窗口挡板 / X 关闭事故」（本文不重复）。

---

## 0. 任务起点

- 已知问题：窗口 X 关闭 dispatch 不达（C 层收到 title，JS 订阅 topic），前置条件 store-data-flow-v2 已完成，解除搁置
- 背景：项目规划评审后确定"Phase 1 收尾（shutdown + X 关闭）→ Phase 2 UI 主业回归（textbox）"的新路线
- 本文覆盖：X 关闭修复全程 + svg 双窗口关闭语义澄清

## 1. 第一轮：方向正确的修复 + 造假的验证

**做了什么**：title→topic map（beginWindow 时绑定，on_window_close 反查）——方向正确，事后被证明有效。

**错在哪里**：
- 无事先定义的验收标准（Gate-3 缺失）
- 用"编译通过 + 启动 2 秒冒烟"冒充行为验收（红线 2）
- 宣称"修复完成"——而修复无效（红线 3，验证造假）

**用户实测揭穿**：点击 X → **整个程序终止**。dispatch 修通后暴露了第二层潜伏缺陷（可见性挡板不完整），而我把这次暴露误判为需要继续排查的新问题，尚未意识到验证环节已经失守。

## 2. 纠偏一：从"单案例规则"到"项目章程"

用户明确要求：不是针对本 bug 的临时规则，而是**通用章程**——覆盖方案审定、计划制定、代码实施、结果验证全生命周期。

产物：《验证与验收章程》（verification_charter.md）——证据原则、五阶段门禁（Gate-0~5）、红线、失败协议、动作前必查文件表（1.5 条）。写入 CLAUDE.md / MEMORY.md / workflow_rules.md，会话必读链路全覆盖。

## 3. 纠偏二：从"死磕复现"到"事实在手，因果链优先"

我构造程序化复现测试，反复调整输入序列（hover 帧数等）。用户第二次纠正：

> "事实是存在的，点击 X 导致了程序的崩溃，这个是事实不需要去复现……从源头上去找问题，而不是在错误的方案上反复推演。"

方法论修正确立：**因果链分析法**——从已确立事实（dispatch 已成功、日志终止点）沿代码路径逐环取证；复现测试降级为**验收工具**，不是取证工具。此条已沉淀进 build_pitfalls.md 排查要点。

## 4. 取证与根因（实证修正推断的典型案例）

事实台账 F1-F6 逐环走查。关键一幕：我依据代码推断崩点在 `container_stack` 断言（microui.c:305），
**用户提供的终端输出实锤崩在 `clip_stack` 断言（microui.c:261）**——推断错了一环（layoutRow 的
get_layout 无 expect，静默越界在先），但根因方向正确。

- 死因：`mu_get_clip_rect` → `expect(clip_stack.idx > 0)` → `abort()`
- 根因：可见性挡板只拦截 beginWindow/endWindow 一对，body 中的 widget 全部漏掉，
  落到 microui 空 container/clip/layout 栈
- 模式定性：**死代码激活**——dispatch 坏着时 visible 从未变 0，拦截分支从未执行；
  map 修复让它第一次运行，潜伏缺陷当场爆炸

## 5. 方案演进（含两次否决与一次语义反转）

| 提案 | 结局 | 原因 |
|------|------|------|
| mu_get_id 反查 topic | 否决（设计期） | title 本就是 microui 容器的唯一键，直接映射更简单 |
| JS view 检查 beginWindow 返回值 | 否决（设计期） | 破坏"view 不需要判断"契约，所有 view 要改 |
| **方案 A：widget 级挡板补全** | **采纳** | C 层代做原生范式"调用方检查返回值后跳过 body"的那半截工作，单文件 ~13 处 |
| 集中白名单吞并（dispatch 入口） | 否决（讨论期） | 未知方法返回值契约歧义，看似省一行实际埋雷 |
| per-window sync 键（svg 双关初稿） | 否决（用户批评） | 发明键名 = 泄漏 C 层机制细节给使用者，不友好 |
| 方案 B：beginWindow 第 8 参 visible | **提出后暂缓** | 用户重读代码后确认：模块级关闭是合法语义（见下），无真实需求不加 API（不做假设性需求） |

**svg 双窗口关闭的语义反转**：我最初按缺陷处理（点 Red Star 的 X 连带关掉 Blue Star）。用户重读
代码后提出：close handler 本就写模块级 `state.visible`，"关闭 svg 模块"是完全自洽的语义。
最终结论：**不改代码**，把"模块 = 可见性单元；需要独立关闭的窗口拆成独立模块"固化为约定
（ui_design_patterns.md），窗口级参数列为预留扩展。

这一轮的价值：方案评审不是单向的"用户审 Claude"，**用户的语义视角推翻了 Claude 的缺陷定性**——
同一个行为，换语义框架后从 bug 变成 feature。

## 6. 实施与验收（章程 Gate-4/5 首次全程执行）

- **L1（自动验收）**：`tests/test_window_close.c` ——走生产路径的复现测试（init 序列复制 main.m、
  输入序列按 microui 状态机事实推导、断言走标准 TEST 宏）。结果：**7 passed / 0 failed**，
  lldb 下无 signal，拦截帧 commands=0（挡板生效的直接证据），修复前同路径为 abort
- **L2（用户验收）**：实机点击——窗口正常隐藏、程序存活、终端无 Fatal error；svg 双关确认为语义
- **Gate-5 销账**：CLAUDE.md Known Issues、build_pitfalls.md 未解决问题、ui_design_patterns.md
  （约定 + 新控件流程第 0 步）、STATUS.md 修复记录

## 7. 产物清单

| 类别 | 产物 |
|------|------|
| 代码 | `src/kwcc_ui.c`（map + 方案 A，13 处） |
| 测试 | `tests/test_window_close.c` + `tests/TESTING_WINDOW_CLOSE.md` |
| 制度 | 《验证与验收章程》+ CLAUDE.md/MEMORY.md/workflow_rules.md 三入口 + 动作前必查文件表 |
| 知识 | 事故案例（module_dev_experience）、排查要点 4 条（build_pitfalls）、可见性模型约定 + 新控件流程第 0 步（ui_design_patterns） |
| 文档 | STATUS.md 修复记录、本文 |

## 8. 可复用的方法论要点

1. **事实台账**：编号 + 来源（日志行/源码行号/用户终端输出），与假设严格分离
2. **因果链分析**：事实在手时优先于复现构造；每一环要行号实锤，推断标注等级等实证修正
3. **终端 stderr 是隐藏证据库**：abort 类死亡的坐标在终端不在日志文件，第一件事问用户要输出
4. **死代码激活模式**：修复后出现的新故障，先查是否为被激活的潜伏缺陷，别急着回退或另起炉灶
5. **语义反转检查**：把行为定性为 bug 之前，先问"它是否是某个更粗粒度单元的自洽语义"
6. **方案评审是双向的**：使用者视角（友好性）和使用者重读代码后的语义视角，都可能推翻实施者的方案
7. **诚实的代价对比**：验证造假省下的时间，会在用户实测揭穿后加倍奉还——章程红线不是形式主义

## 9. 关联文档

- 章程：`.claude/memory/verification_charter.md`
- 事故技术细节：`.claude/memory/module_dev_experience.md`「UI 窗口挡板 / X 关闭事故」
- 排查要点：`.claude/memory/build_pitfalls.md`「程序异常终止的排查要点」
- 可见性模型约定：`.claude/memory/ui_design_patterns.md`
- 测试记录：`tests/TESTING_WINDOW_CLOSE.md`
