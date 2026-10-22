/* WASM App B (v2) -- 注册实例, 只画自己的窗口 buffer */
#include "render_server.h"
static uint32_t g_inst = 0;
static uint32_t g_win = 0;
static int32_t  g_evq[4];
static int32_t  s_alt = 0;
static const uint32_t B_BASE = RS_RGBA(10, 40, 10, 255);

uint32_t app_b_init(void) {
    uint32_t name = (uint32_t)(uintptr_t)"AppB";
    g_inst = rs_register_app(name, 4, 1001, RS_CAP_RENDER | RS_CAP_INPUT, 1);
    g_win = rs_create_window2(g_inst, name, 4, 250, 40, 200, 140,
                              RS_FLAG_VISIBLE | RS_FLAG_FOCUSABLE, 0, 0, 0, 0, 0);
    return g_win;
}
void app_b_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    while (rs_window_poll_event(g_win, (uint32_t)(uintptr_t)g_evq))
        if (g_evq[0] == RS_EV_POINTER_DOWN) s_alt ^= 1;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    for (int i = 0; i < w * h; i++) px[i] = B_BASE;
    int line = frame % h;
    rs_fill_rect(g_win, 0, line, w, 4,
                 s_alt ? RS_RGBA(255, 255, 0, 255) : RS_RGBA(180, 255, 180, 255));
    rs_window_mark_dirty_full(g_win);
}
