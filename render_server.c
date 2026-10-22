/* RenderServer 核心实现
 *
 * 纯 C、零 libc 依赖 (自带 arena 分配器与 memcpy/memset), 可直接编译为 wasm32,
 * 也可在宿主 (WASM3 / 原生测试) 中链接运行。
 *
 * 职责: framebuffer 池 / 窗口树 / 合成 / 脏区。
 * 像素格式 RGBA8 (字节序 R,G,B,A), 与 sample.wasm 约定一致。
 */
#include "render_server.h"

/* ===================== 基本内存工具 (避免 libc) ===================== */
static void rs_memset(void* dst, uint8_t v, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) p[i] = v;
}
static void rs_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* ===================== framebuffer 池 (arena 分配器) ===================== */
#define RS_HEAP_SIZE (6u * 1024u * 1024u)
static uint8_t g_heap[RS_HEAP_SIZE];
static size_t  g_heap_top;

struct rs_blk {
    size_t         size;
    struct rs_blk* next;
};
static struct rs_blk* g_free;

static void* rs_alloc(size_t n) {
    if (n == 0) n = 1;
    n = (n + 15u) & ~(size_t)15u;
    struct rs_blk* prev = NULL;
    struct rs_blk* b = g_free;
    while (b) {
        if (b->size >= n) {
            size_t leftover = b->size - n;
            if (leftover >= sizeof(struct rs_blk) + 16) {
                struct rs_blk* nb =
                    (struct rs_blk*)((uint8_t*)b + sizeof(struct rs_blk) + n);
                nb->size = leftover - sizeof(struct rs_blk);
                nb->next = b->next;
                if (prev) prev->next = nb; else g_free = nb;
                b->size = n;
            } else {
                if (prev) prev->next = b->next; else g_free = b->next;
            }
            return (uint8_t*)b + sizeof(struct rs_blk);
        }
        prev = b;
        b = b->next;
    }
    size_t need = sizeof(struct rs_blk) + n;
    if (g_heap_top + need > RS_HEAP_SIZE) return 0;
    struct rs_blk* h = (struct rs_blk*)(g_heap + g_heap_top);
    g_heap_top += need;
    h->size = n;
    h->next = NULL;
    return (uint8_t*)h + sizeof(struct rs_blk);
}

static void rs_free(void* p) {
    if (!p) return;
    struct rs_blk* b = (struct rs_blk*)((uint8_t*)p - sizeof(struct rs_blk));
    b->next = g_free;
    g_free = b;
}

/* ===================== 屏幕 / 全局脏区 ===================== */
#ifndef RS_SCREEN_W
#define RS_SCREEN_W 480
#endif
#ifndef RS_SCREEN_H
#define RS_SCREEN_H 320
#endif

static uint32_t* g_screen = 0;
static uint32_t  g_bg = RS_RGBA(18, 20, 28, 255);

static int g_dx = 0, g_dy = 0, g_dw = 0, g_dh = 0;
static int g_dirty_empty = 1;
static int g_ldx = 0, g_ldy = 0, g_ldw = 0, g_ldh = 0;

static void dirty_union(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_dirty_empty) {
        g_dx = x; g_dy = y; g_dw = w; g_dh = h; g_dirty_empty = 0;
        return;
    }
    int x2 = g_dx + g_dw, y2 = g_dy + g_dh;
    int nx2 = x + w, ny2 = y + h;
    if (x < g_dx) g_dx = x;
    if (y < g_dy) g_dy = y;
    if (nx2 > x2) x2 = nx2;
    if (ny2 > y2) y2 = ny2;
    g_dw = x2 - g_dx;
    g_dh = y2 - g_dy;
}

/* ===================== 窗口树 ===================== */
#define RS_MAX_WINDOWS 16

struct rs_window {
    uint32_t  id;
    int       used;
    int       x, y, w, h;
    int       z;
    int       visible;
    int       focused;
    uint32_t* buf;
    char      title[32];
};

static struct rs_window g_wins[RS_MAX_WINDOWS];
static uint32_t g_next_id = 1;
static uint32_t g_focused = 0;
static int      g_win_count = 0;

static struct rs_window* win_by_id(uint32_t id) {
    for (int i = 0; i < RS_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id) return &g_wins[i];
    return 0;
}

/* ===================== 单模块 (RS_MONOLITHIC) 下的 app 驱动 ===================== */
#ifdef RS_MONOLITHIC
void rs_app_register_all(void);
void rs_apps_init_all(void);
void rs_apps_frame_all(int32_t frame);
void rs_apps_event(int32_t type, int32_t x, int32_t y, int32_t meta);
#endif

