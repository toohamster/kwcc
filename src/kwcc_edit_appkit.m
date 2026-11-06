/* kwcc_edit_appkit.m — macOS AppKit 编辑后端（#25 S2）
 *
 * 单行 = NSTextField；多行 = NSTextView(plain) in NSScrollView。
 * IME/光标/选区/剪贴板/undo 由 AppKit 全权处理（first responder → responder chain），
 * 不经过 Sokol 事件管道（见方案 §二）。
 *
 * 提交/取消语义（贴 macOS 原生习惯）：
 *   单行：Enter 提交（doCommandBySelector insertNewline:）、Esc 取消（cancelOperation:）、
 *         点击他处失焦 → 提交（textDidEndEditing）
 *   多行：Enter=换行；Cmd+Enter 提交；Esc 取消；失焦 → 提交
 * kwcc_edit_commit/cancel 自带幂等守卫，delegate 重复回调安全。
 *
 * 挂载：sapp_macos_get_window() 传入的 NSWindow contentView；
 *       window 为空时 fallback keyWindow/mainWindow（headless 冒烟友好）。
 * ARC：C 侧 void* 指针不参与 ARC 引用计数 → 用 holder 对象持强引用防释放。
 */
#import <Cocoa/Cocoa.h>
#include "kwcc_edit_appkit.h"
#include "llog.h"

/* ── delegate：编辑事件 → 宿主 ─────────────────────────────── */

@interface kwcc_edit_delegate : NSObject
@property (assign) int multiline;
@end

@implementation kwcc_edit_delegate

/* 命令拦截核心：单行 Enter 提交；多行 Cmd+Enter 提交、Enter 换行；Esc 取消 */
- (BOOL)handleCommand:(SEL)command {
    if (command == @selector(insertNewline:)) {
        if (_multiline) {
            NSEventModifierFlags flags = NSApp.currentEvent.modifierFlags;
            if (flags & NSEventModifierFlagCommand) {
                kwcc_edit_commit();
                return YES;
            }
            return NO;   /* 普通 Enter = 换行，交给编辑器 */
        }
        kwcc_edit_commit();          /* 单行：Enter 提交 */
        return YES;
    }
    if (command == @selector(cancelOperation:)) {   /* Esc */
        kwcc_edit_cancel();
        return YES;
    }
    return NO;   /* 其余（光标移动/选区/粘贴等）全部原生处理 */
}

/* NSTextField（NSControlTextEditingDelegate 协议） */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView
doCommandBySelector:(SEL)command {
    return [self handleCommand:command];
}

/* NSTextView（NSTextViewDelegate 协议）——多行字段的命令拦截 */
- (BOOL)textView:(NSTextView *)textView doCommandBySelector:(SEL)command {
    return [self handleCommand:command];
}

/* 失焦（点击他处/程序 end editing）→ 提交（cancel 路径已在命令拦截处返回，
 * kwcc_edit_commit/cancel 幂等，重复回调安全） */
- (void)controlTextDidEndEditing:(NSNotification *)note {
    (void)note;
    kwcc_edit_commit();
}

- (void)textDidEndEditing:(NSNotification *)note {
    (void)note;
    kwcc_edit_commit();
}

@end

/* ── holder：ARC 强引用（C 侧 void* 不计数）────────────────── */

@interface kwcc_edit_holder : NSObject
@property (strong) NSTextField *tf;
@property (strong) NSScrollView *sv;
@property (strong) NSTextView *tv;
@property (strong) kwcc_edit_delegate *dlg_single;
@property (strong) kwcc_edit_delegate *dlg_multi;
@end

@implementation kwcc_edit_holder
@end

static kwcc_edit_holder *_holder = nil;   /* 文件级 ObjC 引用，ARC 管理 */
static NSView *_container = nil;          /* contentView */

/* window 由宿主 init 注入（依赖倒置：本适配器不依赖 sokol），
 * 为空时 fallback keyWindow/mainWindow（headless 测试友好） */
static NSView *edit_container(NSWindow *win) {
    if (_container) return _container;
    if (!win) win = NSApp.keyWindow ?: NSApp.mainWindow;
    if (!win) return nil;
    _container = win.contentView;
    return _container;
}

/* ── ops 实现 ─────────────────────────────────────────────── */

/* 多行字段句柄 = NSScrollView（容器，rect/visible 作用对象）；
 * 编辑/取值 = 其 documentView（NSTextView）。单行句柄 = NSTextField 本体。
 * 按 runtime 类型分支，禁止跨类型强转（NSTextField 无 string 属性） */
static NSTextView *edit_text_view(void *field) {
    if ([(__bridge NSView *)field isKindOfClass:[NSScrollView class]]) {
        return (NSTextView *)((__bridge NSScrollView *)field).documentView;
    }
    if ([(__bridge NSView *)field isKindOfClass:[NSTextView class]]) {
        return (__bridge NSTextView *)field;
    }
    return nil;
}

