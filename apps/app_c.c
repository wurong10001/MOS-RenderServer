/* WASM App C -- draw only its own window buffer; overlaps App B to test z-order */
#include "render_server.h"
static uint32_t g_win = 0;
static const uint32_t C_BASE = RS_RGBA(10, 10, 40, 255);
uint32_t app_c_init(void) {
    g_win = rs_create_window((uint32_t)(uintptr_t)"AppC", 4, 110, 160, 250, 140);
    return g_win;
}
void app_c_frame(uint32_t win, int32_t frame) {
    (void)win;
    if (!g_win) return;
    int w = rs_window_width(g_win);
    int h = rs_window_height(g_win);
    uint32_t* px = (uint32_t*)rs_window_get_buffer(g_win);
    for (int i = 0; i < w * h; i++) px[i] = C_BASE;
    int cx = w / 2, cy = h / 2;
    int r = 30 + (frame % 20);
    uint32_t disc = RS_RGBA(120, 160, 255, 255);
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy <= r * r) px[(size_t)yy * w + xx] = disc;
        }
    }
    rs_window_mark_dirty_full(g_win);
}
void app_c_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta) {
    (void)win; (void)type; (void)x; (void)y; (void)meta;
}
