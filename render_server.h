#pragma once
#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 * RenderServer  —  多租户 WASM 渲染服务 (C ABI / WASM 导入函数)
 *
 * 架构:
 *   ┌──────────────────────────────────────────────┐
 *   │              WASM RenderServer               │
 *   │  framebuffer 池 / 窗口树 / 合成 / 脏区        │
 *   │  实例注册表(app_id/uid/caps/quota) + 事件路由 │
 *   │  暴露 C ABI / WASM 导入函数                   │
 *   └──────────────────────────────────────────────┘
 *                  │
 *   ┌──────────────┴───────────────────────────────┐
 *   │              JS Host / WebGPU                │
 *   │  事件转发(经 rs_dispatch_event 入队)、        │
 *   │  帧同步、最终提交                             │
 *   └──────────────────────────────────────────────┘
 *
 * 像素格式: RGBA8, 与现有 sample.wasm 约定一致 —— 小端存储 = 字节序 R,G,B,A
 *
 * v2 (WindowSpec 修订版, WASM 友好):
 *   - 调用方身份: rs_register_app() 返回 instance_id, 之后所有窗口都归属实例;
 *     兼容层: v1 rs_create_window 归入特权 legacy 实例 0。
 *   - 不使用函数指针回调: 事件/生命周期经 server 每窗口环形队列投递,
 *     app 在帧循环里 rs_window_poll_event() 取出 (type,x,y,meta)。
 *   - 不使用大结构体 ABI: 创建走标量 + flags 位域 (rs_create_window2)。
 *   - window_id 与 fb 句柄分离: rs_window_get_fb/rs_fb_addr/rs_fb_w/rs_fb_h。
 *   - capabilities 位掩码在 server 侧强制 (RENDER 建窗/绘制, INPUT 收事件);
 *     quota_mem 在窗口分配处强制。
 *   - 已定义未启用(仅存取, 行为待后续): TRANSPARENT/opacity(合成无 alpha
 *     混合路径)、min/max+resizable(宿主决定尺寸)、parent 联动(仅存储)、
 *     MODAL、quota_fps(宿主限速)。
 * ========================================================================= */

#define RS_RGBA(r, g, b, a)                                                 \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) |             \
                ((uint32_t)(g) << 8) | (uint32_t)(r)))

