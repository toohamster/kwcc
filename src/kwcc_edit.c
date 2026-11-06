/* kwcc_edit.c — edit control host 实现
 *
 * 状态机：closed → editing(label) → closed（commit/cancel）
 * 焦点唯一权威：editing 中拒绝其他字段（log_warn）
 * 值交付：commit 时 get_value → 注册表（strdup，下次 commit/cancel 前有效）
 *         → kwcc_ui_bus_dispatch_event 通知 JS（bus handler 内同步取用）
 * 缺报自动 commit：编辑中字段若一帧未上报（窗口关闭/模块隐藏）→ 视为失焦提交
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kwcc_edit.h"
#include "kwcc_ui_bus.h"
#include "llog.h"

/* ── 状态 ──────────────────────────────────────────────────── */

#define KWCC_EDIT_STATE_CLOSED  0
#define KWCC_EDIT_STATE_EDITING 1

static const kwcc_edit_backend_ops_t *g_ops = NULL;
static void *g_window = NULL;

/* 后端字段实例（单例复用：单行/多行各一，microui number_edit 先例） */
static void *g_field_single = NULL;
static void *g_field_multi = NULL;

static int   g_state = KWCC_EDIT_STATE_CLOSED;
static char  g_edit_label[64];
static char  g_edit_topic[128];
static int   g_edit_multiline = 0;
static float g_edit_rect_x = 0, g_edit_rect_y = 0, g_edit_rect_w = 0, g_edit_rect_h = 0;
static int   g_reported = 0;        /* 本帧是否上报（begin_frame 检查） */
static int   g_visible_last = 1;    /* 上一帧可见性（去重 set_visible 调用） */

/* 一次性交付注册表（strdup，替换时释放——无固定长度上限，多行长文本安全） */
static char *g_committed_label = NULL;
static char *g_committed_value = NULL;
static char *g_cancelled_label = NULL;

static void set_committed(const char *label, const char *value) {
    free(g_committed_label);
    free(g_committed_value);
    g_committed_label = label ? strdup(label) : NULL;
    g_committed_value = value ? strdup(value) : NULL;
}

static void set_cancelled(const char *label) {
    free(g_cancelled_label);
    g_cancelled_label = label ? strdup(label) : NULL;
}

/* 取后端字段实例（按 multiline 惰性创建，复用） */
static void *backend_field(int multiline) {
    void **slot = multiline ? &g_field_multi : &g_field_single;
    if (!*slot) {
        *slot = g_ops->create_field(g_window, multiline);
        if (!*slot) {
            log_error("edit: backend create_field(%d) failed", multiline);
            return NULL;
        }
    }
    return *slot;
}

/* ── 公开 API ──────────────────────────────────────────────── */

void kwcc_edit_init(const kwcc_edit_backend_ops_t *ops, void *window) {
    g_ops = ops;
    g_window = window;
    g_state = KWCC_EDIT_STATE_CLOSED;
}

void kwcc_edit_shutdown(void) {
    if (g_field_single) { g_ops->destroy_field(g_field_single); g_field_single = NULL; }
    if (g_field_multi)  { g_ops->destroy_field(g_field_multi);  g_field_multi = NULL; }
    g_ops = NULL;
    g_window = NULL;
    g_state = KWCC_EDIT_STATE_CLOSED;
    free(g_committed_label); g_committed_label = NULL;
    free(g_committed_value); g_committed_value = NULL;
    free(g_cancelled_label); g_cancelled_label = NULL;
}

void kwcc_edit_begin_frame(void) {
    /* 编辑中字段缺报（窗口关闭/模块隐藏）→ 自动 commit，避免输入丢失 */
    if (g_state == KWCC_EDIT_STATE_EDITING && !g_reported) {
        log_info("edit: field '%s' not reported this frame, auto-commit", g_edit_label);
        kwcc_edit_commit();
    }
    g_reported = 0;
}

