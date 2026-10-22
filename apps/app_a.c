/* WASM App A (v2) -- 注册实例, 只画自己的窗口 buffer */
#include "render_server.h"
static uint32_t g_inst = 0;
static uint32_t g_win = 0;
static int32_t  g_evq[4];
static int32_t  s_down = 0;
static const uint32_t A_BASE = RS_RGBA(40, 10, 10, 255);
static const uint32_t A_HOT = RS_RGBA(80, 30, 30, 255);

uint32_t app_a_init(void) {
    uint32_t name = (uint32_t)(uintptr_t)"AppA";
    g_inst = rs_register_app(name, 4, 1000, RS_CAP_RENDER | RS_CAP_INPUT, 1);
    g_win = rs_create_window2(g_inst, name, 4, 16, 16, 200, 140,
                              RS_FLAG_VISIBLE | RS_FLAG_FOCUSABLE, 0, 0, 0, 0, 0);
    return g_win;
}
void app_a_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    while (rs_window_poll_event(g_win, (uint32_t)(uintptr_t)g_evq))
        if (g_evq[0] == RS_EV_POINTER_DOWN || g_evq[0] == RS_EV_POINTER_MOVE)
            s_down = 1;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    uint32_t base = s_down ? A_HOT : A_BASE;
    for (int i = 0; i < w * h; i++) px[i] = base;
    int bx = (frame * 3) % (w - 20);
    int by = (frame * 2) % (h - 20);
    rs_fill_rect(g_win, bx, by, 20, 20, RS_RGBA(235, 235, 235, 255));
    rs_window_mark_dirty_full(g_win);
}