/* ===================== 公共 C ABI 实现 ===================== */
uint32_t rs_create_window(uint32_t title_ptr, uint32_t title_len, int32_t x,
                          int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0 || g_win_count >= RS_MAX_WINDOWS) return 0;
    struct rs_window* win = 0;
    for (int i = 0; i < RS_MAX_WINDOWS; i++)
        if (!g_wins[i].used) { win = &g_wins[i]; break; }
    if (!win) return 0;
    uint32_t* buf = (uint32_t*)rs_alloc((size_t)w * (size_t)h * 4u);
    if (!buf) return 0;
    rs_memset(buf, 0, (size_t)w * (size_t)h * 4u);
    win->id = g_next_id++;
    win->used = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->z = g_win_count;
    win->visible = 1;
    win->focused = 0;
    win->buf = buf;
    if (title_ptr && title_len) {
        uint32_t n = title_len < 31 ? title_len : 31;
        rs_memcpy(win->title, (const void*)(uintptr_t)title_ptr, n);
        win->title[n] = 0;
    } else {
        win->title[0] = 0;
    }
    g_win_count++;
    dirty_union(x, y, w, h);
    return win->id;
}

void rs_destroy_window(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    dirty_union(win->x, win->y, win->w, win->h);
    rs_free(win->buf);
    if (g_focused == win->id) g_focused = 0;
    rs_memset(win, 0, sizeof(*win));
    g_win_count--;
}

void rs_window_set_rect(uint32_t id, int32_t x, int32_t y, int32_t w,
                        int32_t h) {
    struct rs_window* win = win_by_id(id);
    if (!win || w <= 0 || h <= 0) return;
    dirty_union(win->x, win->y, win->w, win->h);
    win->x = x; win->y = y; win->w = w; win->h = h;
    dirty_union(x, y, w, h);
}

void rs_window_set_pos(uint32_t id, int32_t x, int32_t y) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    dirty_union(win->x, win->y, win->w, win->h);
    win->x = x; win->y = y;
    dirty_union(x, y, win->w, win->h);
}

void rs_window_set_z(uint32_t id, int32_t z) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    win->z = z;
    dirty_union(win->x, win->y, win->w, win->h);
}

void rs_window_show(uint32_t id, int32_t visible) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    win->visible = visible ? 1 : 0;
    dirty_union(win->x, win->y, win->w, win->h);
}

void rs_window_focus(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    for (int i = 0; i < RS_MAX_WINDOWS; i++) g_wins[i].focused = 0;
    win->focused = 1;
    g_focused = id;
    int maxz = 0;
    for (int i = 0; i < RS_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].z > maxz) maxz = g_wins[i].z;
    win->z = maxz + 1;
    dirty_union(win->x, win->y, win->w, win->h);
}

uint32_t rs_window_get_buffer(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return 0;
    return (uint32_t)(uintptr_t)win->buf;
}

int32_t rs_window_width(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->w : 0;
}
int32_t rs_window_height(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->h : 0;
}
int32_t rs_window_x(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->x : 0;
}
int32_t rs_window_y(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->y : 0;
}

void rs_window_mark_dirty(uint32_t id, int32_t x, int32_t y, int32_t w,
                          int32_t h) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    int sx = win->x + x, sy = win->y + y;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    dirty_union(sx, sy, w, h);
}

void rs_window_mark_dirty_full(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    dirty_union(win->x, win->y, win->w, win->h);
}

void rs_fill_rect(uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                 uint32_t color) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= win->h) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= win->w) continue;
            win->buf[(size_t)yy * win->w + xx] = color;
        }
    }
}

void rs_put_pixel(uint32_t id, int32_t x, int32_t y, uint32_t color) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (x < 0 || x >= win->w || y < 0 || y >= win->h) return;
    win->buf[(size_t)y * win->w + x] = color;
}

