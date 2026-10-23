/* RenderServer 核心实现 (v2: 多租户身份/能力/事件队列/fb 句柄/配额)
 *
 * 纯 C、零 libc 依赖 (自带 arena 分配器与 memcpy/memset), 可直接编译为 wasm32。
 * v1 ABI 全部保留; v2 见 render_server.h 注释。
 * 已定义未启用: TRANSPARENT/MODAL/opacity 合成效果、min-max clamp、parent 联动、
 * quota_fps —— 字段存取生效, 行为由后续版本或宿主实现。
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

/* ===================== 实例注册表 ===================== */
#define RS_MAX_INSTANCES 16

struct rs_inst {
    uint32_t id;        /* slot+1, 0 为 legacy 特权实例 */
    int      used;
    uint32_t uid;
    uint32_t caps;
    uint32_t manifest;
    uint32_t quota_mem; /* 0 = 不限 */
    uint32_t used_mem;
    char     name[32];
    uint32_t name_len;
    uint8_t* cfg;
    uint32_t cfg_len;
};
static struct rs_inst g_inst[RS_MAX_INSTANCES];

static struct rs_inst* inst_by_id(uint32_t id) {
    if (id == 0) return 0;
    for (int i = 0; i < RS_MAX_INSTANCES; i++)
        if (g_inst[i].used && g_inst[i].id == id) return &g_inst[i];
    return 0;
}

/* 能力判定: legacy 实例 0 恒通过; 未知实例按最小权限拒绝 */
static int inst_cap(uint32_t inst, uint32_t cap) {
    if (inst == 0) return 1;
    struct rs_inst* a = inst_by_id(inst);
    return a && (a->caps & cap) ? 1 : 0;
}

/* 配额记账; 返回 1 成功(已记账), 0 拒绝 */
static int inst_charge(uint32_t inst, uint32_t bytes) {
    if (inst == 0) return 1;
    struct rs_inst* a = inst_by_id(inst);
    if (!a) return 0;
    if (a->quota_mem > 0 && a->used_mem + bytes > a->quota_mem) return 0;
    a->used_mem += bytes;
    return 1;
}
static void inst_refund(uint32_t inst, uint32_t bytes) {
    struct rs_inst* a = inst_by_id(inst);
    if (a && a->used_mem >= bytes) a->used_mem -= bytes;
}

/* ===================== 窗口树 + 事件队列 ===================== */
#define RS_MAX_WINDOWS 16
#define RS_EVQ 8

struct rs_evt { int32_t v[4]; };

