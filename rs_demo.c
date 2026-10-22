/* Monolithic (RS_MONOLITHIC) app registry: registers A/B/C into the server */
#include "render_server.h"
uint32_t app_a_init(void);
void    app_a_frame(uint32_t win, int32_t frame);
void    app_a_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta);
uint32_t app_b_init(void);
void    app_b_frame(uint32_t win, int32_t frame);
void    app_b_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta);
uint32_t app_c_init(void);
void    app_c_frame(uint32_t win, int32_t frame);
void    app_c_event(uint32_t win, int32_t type, int32_t x, int32_t y, int32_t meta);
struct rs_appreg {
    const char* name;
    uint32_t (*init)(void);
    void (*frame)(uint32_t, int32_t);
    void (*event)(uint32_t, int32_t, int32_t, int32_t, int32_t);
    uint32_t win;
};
static struct rs_appreg g_apps[3];
void rs_app_register_all(void) {
    g_apps[0] = (struct rs_appreg){"app_a", app_a_init, app_a_frame, app_a_event, 0};
    g_apps[1] = (struct rs_appreg){"app_b", app_b_init, app_b_frame, app_b_event, 0};
    g_apps[2] = (struct rs_appreg){"app_c", app_c_init, app_c_frame, app_c_event, 0};
}
void rs_apps_init_all(void) {
    for (int i = 0; i < 3; i++) g_apps[i].win = g_apps[i].init();
}
void rs_apps_frame_all(int32_t frame) {
    for (int i = 0; i < 3; i++)
        if (g_apps[i].win) g_apps[i].frame(g_apps[i].win, frame);
}
void rs_apps_event(int32_t type, int32_t x, int32_t y, int32_t meta) {
    int32_t wid = rs_hit_test(x, y);
    if (!wid) return;
    for (int i = 0; i < 3; i++)
        if (g_apps[i].win == (uint32_t)wid) {
            g_apps[i].event(g_apps[i].win, type, x, y, meta);
            break;
        }
}
