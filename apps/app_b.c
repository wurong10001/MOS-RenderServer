/* WASM App B -- draw only its own window buffer */
#include "render_server.h"
static uint32_t g_win = 0;
static const uint32_t B_BASE = RS_RGBA(10, 40, 10, 255);
uint32_t app_b_init(void) {
    g_win = rs_create_window((uint32_t)(uintptr_t)"AppB", 4, 250, 40, 200, 140);
    return g_win;
}
void app_b_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    for (int i = 0; i < w * h; i++) px[i] = B_BASE;
    int line = frame % h;
    rs_fill_rect(g_win, 0, line, w, 4, RS_RGBA(180, 255, 180, 255));
    rs_window_mark_dirty_full(g_win);
}
void app_b_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta) {
    (void)win; (void)type; (void)x; (void)y; (void)meta;
}