/* 编译模式: RS_COMPILE_SERVER / RS_MONOLITHIC / (其它=独立 app 从 render_server 导入) */
#if defined(RS_COMPILE_SERVER) || defined(RS_MONOLITHIC)
#define RS_IMPORT(name)
#else
#define RS_IMPORT(name)                                                      \
    __attribute__((import_module("render_server"), import_name(name)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Host 导入 (env): host_log / host_get_time / host_submit ---- */
extern void host_log(uint32_t ptr, uint32_t len);
extern uint32_t host_get_time(void);
extern void host_submit(uint32_t screen_offset, uint32_t w, uint32_t h,
                        uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh);

/* ---- flags 位 (rs_create_window2) ---- */
#define RS_FLAG_VISIBLE     (1u << 0)
#define RS_FLAG_FOCUSABLE   (1u << 1)
#define RS_FLAG_RESIZABLE   (1u << 2) /* 存储, 宿主执行 */
#define RS_FLAG_MODAL       (1u << 3) /* 存储, 未启用 */
#define RS_FLAG_TRANSPARENT (1u << 4) /* 存储, 合成 alpha 路径未启用 */

/* ---- capabilities 位 (rs_register_app) ---- */
#define RS_CAP_RENDER  (1u << 0)
#define RS_CAP_INPUT   (1u << 1)
#define RS_CAP_STORAGE (1u << 2) /* server 暂无存储 API, 预留 */
#define RS_CAP_NETWORK (1u << 3) /* 宿主侧使用, server 仅存储 */

/* ---- 事件类型 (队列载荷) ---- */
#define RS_EV_POINTER_DOWN 1
#define RS_EV_POINTER_MOVE 2
#define RS_EV_POINTER_UP   3
#define RS_EV_KEY          4  /* x=键码, 经 meta 传 */
#define RS_EV_FOCUS        10
#define RS_EV_BLUR         11
#define RS_EV_RESIZE       12 /* x=新宽 y=新高 */
#define RS_EV_CLOSE_REQ    20

/* ======================= v1 ABI (保留, 兼容) ======================= */
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

/* ======================= v2 ABI ======================= */
/* 实例注册: 返回 instance_id(>=1); 表满返回 0 */
RS_IMPORT("rs_register_app")
uint32_t rs_register_app(uint32_t name_ptr, uint32_t name_len, uint32_t uid,
                         uint32_t capabilities, uint32_t manifest_version);
RS_IMPORT("rs_instance_uid")
uint32_t rs_instance_uid(uint32_t inst);
RS_IMPORT("rs_instance_caps")
uint32_t rs_instance_caps(uint32_t inst);
RS_IMPORT("rs_instance_manifest_version")
uint32_t rs_instance_manifest_version(uint32_t inst);
/* 复制实例名到 dst(线性内存), 返回实际长度(可能大于 cap, 截断写入) */
RS_IMPORT("rs_instance_name")
uint32_t rs_instance_name(uint32_t inst, uint32_t dst, uint32_t cap);
RS_IMPORT("rs_set_instance_quota_mem")
void rs_set_instance_quota_mem(uint32_t inst, uint32_t quota_bytes);
RS_IMPORT("rs_instance_used_mem")
uint32_t rs_instance_used_mem(uint32_t inst);
RS_IMPORT("rs_set_instance_config")
void rs_set_instance_config(uint32_t inst, uint32_t ptr, uint32_t len);
RS_IMPORT("rs_instance_config_len")
uint32_t rs_instance_config_len(uint32_t inst);
RS_IMPORT("rs_instance_get_config")
uint32_t rs_instance_get_config(uint32_t inst, uint32_t dst, uint32_t cap);

/* v2 建窗: flags/min/max/parent; 需 RENDER 能力, 计 quota */
RS_IMPORT("rs_create_window2")
uint32_t rs_create_window2(uint32_t inst, uint32_t title_ptr,
                           uint32_t title_len, int32_t x, int32_t y, int32_t w,
                           int32_t h, uint32_t flags, int32_t min_w,
                           int32_t min_h, int32_t max_w, int32_t max_h,
                           int32_t parent_window_id);
RS_IMPORT("rs_window_get_flags")
uint32_t rs_window_get_flags(uint32_t id);
RS_IMPORT("rs_window_parent")
int32_t rs_window_parent(uint32_t id);
RS_IMPORT("rs_window_instance")
uint32_t rs_window_instance(uint32_t id);
RS_IMPORT("rs_set_window_title")
void rs_set_window_title(uint32_t id, uint32_t ptr, uint32_t len);
RS_IMPORT("rs_window_get_title")
uint32_t rs_window_get_title(uint32_t id, uint32_t dst, uint32_t cap);
RS_IMPORT("rs_set_window_icon")
void rs_set_window_icon(uint32_t id, uint32_t icon_res_id);
RS_IMPORT("rs_window_get_icon")
uint32_t rs_window_get_icon(uint32_t id);
RS_IMPORT("rs_window_set_opacity")
void rs_window_set_opacity(uint32_t id, uint32_t opacity0_255);
RS_IMPORT("rs_window_get_opacity")
uint32_t rs_window_get_opacity(uint32_t id);

/* fb 句柄与窗口分离 */
RS_IMPORT("rs_window_get_fb")
uint32_t rs_window_get_fb(uint32_t id);
RS_IMPORT("rs_fb_addr")
uint32_t rs_fb_addr(uint32_t fb);
RS_IMPORT("rs_fb_w")
uint32_t rs_fb_w(uint32_t fb);
RS_IMPORT("rs_fb_h")
uint32_t rs_fb_h(uint32_t fb);

/* 事件队列: 弹出最早事件, 写 4 个 int32(type,x,y,meta) 到 out_ptr; 返回 1/0 */
RS_IMPORT("rs_window_poll_event")
int32_t rs_window_poll_event(uint32_t id, uint32_t out_ptr);

/* 关闭拦截: request 置挂起并投递 RS_EV_CLOSE_REQ; ack 才真正销毁 */
RS_IMPORT("rs_window_close_request")
void rs_window_close_request(uint32_t id);
RS_IMPORT("rs_window_close_pending")
uint32_t rs_window_close_pending(uint32_t id);
RS_IMPORT("rs_window_close_ack")
void rs_window_close_ack(uint32_t id);

/* ======================= Host 入口 ======================= */
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