struct rs_window {
    uint32_t  id;
    int       used;
    int       x, y, w, h;
    int       alloc_w, alloc_h;
    int       z;
    int       visible;
    int       focused;
    uint32_t  inst;
    uint32_t  flags;
    int32_t   min_w, min_h, max_w, max_h; /* 存储, clamp 未启用 */
    int32_t   parent;                     /* 存储, 联动未启用 */
    uint32_t  icon;
    uint32_t  opacity;    /* 0..255, 存储; 合成 alpha 路径未启用 */
    uint32_t  close_pend;
    uint32_t* buf;
    char      title[128];
    uint32_t  title_len;
    struct rs_evt evq[RS_EVQ];
    int ev_head, ev_count;
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
static int win_slot(const struct rs_window* w) { return (int)(w - g_wins); }

static void q_push(struct rs_window* win, int32_t type, int32_t x, int32_t y,
                   int32_t meta) {
    if (!inst_cap(win->inst, RS_CAP_INPUT)) return;
    if (win->ev_count >= RS_EVQ) return; /* 队满丢弃(背压由 app 帧循环保证) */
    int tail = (win->ev_head + win->ev_count) % RS_EVQ;
    win->evq[tail].v[0] = type;
    win->evq[tail].v[1] = x;
    win->evq[tail].v[2] = y;
    win->evq[tail].v[3] = meta;
    win->ev_count++;
}

/* ===================== 单模块 (RS_MONOLITHIC) app 驱动 ===================== */
#ifdef RS_MONOLITHIC
void rs_app_register_all(void);
void rs_apps_init_all(void);
void rs_apps_frame_all(int32_t frame);
#endif

/* ===================== 实例 API ===================== */
uint32_t rs_register_app(uint32_t name_ptr, uint32_t name_len, uint32_t uid,
                         uint32_t capabilities, uint32_t manifest_version) {
    struct rs_inst* a = 0;
    for (int i = 0; i < RS_MAX_INSTANCES; i++)
        if (!g_inst[i].used) { a = &g_inst[i]; break; }
    if (!a) return 0;
    rs_memset(a, 0, sizeof(*a));
    a->used = 1;
    a->id = (uint32_t)(a - g_inst) + 1u;
    a->uid = uid;
    a->caps = capabilities;
    a->manifest = manifest_version;
    if (name_ptr && name_len) {
        uint32_t n = name_len < 31 ? name_len : 31;
        rs_memcpy(a->name, (const void*)(uintptr_t)name_ptr, n);
        a->name[n] = 0;
        a->name_len = n;
    }
    return a->id;
}

uint32_t rs_instance_uid(uint32_t inst) {
    struct rs_inst* a = inst_by_id(inst);
    return a ? a->uid : 0;
}
uint32_t rs_instance_caps(uint32_t inst) {
    struct rs_inst* a = inst_by_id(inst);
    return a ? a->caps : 0;
}
uint32_t rs_instance_manifest_version(uint32_t inst) {
    struct rs_inst* a = inst_by_id(inst);
    return a ? a->manifest : 0;
}
uint32_t rs_instance_name(uint32_t inst, uint32_t dst, uint32_t cap) {
    struct rs_inst* a = inst_by_id(inst);
    if (!a || !dst) return 0;
    uint32_t n = a->name_len < cap ? a->name_len : cap;
    rs_memcpy((void*)(uintptr_t)dst, a->name, n);
    return a->name_len;
}
void rs_set_instance_quota_mem(uint32_t inst, uint32_t quota_bytes) {
    struct rs_inst* a = inst_by_id(inst);
    if (a) a->quota_mem = quota_bytes;
}
uint32_t rs_instance_used_mem(uint32_t inst) {
    struct rs_inst* a = inst_by_id(inst);
    return a ? a->used_mem : 0;
}
void rs_set_instance_config(uint32_t inst, uint32_t ptr, uint32_t len) {
    struct rs_inst* a = inst_by_id(inst);
    if (!a) return;
    if (len > 4096) len = 4096;
    if (len && !ptr) return;
    uint8_t* copy = len ? (uint8_t*)rs_alloc(len) : 0;
    if (len && !copy) return;
    if (len) rs_memcpy(copy, (const void*)(uintptr_t)ptr, len);
    rs_free(a->cfg);
    a->cfg = copy;
    a->cfg_len = len;
}
uint32_t rs_instance_config_len(uint32_t inst) {
    struct rs_inst* a = inst_by_id(inst);
    return a ? a->cfg_len : 0;
}
uint32_t rs_instance_get_config(uint32_t inst, uint32_t dst, uint32_t cap) {
    struct rs_inst* a = inst_by_id(inst);
    if (!a || !dst) return 0;
    uint32_t n = a->cfg_len < cap ? a->cfg_len : cap;
    if (n && a->cfg) rs_memcpy((void*)(uintptr_t)dst, a->cfg, n);
    return a->cfg_len;
}

/* ===================== 窗口创建/销毁 ===================== */
static uint32_t win_create(uint32_t inst, uint32_t title_ptr,
                           uint32_t title_len, int32_t x, int32_t y, int32_t w,
                           int32_t h, uint32_t flags, int32_t min_w,
                           int32_t min_h, int32_t max_w, int32_t max_h,
                           int32_t parent) {
    if (w <= 0 || h <= 0 || g_win_count >= RS_MAX_WINDOWS) return 0;
    if ((size_t)w > RS_HEAP_SIZE / 4u / (size_t)h) return 0;
    if (!inst_cap(inst, RS_CAP_RENDER)) return 0;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
    if (!inst_charge(inst, bytes)) return 0;
    struct rs_window* win = 0;
    for (int i = 0; i < RS_MAX_WINDOWS; i++)
        if (!g_wins[i].used) { win = &g_wins[i]; break; }
    if (!win) { inst_refund(inst, bytes); return 0; }
    uint32_t* buf = (uint32_t*)rs_alloc((size_t)w * (size_t)h * 4u);
    if (!buf) { inst_refund(inst, bytes); return 0; }
    rs_memset(buf, 0, (size_t)w * (size_t)h * 4u);

    win->id = g_next_id++;
    win->used = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->alloc_w = w; win->alloc_h = h;
    win->z = g_win_count;
    win->inst = inst;
    win->flags = flags;
    win->visible = (flags & RS_FLAG_VISIBLE) ? 1 : 0;
    win->focused = 0;
    win->min_w = min_w; win->min_h = min_h; win->max_w = max_w; win->max_h = max_h;
    win->parent = parent;
    win->icon = 0;
    win->opacity = 255;
    win->close_pend = 0;
    win->buf = buf;
    win->ev_head = win->ev_count = 0;
    if (title_ptr && title_len) {
        uint32_t n = title_len < 127 ? title_len : 127;
        rs_memcpy(win->title, (const void*)(uintptr_t)title_ptr, n);
        win->title[n] = 0;
        win->title_len = n;
    } else {
        win->title[0] = 0;
        win->title_len = 0;
    }
    g_win_count++;
    dirty_union(x, y, w, h);
    return win->id;
}

uint32_t rs_create_window(uint32_t title_ptr, uint32_t title_len, int32_t x,
                          int32_t y, int32_t w, int32_t h) {
    return win_create(0, title_ptr, title_len, x, y, w, h,
                      RS_FLAG_VISIBLE | RS_FLAG_FOCUSABLE, 0, 0, 0, 0, 0);
}

uint32_t rs_create_window2(uint32_t inst, uint32_t title_ptr,
                           uint32_t title_len, int32_t x, int32_t y, int32_t w,
                           int32_t h, uint32_t flags, int32_t min_w,
                           int32_t min_h, int32_t max_w, int32_t max_h,
                           int32_t parent_window_id) {
    if (inst != 0 && !inst_by_id(inst)) return 0;
    return win_create(inst, title_ptr, title_len, x, y, w, h, flags, min_w,
                      min_h, max_w, max_h, parent_window_id);
}

void rs_destroy_window(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    dirty_union(win->x, win->y, win->w, win->h);
    inst_refund(win->inst, (uint32_t)win->alloc_w * (uint32_t)win->alloc_h * 4u);
    rs_free(win->buf);
    if (g_focused == win->id) g_focused = 0;
    rs_memset(win, 0, sizeof(*win));
    g_win_count--;
}

/* ===================== 窗口几何/状态 ===================== */
void rs_window_set_rect(uint32_t id, int32_t x, int32_t y, int32_t w,
                        int32_t h) {
    struct rs_window* win = win_by_id(id);
    if (!win || w <= 0 || h <= 0) return;
    if (!inst_cap(win->inst, RS_CAP_RENDER)) return;
    int resized = (win->w != w || win->h != h);
    if (resized && (win->alloc_w != w || win->alloc_h != h)) {
        if ((size_t)w > RS_HEAP_SIZE / 4u / (size_t)h) return;
        uint32_t new_bytes = (uint32_t)((size_t)w * (size_t)h * 4u);
        uint32_t old_bytes =
            (uint32_t)((size_t)win->alloc_w * (size_t)win->alloc_h * 4u);
        uint32_t extra_bytes = new_bytes > old_bytes ? new_bytes - old_bytes : 0;
        if (!inst_charge(win->inst, extra_bytes)) return;

        uint32_t* new_buf = (uint32_t*)rs_alloc(new_bytes);
        if (!new_buf) {
            inst_refund(win->inst, extra_bytes);
            return;
        }
        rs_memset(new_buf, 0, new_bytes);
        int copy_w = win->alloc_w < w ? win->alloc_w : w;
        int copy_h = win->alloc_h < h ? win->alloc_h : h;
        for (int row = 0; row < copy_h; row++) {
            rs_memcpy(&new_buf[(size_t)row * (size_t)w],
                      &win->buf[(size_t)row * (size_t)win->alloc_w],
                      (size_t)copy_w * 4u);
        }

        rs_free(win->buf);
        win->buf = new_buf;
        win->alloc_w = w;
        win->alloc_h = h;
        if (old_bytes > new_bytes)
            inst_refund(win->inst, old_bytes - new_bytes);
    }
    dirty_union(win->x, win->y, win->w, win->h);
    win->x = x; win->y = y; win->w = w; win->h = h;
    dirty_union(x, y, w, h);
    if (resized) q_push(win, RS_EV_RESIZE, w, h, 0);
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
    if (visible) win->flags |= RS_FLAG_VISIBLE;
    else win->flags &= ~RS_FLAG_VISIBLE;
    dirty_union(win->x, win->y, win->w, win->h);
}

void rs_window_focus(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (!(win->flags & RS_FLAG_FOCUSABLE)) return;
    uint32_t prev_id = (g_focused == win->id) ? 0 : g_focused;
    for (int i = 0; i < RS_MAX_WINDOWS; i++) g_wins[i].focused = 0;
    win->focused = 1;
    g_focused = id;
    int maxz = 0;
    for (int i = 0; i < RS_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].z > maxz) maxz = g_wins[i].z;
    win->z = maxz + 1;
    dirty_union(win->x, win->y, win->w, win->h);
    struct rs_window* prev = prev_id ? win_by_id(prev_id) : 0;
    if (prev) q_push(prev, RS_EV_BLUR, 0, 0, 0);
    q_push(win, RS_EV_FOCUS, 0, 0, 0);
}