/* ===================== 合成 ===================== */
static void composite_region(int dx, int dy, int dw, int dh) {
    int done[RS_MAX_WINDOWS] = {0};
    for (int yy = dy; yy < dy + dh; yy++) {
        if (yy < 0 || yy >= RS_SCREEN_H) continue;
        for (int xx = dx; xx < dx + dw; xx++) {
            if (xx < 0 || xx >= RS_SCREEN_W) continue;
            g_screen[(size_t)yy * RS_SCREEN_W + xx] = g_bg;
        }
    }
    for (int pass = 0; pass < RS_MAX_WINDOWS; pass++) {
        int best = -1, bestz = 0x7fffffff;
        for (int i = 0; i < RS_MAX_WINDOWS; i++) {
            if (!g_wins[i].used || !g_wins[i].visible) continue;
        if (done[i]) continue;
            if (g_wins[i].z < bestz) { bestz = g_wins[i].z; best = i; }
        }
        if (best < 0) break;
        done[best] = 1;
        struct rs_window* win = &g_wins[best];
        int ix = win->x > dx ? win->x : dx;
        int iy = win->y > dy ? win->y : dy;
        int ix2 = (win->x + win->w) < (dx + dw) ? (win->x + win->w) : (dx + dw);
        int iy2 = (win->y + win->h) < (dy + dh) ? (win->y + win->h) : (dy + dh);
        if (ix2 <= ix || iy2 <= iy) continue;
        for (int yy = iy; yy < iy2; yy++) {
            size_t src_row = ((size_t)(yy - win->y)) * win->w + (ix - win->x);
            size_t dst_row = (size_t)yy * RS_SCREEN_W + ix;
            rs_memcpy(&g_screen[dst_row], &win->buf[src_row],
                      (size_t)(ix2 - ix) * 4u);
        }
    }
}

void rs_composite(void) {
    if (g_dirty_empty) return;
    int dx = g_dx, dy = g_dy, dw = g_dw, dh = g_dh;
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > RS_SCREEN_W) dw = RS_SCREEN_W - dx;
    if (dy + dh > RS_SCREEN_H) dh = RS_SCREEN_H - dy;
    if (dw <= 0 || dh <= 0) { g_dirty_empty = 1; return; }
    composite_region(dx, dy, dw, dh);
    g_ldx = dx; g_ldy = dy; g_ldw = dw; g_ldh = dh;
    host_submit((uint32_t)(uintptr_t)g_screen, RS_SCREEN_W, RS_SCREEN_H,
                (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh);
    g_dirty_empty = 1;
    g_dx = g_dy = g_dw = g_dh = 0;
}

void rs_request_redraw(void) {
    dirty_union(0, 0, RS_SCREEN_W, RS_SCREEN_H);
}

/* ===================== 入口 / 帧 / 事件 ===================== */
uint32_t rs_init(void) {
    g_heap_top = 0;
    g_free = 0;
    g_dirty_empty = 1;
    g_focused = 0;
    g_win_count = 0;
    rs_memset(g_wins, 0, sizeof(g_wins));
    g_screen = (uint32_t*)rs_alloc((size_t)RS_SCREEN_W * RS_SCREEN_H * 4u);
    if (!g_screen) return 0;
    rs_memset(g_screen, 0, (size_t)RS_SCREEN_W * RS_SCREEN_H * 4u);
    rs_request_redraw();
#ifdef RS_MONOLITHIC
    rs_app_register_all();
    rs_apps_init_all();
#endif
    return (uint32_t)(uintptr_t)g_screen;
}

void rs_frame(int32_t frame) {
    (void)frame; /* used only in monolithic build */
#ifdef RS_MONOLITHIC
    rs_apps_frame_all(frame);
#endif
    rs_composite();
}

int32_t rs_hit_test(int32_t x, int32_t y) {
    int best = -1, bestz = -1;
    for (int i = 0; i < RS_MAX_WINDOWS; i++) {
        struct rs_window* w = &g_wins[i];
        if (!w->used || !w->visible) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (w->z > bestz) { bestz = w->z; best = i; }
        }
    }
    return best >= 0 ? g_wins[best].id : 0;
}

void rs_dispatch_event(int32_t type, int32_t x, int32_t y, int32_t meta) {
#ifdef RS_MONOLITHIC
    (void)meta; (void)type; (void)x; (void)y;
    rs_apps_event(type, x, y, meta);
#else
    (void)type; (void)x; (void)y; (void)meta;
#endif
}

uint32_t rs_get_screen_buffer(void) {
    return (uint32_t)(uintptr_t)g_screen;
}
int32_t rs_get_screen_w(void) { return RS_SCREEN_W; }
int32_t rs_get_screen_h(void) { return RS_SCREEN_H; }
int32_t rs_get_dirty_x(void) { return g_ldx; }
int32_t rs_get_dirty_y(void) { return g_ldy; }
int32_t rs_get_dirty_w(void) { return g_ldw; }
int32_t rs_get_dirty_h(void) { return g_ldh; }
