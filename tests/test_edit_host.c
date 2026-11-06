/* test_edit_host.c — L1 for kwcc_edit host (S1)
 *
 * FAKE backend records backend call sequence; host state machine/registry/
 * focus-authority/auto-commit asserted against it. This also proves the ops
 * interface isolation: the whole host is testable without AppKit.
 *
 * Covers (plan §六): L1.1 begin sequence / L1.2 commit registry / L1.3 cancel
 * L1.4 focus authority / L1.5 visibility sync / L1.6 idempotency / auto-commit
 * (change-event JS delivery asserted separately in tests/test_edit_js.c, S3)
 *
 * Run: ./tests/bin/test_edit_host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "src/kwcc_edit.h"

static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── FAKE backend: record call sequence ── */
static char g_calls[4096];
static size_t g_len = 0;
static int g_creates = 0;
static char g_last_value[256];

static void rec(const char *s) {
    g_len += (size_t)snprintf(g_calls + g_len, sizeof(g_calls) - g_len, "%s;", s);
}
static void reset_calls(void) { g_len = 0; g_calls[0] = '\0'; }

static void *fake_create(void *window, int multiline) {
    (void)window;
    g_creates++;
    rec(multiline ? "create_multi" : "create_single");
    return (void *)(intptr_t)(0x1000 + g_creates);
}
static void fake_destroy(void *field) { (void)field; rec("destroy"); }
static void fake_set_value(void *field, const char *text) {
    (void)field;
    snprintf(g_last_value, sizeof(g_last_value), "%s", text ? text : "");
    rec("set_value");
}
static void fake_set_rect(void *field, float x, float y, float w, float h) {
    (void)field; (void)x; (void)y; (void)w; (void)h; rec("set_rect");
}
static void fake_set_visible(void *field, int visible) {
    (void)field; rec(visible ? "show" : "hide");
}
static void fake_focus(void *field) { (void)field; rec("focus"); }
static void fake_blur(void *field) { (void)field; rec("blur"); }
static char *fake_get_value(void *field) {
    (void)field; rec("get_value");
    return strdup("typed-value");
}

static const kwcc_edit_backend_ops_t g_mock_ops = {
    fake_create, fake_destroy, fake_set_value, fake_set_rect,
    fake_set_visible, fake_focus, fake_blur, fake_get_value
};

#define FIELD_A "fieldA"
static void report_a(int clicked, int visible) {
    kwcc_edit_report_field(FIELD_A, "store-value-A", "form/a", 0,
                           50, 50, 200, 30, clicked, visible);
}

int main(void) {
    kwcc_edit_init(&g_mock_ops, (void *)0xDEAD);
    TEST(init_closed, !kwcc_edit_is_editing());

    /* L1.1: click on closed → begin sequence */
    reset_calls();
    report_a(1, 1);
    TEST(begin_sequence, strstr(g_calls,
        "create_single;set_value;set_rect;show;focus;") != NULL);
    TEST(state_editing, kwcc_edit_is_editing());
    TEST(editing_label_a, strcmp(kwcc_edit_editing_label(), FIELD_A) == 0);
    TEST(value_seeded_from_store, strcmp(g_last_value, "store-value-A") == 0);

    /* L1.6a: editing 中重复 begin 同一字段 → 幂等（无新调用） */
    reset_calls();
    report_a(1, 1);
    TEST(rebegin_same_field_noop, g_len == 0 && kwcc_edit_is_editing());

    /* L1.4: editing 中点击其他字段 → 拒绝，状态不变 */
    reset_calls();
    kwcc_edit_report_field("fieldB", "store-value-B", "form/b", 0,
                           50, 90, 200, 30, 1, 1);
    TEST(focus_authority_reject, g_len == 0 &&
         strcmp(kwcc_edit_editing_label(), FIELD_A) == 0);

    /* L1.5: 可见性同步 */
    reset_calls();
    report_a(0, 0);   /* 字段被遮挡/滚出 → hide */
    TEST(visibility_hide, strstr(g_calls, "hide;") != NULL);
    reset_calls();
    report_a(0, 1);   /* 恢复 → show */
    TEST(visibility_show, strstr(g_calls, "show;") != NULL);

    /* L1.2: commit → get_value → 注册表交付（幂等） */
    reset_calls();
    kwcc_edit_commit();
    TEST(commit_sequence, strstr(g_calls, "get_value;blur;hide;") != NULL);
    TEST(commit_state_closed, !kwcc_edit_is_editing());
    TEST(commit_registry_label, strcmp(kwcc_edit_committed_label(), FIELD_A) == 0);
    TEST(commit_registry_value, strcmp(kwcc_edit_committed_value(), "typed-value") == 0);
    reset_calls();
    kwcc_edit_commit();   /* 二次 commit 幂等 */
    TEST(commit_idempotent, g_len == 0);

    /* L1.3: cancel → 原值保留，注册表不更新，cancel 事件源记录 */
    report_a(1, 1);       /* 重新进入编辑 */
    kwcc_edit_cancel();
    TEST(cancel_state_closed, !kwcc_edit_is_editing());
    TEST(cancel_registry_label, strcmp(kwcc_edit_cancelled_label(), FIELD_A) == 0);
    TEST(cancel_keeps_commit_registry, strcmp(kwcc_edit_committed_value(), "typed-value") == 0);

    /* auto-commit：编辑中字段缺报（窗口关闭/模块隐藏）→ begin_frame 提交 */
    reset_calls();
    report_a(1, 1);                      /* frame N: begin（reported=1） */
    kwcc_edit_begin_frame();             /* frame N 结束：reported=1 → 不提交 */
    TEST(frame_with_report_no_commit, kwcc_edit_is_editing());
    kwcc_edit_begin_frame();             /* frame N+1: 缺报 → 自动提交 */
    TEST(missing_report_autocommit, !kwcc_edit_is_editing() &&
         strcmp(kwcc_edit_committed_value(), "typed-value") == 0);

    /* 多行字段：独立后端实例 */
    reset_calls();
    kwcc_edit_report_field("bio", "hello", "form/bio", 1,
                           50, 130, 200, 100, 1, 1);
    TEST(multiline_backend_instance, strstr(g_calls, "create_multi;") != NULL);
    kwcc_edit_commit();

    /* L1.7 位置：AppKit 后端在 S2（test_edit_appkit 冒烟） */
    kwcc_edit_shutdown();
    printf("=== test_edit_host: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