uint32_t rs_window_get_buffer(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return 0;
    return (uint32_t)(uintptr_t)win->buf;
}

/* fb 句柄: slot+1, 与窗口对象分离 */
uint32_t rs_window_get_fb(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? (uint32_t)(win_slot(win) + 1) : 0;
}
uint32_t rs_fb_addr(uint32_t fb) {
    if (fb == 0 || fb > RS_MAX_WINDOWS) return 0;
    struct rs_window* w = &g_wins[fb - 1];
    return w->used ? (uint32_t)(uintptr_t)w->buf : 0;
}
uint32_t rs_fb_w(uint32_t fb) {
    if (fb == 0 || fb > RS_MAX_WINDOWS) return 0;
    struct rs_window* w = &g_wins[fb - 1];
    return w->used ? (uint32_t)w->alloc_w : 0;
}
uint32_t rs_fb_h(uint32_t fb) {
    if (fb == 0 || fb > RS_MAX_WINDOWS) return 0;
    struct rs_window* w = &g_wins[fb - 1];
    return w->used ? (uint32_t)w->alloc_h : 0;
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
uint32_t rs_window_get_flags(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->flags : 0;
}
int32_t rs_window_parent(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->parent : 0;
}
uint32_t rs_window_instance(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->inst : 0;
}

void rs_set_window_title(uint32_t id, uint32_t ptr, uint32_t len) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (len > 127) len = 127;
    if (ptr && len) {
        rs_memcpy(win->title, (const void*)(uintptr_t)ptr, len);
        win->title[len] = 0;
        win->title_len = len;
    } else {
        win->title[0] = 0;
        win->title_len = 0;
    }
}
uint32_t rs_window_get_title(uint32_t id, uint32_t dst, uint32_t cap) {
    struct rs_window* win = win_by_id(id);
    if (!win || !dst) return 0;
    uint32_t n = win->title_len < cap ? win->title_len : cap;
    rs_memcpy((void*)(uintptr_t)dst, win->title, n);
    return win->title_len;
}
void rs_set_window_icon(uint32_t id, uint32_t icon_res_id) {
    struct rs_window* win = win_by_id(id);
    if (win) win->icon = icon_res_id;
}
uint32_t rs_window_get_icon(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->icon : 0;
}
void rs_window_set_opacity(uint32_t id, uint32_t opacity0_255) {
    struct rs_window* win = win_by_id(id);
    if (win) win->opacity = opacity0_255 > 255 ? 255 : opacity0_255;
}
uint32_t rs_window_get_opacity(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->opacity : 0;
}

