/* kwcc_edit_appkit.h — macOS AppKit 编辑后端（kwcc_edit_backend_ops_t 实现）
 * 纯文本：单行 NSTextField / 多行 NSTextView(plain, in NSScrollView)
 * 富文本 WKWebView 后端 = 远期另一适配器，接口不变
 */
#ifndef KWCC_EDIT_APPKIT_H
#define KWCC_EDIT_APPKIT_H

#include "kwcc_edit.h"

extern const kwcc_edit_backend_ops_t kwcc_edit_appkit_ops;

#endif