static void *appkit_create_field(void *window, int multiline) {
    if (!_holder) _holder = [[kwcc_edit_holder alloc] init];
    if (!edit_container(window ? (__bridge NSWindow *)window : nil)) {
        NSLog(@"kwcc_edit: no window/container for field (multiline=%d)", multiline);
        return nil;
    }

    if (multiline) {
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)];
        sv.hasVerticalScroller = YES;
        sv.borderType = NSBezelBorder;
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)];
        tv.editable = YES;
        tv.richText = NO;                 /* plain text */
        tv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        sv.documentView = tv;
        kwcc_edit_delegate *dlg = [[kwcc_edit_delegate alloc] init];
        dlg.multiline = 1;
        tv.delegate = dlg;
        [sv setHidden:YES];
        [_container addSubview:sv];
        _holder.sv = sv;
        _holder.tv = tv;
        _holder.dlg_multi = dlg;
        return (__bridge void *)sv;   /* 句柄 = 容器（否则 rect/visible 作用错对象） */
    }

    NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
    tf.editable = YES;
    tf.bezeled = YES;
    tf.drawsBackground = YES;
    kwcc_edit_delegate *dlg = [[kwcc_edit_delegate alloc] init];
    dlg.multiline = 0;
    tf.delegate = dlg;
    [tf setHidden:YES];
    [_container addSubview:tf];
    _holder.tf = tf;
    _holder.dlg_single = dlg;
    return (__bridge void *)tf;
}

static void appkit_destroy_field(void *field) {
    NSView *v = (__bridge NSView *)field;
    [v removeFromSuperview];
    /* holder 强引用由 shutdown 置 nil 释放 */
}

static void appkit_set_value(void *field, const char *text) {
    NSString *s = [NSString stringWithUTF8String:text ? text : ""];
    if (s == nil) s = @"";
    NSTextView *tv = edit_text_view(field);
    if (tv) {
        tv.string = s;
    } else {
        ((__bridge NSTextField *)field).stringValue = s;
    }
}

static void appkit_set_rect(void *field, float x, float y, float w, float h) {
    NSView *v = (__bridge NSView *)field;
    NSView *parent = v.superview;
    /* 实测依据（kwcc.log edit-diag 2026-11-06：容器 flipped=0，直映射产生
     * name 下移 / bio 上移的镜像签名）→ mu 坐标系是 y-down（自顶向下），
     * bottom-origin 容器必须换算 y' = 容器高 - y - h；flipped 容器直映射 */
    float fy = y;
    if (parent && !parent.isFlipped) {
        fy = parent.bounds.size.height - y - h;
    }
    v.frame = NSMakeRect(x, fy, w, h);

    /* 诊断插桩（仅 KWCC_DEBUG 构建生效；rect 变化时才触发，宿主已按变化过滤） */
    static float dx = -1, dy = -1, dw = -1, dh = -1;
    if (x != dx || y != dy || w != dw || h != dh) {
        dx = x; dy = y; dw = w; dh = h;
        kwcc_diag("edit-diag: gl=(%.0f,%.0f,%.0f,%.0f) container(h=%.0f flipped=%d) "
                  "applied=(%.0f,%.0f,%.0f,%.0f)",
                  x, y, w, h, parent.bounds.size.height, parent.isFlipped,
                  x, fy, w, h);
    }
}

static void appkit_set_visible(void *field, int visible) {
    [(__bridge NSView *)field setHidden:!visible];
}

static void appkit_focus(void *field) {
    /* 焦点给实际编辑视图：多行 = sv 内的 documentView，单行 = NSTextField 本体 */
    NSView *target = (NSView *)edit_text_view(field);
    if (!target) target = (__bridge NSView *)field;
    if (target.window) [target.window makeFirstResponder:target];
}

static void appkit_blur(void *field) {
    NSView *v = (__bridge NSView *)field;
    if (v.window && ![v.window.firstResponder isKindOfClass:[NSWindow class]]) {
        [v.window makeFirstResponder:nil];
    }
}

static char *appkit_get_value(void *field) {
    NSString *s;
    NSTextView *tv = edit_text_view(field);
    if (tv) {
        s = tv.string;
    } else {
        s = ((__bridge NSTextField *)field).stringValue;
    }
    return strdup(s.UTF8String ? s.UTF8String : "");
}

const kwcc_edit_backend_ops_t kwcc_edit_appkit_ops = {
    appkit_create_field,
    appkit_destroy_field,
    appkit_set_value,
    appkit_set_rect,
    appkit_set_visible,
    appkit_focus,
    appkit_blur,
    appkit_get_value
};

/* holder/delegate 生命周期：shutdown 置 nil（由 kwcc_edit_shutdown 经
 * destroy_field 后调用者无需额外清理；_holder 在进程生命周期内复用） */
__attribute__((unused)) static void kwcc_edit_appkit_release(void) {
    _holder = nil;
    _container = nil;
}
