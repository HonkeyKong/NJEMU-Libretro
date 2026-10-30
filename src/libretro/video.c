/* CPU implementation of every operation used by the shared sprite renderers.
 * All software and texture draws share the same BGR5551 work buffer. */
#include "emumain.h"
#include <stdlib.h>

typedef struct {
    uint16_t work[BUF_WIDTH * SCR_HEIGHT];
    uint16_t draw[BUF_WIDTH * SCR_HEIGHT];
    uint16_t depth[BUF_WIDTH * SCR_HEIGHT];
    uint8_t *texture[8];
    layer_texture_info_t info[8];
    unsigned count;
    int left, top, right, bottom, depth_test;
} lr_video;
void *video_data, *show_frame, *draw_frame, *work_frame;
int video_mode;
RECT full_rect = {0, 0, BUF_WIDTH, SCR_HEIGHT};
uint32_t lr_framebuffer[384 * 224];

static void release(void *data)
{
    lr_video *v = data;
    if (!v) return;
    for (unsigned i = 0; i < v->count; ++i) free(v->texture[i]);
    free(v);
    show_frame = draw_frame = work_frame = NULL;
}
static void *init(layer_texture_info_t *layers, uint8_t count, clut_info_t *clut)
{
    lr_video *v = calloc(1, sizeof(*v));
    (void)clut;
    if (!v || count > 8) { free(v); return NULL; }
    v->count = count;
    for (unsigned i = 0; i < count; ++i) {
        v->info[i] = layers[i];
        v->texture[i] = calloc(layers[i].width * layers[i].height, layers[i].bytes_per_pixel);
        if (!v->texture[i]) { release(v); return NULL; }
    }
    work_frame = v->work; draw_frame = show_frame = v->draw;
    v->right = BUF_WIDTH; v->bottom = SCR_HEIGHT;
    return v;
}
static void noop(void *data) { (void)data; }
static void flip(void *data, bool sync) { (void)data; (void)sync; }
static void *addr(void *data, void *frame, int x, int y) { (void)data; return (uint16_t *)frame + y * BUF_WIDTH + x; }
static void *work(void *data) { return ((lr_video *)data)->work; }
static void *texture(void *data, uint8_t index) { lr_video *v = data; return index < v->count ? v->texture[index] : NULL; }
static void scissor(void *data, uint16_t l, uint16_t t, uint16_t r, uint16_t b)
{
    lr_video *v = data;
    v->left = l < BUF_WIDTH ? l : BUF_WIDTH;
    v->right = r < BUF_WIDTH ? r : BUF_WIDTH;
    v->top = t < SCR_HEIGHT ? t : SCR_HEIGHT;
    v->bottom = b < SCR_HEIGHT ? b : SCR_HEIGHT;
}
static void clear(void *data) { lr_video *v = data; memset(v->work, 0, sizeof(v->work)); memset(v->draw, 0, sizeof(v->draw)); }
static void clear_frame(void *data, int index)
{
    lr_video *v = data;
    memset(index == COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP ? v->work : v->draw, 0, sizeof(v->draw));
}
static void fill(void *data, void *frame, uint32_t color)
{
    (void)data;
    uint16_t c = MAKECOL15(GETR32(color), GETG32(color), GETB32(color));
    for (unsigned i = 0; i < BUF_WIDTH * SCR_HEIGHT; ++i) ((uint16_t *)frame)[i] = c;
}
static void start(void *data, uint32_t color)
{
    lr_video *v = data;
    fill(v, v->work, color);
    v->depth_test = 0;
}
static void copy_mode(void *data, void *src, void *dst, RECT *sr, RECT *dr, int mode)
{
    (void)data;
    int sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    int dw = dr->right - dr->left, dh = dr->bottom - dr->top;
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
        int dx = dr->left + x, dy = dr->top + y;
        int sx = x * sw / dw, sy = y * sh / dh;
        if (mode == 1) { sx = sw - 1 - sx; sy = sh - 1 - sy; }
        if (mode == 2) { sx = sw - 1 - y * sw / dh; sy = x * sh / dw; }
        sx += sr->left; sy += sr->top;
        if (dx >= 0 && dx < BUF_WIDTH && dy >= 0 && dy < SCR_HEIGHT && sx >= 0 && sx < BUF_WIDTH && sy >= 0 && sy < SCR_HEIGHT)
            ((uint16_t *)dst)[dy * BUF_WIDTH + dx] = ((uint16_t *)src)[sy * BUF_WIDTH + sx];
    }
}
static void copy(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,0); }
static void copy_flip(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,1); }
static void copy_rotate(void *d, void *s, void *t, RECT *sr, RECT *dr) { copy_mode(d,s,t,sr,dr,2); }
static void transfer(void *d, RECT *s, RECT *t) { lr_video *v = d; copy(d,v->work,v->draw,s,t); }
static void draw_texture(void *d, uint32_t sf, uint32_t df, void *s, void *t, RECT *sr, RECT *dr) { (void)sf; (void)df; copy(d,s,t,sr,dr); }
static void *native(void *data, int index)
{
    lr_video *v = data;
    if (index == COMMON_GRAPHIC_OBJECTS_SCREEN_BITMAP) return v->work;
    if (index == COMMON_GRAPHIC_OBJECTS_INITIAL_TEXTURE_LAYER) return v->texture[0];
    if (index == COMMON_GRAPHIC_OBJECTS_GLOBAL_CONTEXT) return v;
    return v->draw;
}
static void upload(void *data, uint8_t i) { (void)data; (void)i; }
static void upload_clut(void *data, uint16_t *p, uint8_t b) { (void)data; (void)p; (void)b; }
/* Texel for destination pixel p of a quad spanning [p0, p0+len) with texels
 * [t0, t1): samples at the pixel centre, t0 + (2*(p-p0)+1)*(t1-t0) / (2*len),
 * truncated toward zero. Reversed texels describe flipped tiles. Exact integer
 * form, so callers can hoist it out of the per-pixel loop. */
