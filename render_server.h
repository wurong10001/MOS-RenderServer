#pragma once
#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 * RenderServer  —  多租户 WASM 渲染服务 (C ABI / WASM 导入函数)
 *
 * 架构 (见仓库根目录架构图):
 *   ┌──────────────────────────────────────────────┐
 *   │              WASM RenderServer               │
 *   │  管理 framebuffer 池、窗口树、合成、脏区       │
 *   │  暴露 C ABI / WASM 导入函数                   │
 *   └──────────────┬───────────────────────────────┘
 *                  │ 调用渲染 API
 *   ┌──────────────┼──────────────┬───────────────┐
 *   │ WASM App A   │ WASM App B   │ WASM App C    │
 *   │ 只画自己的    │ 只画自己的    │ 只画自己的     │
 *   │ 窗口 buffer  │ 窗口 buffer  │ 窗口 buffer   │
 *   └──────────────┴──────────────┴───────────────┘
 *                  │
 *   ┌──────────────┴───────────────────────────────┐
 *   │              JS Host / WebGPU                │
 *   │  事件转发、帧同步、最终提交                     │
 *   └──────────────────────────────────────────────┘
 *
 * 像素格式: RGBA8, 与现有 sample.wasm 约定一致 —— 小端存储 = 字节序 R,G,B,A
 *   即 32 位像素值 (a<<24)|(b<<16)|(g<<8)|r  => 0xFF000000 为不透明黑
 * ========================================================================= */

/* 像素构造宏 (host 端 / app 端通用) */
#define RS_RGBA(r, g, b, a)                                                 \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) |             \
                ((uint32_t)(g) << 8) | (uint32_t)(r)))

/* ---------------------------------------------------------------------------
 * 编译模式 (由 build 脚本注入宏):
 *   RS_COMPILE_SERVER : 正在编译 RenderServer 本体 (rs_* 为定义)
 *   RS_MONOLITHIC     : 单模块整合构建 (server+apps 链接在一起, rs_* 为普通 extern)
 *   其它               : 独立 wasm app, rs_* 从 "render_server" 模块导入
 * ------------------------------------------------------------------------- */
#if defined(RS_COMPILE_SERVER) || defined(RS_MONOLITHIC)
#define RS_IMPORT(name)
#else
#define RS_IMPORT(name)                                                      \
    __attribute__((import_module("render_server"), import_name(name)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Host 导入函数 (由 JS Host / WASM3 宿主提供, 模块名 "env")
 * ------------------------------------------------------------------------- */
extern void host_log(uint32_t ptr, uint32_t len);
extern uint32_t host_get_time(void);
extern void host_submit(uint32_t screen_offset, uint32_t w, uint32_t h,
                        uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh);

/* ---------------------------------------------------------------------------
 * 应用侧 C ABI (App 调用; 同时也是 server 模块导出的函数)
 * ------------------------------------------------------------------------- */
RS_IMPORT("rs_create_window")
uint32_t rs_create_window(uint32_t title_ptr, uint32_t title_len, int32_t x,
                          int32_t y, int32_t w, int32_t h);

RS_IMPORT("rs_destroy_window")
void rs_destroy_window(uint32_t id);

RS_IMPORT("rs_window_set_rect")
void rs_window_set_rect(uint32_t id, int32_t x, int32_t y, int32_t w,
                        int32_t h);

RS_IMPORT("rs_window_set_pos")
void rs_window_set_pos(uint32_t id, int32_t x, int32_t y);

RS_IMPORT("rs_window_set_z")
void rs_window_set_z(uint32_t id, int32_t z);

RS_IMPORT("rs_window_show")
void rs_window_show(uint32_t id, int32_t visible);

RS_IMPORT("rs_window_focus")
void rs_window_focus(uint32_t id);

RS_IMPORT("rs_window_get_buffer")
uint32_t rs_window_get_buffer(uint32_t id);

RS_IMPORT("rs_window_width")
int32_t rs_window_width(uint32_t id);
RS_IMPORT("rs_window_height")
int32_t rs_window_height(uint32_t id);
RS_IMPORT("rs_window_x")
int32_t rs_window_x(uint32_t id);
RS_IMPORT("rs_window_y")
int32_t rs_window_y(uint32_t id);

RS_IMPORT("rs_window_mark_dirty")
void rs_window_mark_dirty(uint32_t id, int32_t x, int32_t y, int32_t w,
                          int32_t h);
RS_IMPORT("rs_window_mark_dirty_full")
void rs_window_mark_dirty_full(uint32_t id);

RS_IMPORT("rs_fill_rect")
void rs_fill_rect(uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                 uint32_t color);
RS_IMPORT("rs_put_pixel")
void rs_put_pixel(uint32_t id, int32_t x, int32_t y, uint32_t color);

/* ---------------------------------------------------------------------------
 * Server / Host 入口 API (由 server 模块导出, JS Host 调用)
 * ------------------------------------------------------------------------- */
RS_IMPORT("rs_init")
uint32_t rs_init(void);

RS_IMPORT("rs_request_redraw")
void rs_request_redraw(void);

RS_IMPORT("rs_composite")
void rs_composite(void);

RS_IMPORT("rs_frame")
void rs_frame(int32_t frame);

RS_IMPORT("rs_dispatch_event")
void rs_dispatch_event(int32_t type, int32_t x, int32_t y, int32_t meta);

RS_IMPORT("rs_hit_test")
int32_t rs_hit_test(int32_t x, int32_t y);

RS_IMPORT("rs_get_screen_buffer")
uint32_t rs_get_screen_buffer(void);
RS_IMPORT("rs_get_screen_w")
int32_t rs_get_screen_w(void);
RS_IMPORT("rs_get_screen_h")
int32_t rs_get_screen_h(void);

RS_IMPORT("rs_get_dirty_x")
int32_t rs_get_dirty_x(void);
RS_IMPORT("rs_get_dirty_y")
int32_t rs_get_dirty_y(void);
RS_IMPORT("rs_get_dirty_w")
int32_t rs_get_dirty_w(void);
RS_IMPORT("rs_get_dirty_h")
int32_t rs_get_dirty_h(void);

#ifdef __cplusplus
}
#endif

/* 事件类型常量 (与 JS Host 侧保持一致) */
#define RS_EVT_POINTER_DOWN 1
#define RS_EVT_POINTER_MOVE 2
#define RS_EVT_POINTER_UP   3
#define RS_EVT_KEY          4