/* ===================== 绘制 (需 RENDER) ===================== */
void rs_window_mark_dirty(uint32_t id, int32_t x, int32_t y, int32_t w,
                          int32_t h) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (!inst_cap(win->inst, RS_CAP_RENDER)) return;
    int sx = win->x + x, sy = win->y + y;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    dirty_union(sx, sy, w, h);
}

void rs_window_mark_dirty_full(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (!inst_cap(win->inst, RS_CAP_RENDER)) return;
    dirty_union(win->x, win->y, win->w, win->h);
}

void rs_fill_rect(uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    if (!inst_cap(win->inst, RS_CAP_RENDER)) return;
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
    if (!inst_cap(win->inst, RS_CAP_RENDER)) return;
    if (x < 0 || x >= win->w || y < 0 || y >= win->h) return;
    win->buf[(size_t)y * win->w + x] = color;
}

/* ===================== 合成 (逻辑与 v1 一致) ===================== */
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
    rs_memset(g_inst, 0, sizeof(g_inst));
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
    (void)frame;
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
        if (w->parent != 0) continue; /* 联动未启用, 暂不阻断命中 */
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (w->z > bestz) { bestz = w->z; best = i; }
        }
    }
    return best >= 0 ? g_wins[best].id : 0;
}

void rs_dispatch_event(int32_t type, int32_t x, int32_t y, int32_t meta) {
    int32_t wid = rs_hit_test(x, y);
    if (!wid) return;
    struct rs_window* win = win_by_id((uint32_t)wid);
    if (!win) return;
    q_push(win, type, x, y, meta);
}

int32_t rs_window_poll_event(uint32_t id, uint32_t out_ptr) {
    struct rs_window* win = win_by_id(id);
    if (!win || win->ev_count <= 0 || !out_ptr) return 0;
    int32_t* out = (int32_t*)(uintptr_t)out_ptr;
    out[0] = win->evq[win->ev_head].v[0];
    out[1] = win->evq[win->ev_head].v[1];
    out[2] = win->evq[win->ev_head].v[2];
    out[3] = win->evq[win->ev_head].v[3];
    win->ev_head = (win->ev_head + 1) % RS_EVQ;
    win->ev_count--;
    return 1;
}

void rs_window_close_request(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    if (!win) return;
    win->close_pend = 1;
    q_push(win, RS_EV_CLOSE_REQ, 0, 0, 0);
}
uint32_t rs_window_close_pending(uint32_t id) {
    struct rs_window* win = win_by_id(id);
    return win ? win->close_pend : 0;
}
void rs_window_close_ack(uint32_t id) {
    rs_destroy_window(id);
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
