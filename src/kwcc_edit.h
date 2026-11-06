/* kwcc_edit.h — edit control host: 状态机 + 协议 + 焦点唯一权威
 *
 * 架构（#25，方案见 requirements/edit-control-implementation-plan.md）：
 *   展示层（kwcc_ui，无状态视口）→ 编辑契约（本模块）→ kwcc_edit_backend_ops_t（平台后端）
 * 编辑委托 OS 原生控件（macOS: AppKit），IME/光标/选区/剪贴板由后端自治。
 * 本模块不持久文本副本：值主权在 JS store；commit 后经注册表一次性交付。
 */
#ifndef KWCC_EDIT_H
#define KWCC_EDIT_H

/* 后端 ops（模式 B：隔离 + 可替换；实现于 kwcc_edit_appkit.m / 测试 mock） */
typedef struct kwcc_edit_backend_ops {
    void *(*create_field)(void *window, int multiline);       /* 返回字段实例 */
    void  (*destroy_field)(void *field);
    void  (*set_value)(void *field, const char *text);
    void  (*set_rect)(void *field, float x, float y, float w, float h);
    void  (*set_visible)(void *field, int visible);
    void  (*focus)(void *field);
    void  (*blur)(void *field);
    char *(*get_value)(void *field);          /* 返回 malloc 字符串，调用方 free */
} kwcc_edit_backend_ops_t;

/* 初始化（main.m：window = sapp_macos_get_window()） */
void kwcc_edit_init(const kwcc_edit_backend_ops_t *ops, void *window);
void kwcc_edit_shutdown(void);

/* 每帧调用（kwcc_begin_frame）：检查编辑中字段的缺失上报（缺报 → 自动 commit），重置帧注册 */
void kwcc_edit_begin_frame(void);

/* ui.textbox 展示路径每帧为每个字段调用：
 * clicked = 命中检测（mu_mouse_over && mouse_pressed）
 * visible = 字段可见（未被挡板拦截且与 clip 相交）
 * 编辑中字段每帧必须上报；缺报由 begin_frame 自动 commit（窗口关闭/模块隐藏场景） */
void kwcc_edit_report_field(const char *label, const char *value, const char *topic,
                            int multiline, float x, float y, float w, float h,
                            int clicked, int visible);

/* 后端 delegate 回调（AppKit 控制事件） */
void kwcc_edit_commit(void);   /* Enter / 点击他处 / 字段缺报 */
void kwcc_edit_cancel(void);   /* Esc */

/* 状态查询 */
int         kwcc_edit_is_editing(void);
const char *kwcc_edit_editing_label(void);

/* 一次性交付注册表（在下一次 commit/cancel 前有效；JS 在 bus handler 内同步取用） */
const char *kwcc_edit_committed_label(void);
const char *kwcc_edit_committed_value(void);
const char *kwcc_edit_cancelled_label(void);

#endif