static int texel(int p, int p0, int len, int t0, int t1)
{
    /* Texels are 16-bit; spans this short keep every term below 2^30, so use
       the hardware 32-bit divide (64-bit is a libgcc call on 32-bit ARM). */
    if (len <= 4096 && (unsigned)(p - p0) < 4096u)
        return (t0 * 2 * len + (2 * (p - p0) + 1) * (t1 - t0)) / (2 * len);
    int64_t d = 2 * (int64_t)len;
    return (int)(((int64_t)t0 * d + (2 * (int64_t)(p - p0) + 1) * (t1 - t0)) / d);
}
static void blit(void *data, uint8_t index, void *palette, uint8_t bank, uint32_t count, void *vertices)
{
    lr_video *v = data;
    struct Vertex *vs = vertices;
    uint16_t *clut = palette;
    int ucol[BUF_WIDTH]; /* texel column per destination column, -1 if outside */
    (void)bank; /* palette already points at the selected bank and subpalette */
    if (index >= v->count) return;
    layer_texture_info_t info = v->info[index];
    const int indexed = info.bytes_per_pixel == 1;
    if (indexed && !clut) return; /* every texel would be transparent */
    const uint8_t *tex8 = v->texture[index];
    const uint16_t *tex16 = (const uint16_t *)v->texture[index];
    for (unsigned i = 0; i + 1 < count; i += 2) {
        struct Vertex a = vs[i], b = vs[i+1];
        int w = b.x - a.x, h = b.y - a.y;
        if (w <= 0 || h <= 0) continue;
        int l = a.x > v->left ? a.x : v->left;
        int t = a.y > v->top ? a.y : v->top;
        int r = b.x < v->right ? b.x : v->right;
        int bot = b.y < v->bottom ? b.y : v->bottom;
        if (l >= r) continue;
        for (int x = l; x < r; ++x) {
            int u = texel(x, a.x, w, a.u, b.u);
            ucol[x - l] = (u < 0 || (size_t)u >= info.width) ? -1 : u;
        }
        uint16_t z = (uint16_t)a.z;
        for (int y = t; y < bot; ++y) {
            int tv = texel(y, a.y, h, a.v, b.v);
            if (tv < 0 || (size_t)tv >= info.height) continue;
            size_t row = (size_t)tv * info.width;
            uint16_t *dst = v->work + y * BUF_WIDTH, *dep = v->depth + y * BUF_WIDTH;
            for (int x = l; x < r; ++x) {
                int u = ucol[x - l];
                if (u < 0) continue;
                uint16_t c = indexed ? clut[tex8[row + u]] : tex16[row + u];
                if (c & 0x8000) continue;
                if (v->depth_test) {
                    if (z < dep[x]) continue;
                    dep[x] = z;
                }
                dst[x] = c;
            }
        }
    }
}
static void points(void *data, uint32_t count, void *vertices)
{
    lr_video *v = data;
    struct PointVertex *p = vertices;
    for (unsigned i = 0; i < count; ++i)
        if (p[i].x >= v->left && p[i].x < v->right && p[i].y >= v->top && p[i].y < v->bottom)
            v->work[p[i].y * BUF_WIDTH + p[i].x] = p[i].color;
}
static void flush(void *data, void *addr, size_t size) { (void)data; (void)addr; (void)size; }
static void depth_on(void *data) { ((lr_video *)data)->depth_test = 1; }
static void depth_off(void *data) { ((lr_video *)data)->depth_test = 0; }
static void depth_clear(void *data) { lr_video *v = data; memset(v->depth, 0, sizeof(v->depth)); }
static void color_clear(void *data) { lr_video *v = data; memset(v->work, 0, sizeof(v->work)); }
static video_driver_t cpu_video = {
    "libretro", init, release, noop, flip, addr, work, texture, scissor,
    clear, clear_frame, fill, start, transfer, copy, copy_flip, copy_rotate,
    draw_texture, native, upload, upload_clut, blit, points, flush,
    depth_on, depth_off, depth_clear, color_clear
};
video_driver_t *video_drivers[] = {&cpu_video, NULL};
void lr_video_submit(void)
{
    lr_video *v = video_data;
    int left = 16, top = 16, flipped = 0;
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    left = 64;
    flipped = cps_flip_screen != 0;
#endif
    if (!v) return;
    for (unsigned y = 0; y < lr_height; ++y) for (unsigned x = 0; x < lr_width; ++x) {
        unsigned sx = flipped ? lr_width - 1 - x : x;
        unsigned sy = flipped ? lr_height - 1 - y : y;
        uint16_t c = v->work[(top + sy) * BUF_WIDTH + left + sx];
        lr_framebuffer[y * lr_width + x] = (GETR15(c) << 16) | (GETG15(c) << 8) | GETB15(c);
    }
}
