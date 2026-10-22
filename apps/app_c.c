/* WASM App C (v2) -- 注册实例, 只画自己的窗口 buffer; 与 App B 重叠测 z-order */
#include "render_server.h"
static uint32_t g_inst = 0;
static uint32_t g_win = 0;
static int32_t  g_evq[4];
static int32_t  s_grow = 0;
static const uint32_t C_BASE = RS_RGBA(10, 10, 40, 255);

uint32_t app_c_init(void) {
    uint32_t name = (uint32_t)(uintptr_t)"AppC";
    g_inst = rs_register_app(name, 4, 1002, RS_CAP_RENDER | RS_CAP_INPUT, 1);
    g_win = rs_create_window2(g_inst, name, 4, 110, 160, 250, 140,
                              RS_FLAG_VISIBLE | RS_FLAG_FOCUSABLE, 0, 0, 0, 0, 0);
    return g_win;
}
void app_c_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    while (rs_window_poll_event(g_win, (uint32_t)(uintptr_t)g_evq))
        if (g_evq[0] == RS_EV_KEY) s_grow ^= 1;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    for (int i = 0; i < w * h; i++) px[i] = C_BASE;
    int cx = w / 2, cy = h / 2;
    int r = 30 + (frame % 20) + (s_grow ? 20 : 0);
    uint32_t disc = RS_RGBA(120, 160, 255, 255);
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy <= r * r) px[(size_t)yy * w + xx] = disc;
        }
    }
    rs_window_mark_dirty_full(g_win);
}
