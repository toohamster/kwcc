/* test_edit_appkit.m — L1.7 headless smoke: AppKit backend ops against real
 * NSTextField / NSTextView (no sokol runtime; ops fall back to keyWindow,
 * the test creates its own NSWindow).
 *
 * Run: ./tests/bin/test_edit_appkit
 */
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/kwcc_edit_appkit.h"

static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* 翻转坐标系容器：模拟生产环境（sokol GL view）top-left origin，
 * 验证 set_rect 的 y 换算（y' = 容器高 - y - h） */
@interface TestFlippedView : NSView
@end
@implementation TestFlippedView
- (BOOL)isFlipped { return YES; }
@end

int main(void) {
    [NSApplication sharedApplication];

    /* 自建 keyWindow：backend 经 keyWindow fallback 挂载 */
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 400, 300)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
    [win makeKeyWindow];

    /* 换入翻转坐标系容器（模拟 sokol GL view；编辑控件挂载其上） */
    TestFlippedView *cv = [[TestFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    win.contentView = cv;
    TEST(container_flipped, cv.isFlipped == YES && win.contentView == cv);

    /* 窗口句柄经宿主注入（生产路径：main.m 传 sapp_macos_get_window()） */
    kwcc_edit_init(&kwcc_edit_appkit_ops, (__bridge void *)win);

    /* ── 单行 ── */
    void *tf = kwcc_edit_appkit_ops.create_field((__bridge void *)win, 0);
    TEST(single_field_created, tf != NULL);

    kwcc_edit_appkit_ops.set_value(tf, "hello-kwcc");
    char *v = kwcc_edit_appkit_ops.get_value(tf);
    TEST(single_value_roundtrip, v && strcmp(v, "hello-kwcc") == 0);
    free(v);

    kwcc_edit_appkit_ops.set_rect(tf, 40, 40, 200, 24);
    TEST(single_rect_on_flipped_container,
         ((__bridge NSView *)tf).frame.origin.x == 40 &&
         ((__bridge NSView *)tf).frame.origin.y == 40);   /* flipped 容器直映射（mu y-down） */
    kwcc_edit_appkit_ops.set_visible(tf, 1);
    kwcc_edit_appkit_ops.focus(tf);
    TEST(single_focused, [win.firstResponder isKindOfClass:[NSTextField class]] ||
                         [win.firstResponder isKindOfClass:[NSText class]]);

    /* 键入/IME/responder 链行为 = L2（需要真实 GUI 会话，headless 进程 window
     * 无法进入 key 状态，field editor 不参与）——L1 只验 ops 调用无异常 */

    kwcc_edit_appkit_ops.blur(tf);
    TEST(blur_releases_first_responder, win.firstResponder == win);

    /* ── 多行 ── */
    void *tv = kwcc_edit_appkit_ops.create_field((__bridge void *)win, 1);
    TEST(multi_field_created, tv != NULL);

    /* 回归（2026-11-05）：句柄必须是容器 NSScrollView——rect/visible 作用在容器上，
     * 编辑在 documentView。旧实现句柄返回 tv 导致 sv 永远 hidden、Bio 无法输入 */
    NSView *mv = (__bridge NSView *)tv;
    TEST(multi_handle_is_scrollview, [mv isKindOfClass:[NSScrollView class]]);
    kwcc_edit_appkit_ops.set_visible(tv, 1);
    TEST(multi_visible_on_container, mv.hidden == NO);
    kwcc_edit_appkit_ops.set_rect(tv, 40, 60, 200, 80);
    TEST(multi_rect_on_container,
         mv.frame.origin.x == 40 && mv.frame.origin.y == 60 &&
         mv.frame.size.width == 200 && mv.frame.size.height == 80);

    kwcc_edit_appkit_ops.set_value(tv, "line1\nline2");
    v = kwcc_edit_appkit_ops.get_value(tv);
    TEST(multi_value_roundtrip, v && strcmp(v, "line1\nline2") == 0);
    free(v);
    kwcc_edit_appkit_ops.set_visible(tv, 0);

    kwcc_edit_appkit_ops.destroy_field(tf);
    kwcc_edit_appkit_ops.destroy_field(tv);
    kwcc_edit_shutdown();

    printf("=== test_edit_appkit: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
