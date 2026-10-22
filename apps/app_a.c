/* WASM App A -- draw only its own window buffer */
#include "render_server.h"
static uint32_t g_win = 0;
static const uint32_t A_BASE = RS_RGBA(40, 10, 10, 255);
uint32_t app_a_init(void) {
    g_win = rs_create_window((uint32_t)(uintptr_t)"AppA", 4, 16, 16, 200, 140);
    return g_win;
}
void app_a_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    for (int i = 0; i < w * h; i++) px[i] = A_BASE;
    int bx = (frame * 3) % (w - 20);
    int by = (frame * 2) % (h - 20);
    rs_fill_rect(g_win, bx, by, 20, 20, RS_RGBA(235, 235, 235, 255));
    rs_window_mark_dirty_full(g_win);
}
void app_a_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta) {
    (void)win; (void)type; (void)x; (void)y; (void)meta;
}