void kwcc_edit_report_field(const char *label, const char *value, const char *topic,
                            int multiline, float x, float y, float w, float h,
                            int clicked, int visible) {
    if (!label || !g_ops) return;

    int is_editing_field = (g_state == KWCC_EDIT_STATE_EDITING &&
                            strcmp(g_edit_label, label) == 0);

    if (g_state == KWCC_EDIT_STATE_EDITING && !is_editing_field) {
        /* 焦点唯一权威：editing 中其他字段的上报只注册不响应 */
        if (clicked) {
            log_warn("edit: reject focus request for '%s', '%s' is editing",
                     label, g_edit_label);
        }
        return;
    }

    if (is_editing_field) {
        g_reported = 1;
        /* rect 同步：字段位置/尺寸变化（窗口 resize 等）→ 原生控件跟随 */
        if (x != g_edit_rect_x || y != g_edit_rect_y ||
            w != g_edit_rect_w || h != g_edit_rect_h) {
            void *field = backend_field(g_edit_multiline);
            if (field) {
                g_ops->set_rect(field, x, y, w, h);
                g_edit_rect_x = x; g_edit_rect_y = y;
                g_edit_rect_w = w; g_edit_rect_h = h;
            }
        }
        /* 可见性同步：宿主在编辑态隐藏/恢复原生控件（滚动出视野/遮挡场景） */
        if (visible != g_visible_last) {
            void *field = backend_field(g_edit_multiline);
            if (field) g_ops->set_visible(field, visible);
            g_visible_last = visible;
        }
        if (clicked) {
            /* 编辑中点击自身：无害（幂等） */
        }
        return;
    }

    /* closed 态：点击 → 授予焦点开始编辑 */
    if (clicked) {
        void *field = backend_field(multiline);
        if (!field) return;
        g_ops->set_value(field, value ? value : "");
        g_ops->set_rect(field, x, y, w, h);
        g_ops->set_visible(field, 1);
        g_ops->focus(field);

        snprintf(g_edit_label, sizeof(g_edit_label), "%s", label);
        snprintf(g_edit_topic, sizeof(g_edit_topic), "%s", topic ? topic : "");
        g_edit_multiline = multiline;
        g_edit_rect_x = x; g_edit_rect_y = y;
        g_edit_rect_w = w; g_edit_rect_h = h;
        g_state = KWCC_EDIT_STATE_EDITING;
        g_reported = 1;
        g_visible_last = 1;
        log_info("edit: begin editing '%s' (multiline=%d)", label, multiline);
    }
}

void kwcc_edit_commit(void) {
    if (g_state != KWCC_EDIT_STATE_EDITING || !g_ops) return;   /* 幂等 */
    void *field = backend_field(g_edit_multiline);
    if (!field) { g_state = KWCC_EDIT_STATE_CLOSED; return; }

    char *value = g_ops->get_value(field);
    set_committed(g_edit_label, value);
    free(value);

    g_ops->blur(field);
    g_ops->set_visible(field, 0);
    g_state = KWCC_EDIT_STATE_CLOSED;
    g_reported = 0;
    log_info("edit: committed '%s'", g_edit_label);

    /* 通知 JS（bus handler 内同步经 ui.textboxValue 取值） */
    if (g_edit_topic[0]) {
        kwcc_ui_bus_dispatch_event(g_edit_topic, "change");
    }
}

void kwcc_edit_cancel(void) {
    if (g_state != KWCC_EDIT_STATE_EDITING || !g_ops) return;   /* 幂等 */
    void *field = backend_field(g_edit_multiline);
    if (field) {
        g_ops->blur(field);
        g_ops->set_visible(field, 0);
    }
    set_cancelled(g_edit_label);
    g_state = KWCC_EDIT_STATE_CLOSED;
    g_reported = 0;
    log_info("edit: cancelled '%s'", g_edit_label);

    if (g_edit_topic[0]) {
        kwcc_ui_bus_dispatch_event(g_edit_topic, "cancel");
    }
}

int kwcc_edit_is_editing(void) {
    return g_state == KWCC_EDIT_STATE_EDITING;
}

const char *kwcc_edit_editing_label(void) {
    return g_state == KWCC_EDIT_STATE_EDITING ? g_edit_label : NULL;
}

const char *kwcc_edit_committed_label(void) { return g_committed_label; }
const char *kwcc_edit_committed_value(void) { return g_committed_value; }
const char *kwcc_edit_cancelled_label(void) { return g_cancelled_label; }
