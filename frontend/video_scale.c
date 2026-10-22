#include "video_scale.h"

#include <string.h>

typedef uint32_t __attribute__((may_alias)) video_scale_pair_t;

static uint16_t read_pixel(const uint8_t *row, int x, video_scale_format_t format) {
    if (format == VIDEO_SCALE_XRGB8888) {
        uint32_t p;
        memcpy(&p, row + (size_t)x * 4, sizeof(p));
        return (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
    }
    uint16_t p;
    memcpy(&p, row + (size_t)x * 2, sizeof(p));
    if (format == VIDEO_SCALE_RGB565) return p;
    return (uint16_t)(((p & 0x7fe0) << 1) | (p & 0x001f));
}

bool video_scale_init(video_scale_t *ctx, int src_w, int src_h, int dst_w, int max_game_h) {
    if (!ctx || src_w < 1 || src_h < 1 || dst_w < 1 || max_game_h < 1 ||
        dst_w > VIDEO_SCALE_MAX_WIDTH || max_game_h > VIDEO_SCALE_MAX_HEIGHT ||
        src_w > VIDEO_SCALE_MAX_WIDTH || src_h > VIDEO_SCALE_MAX_HEIGHT) return false;
    ctx->src_w = src_w;
    ctx->src_h = src_h;
    ctx->dst_w = dst_w;
    ctx->max_game_h = max_game_h;
    int h = (int)((int64_t)src_h * dst_w / src_w);
    int w = dst_w;
    if (h > max_game_h) {
        h = max_game_h;
        w = (int)((int64_t)src_w * h / src_h);
        if (w < 1) w = 1;
        if (w > dst_w) w = dst_w;
    }
    ctx->draw_w = w;
    ctx->draw_x0 = (dst_w - w) / 2;
    ctx->game_h = h < 1 ? 1 : h;
    for (int x = 0; x < w; x++) ctx->xmap[x] = (int)((int64_t)x * src_w / w);
    for (int y = 0; y < ctx->game_h; y++) ctx->ymap[y] = (int)((int64_t)y * src_h / ctx->game_h);
    return true;
}

bool video_scale_render(const video_scale_t *ctx, const void *src, size_t pitch,
                        video_scale_format_t format, void *dst, size_t dst_stride) {
    if (!ctx || !src || !dst || ctx->src_w < 1 || ctx->src_h < 1 || ctx->draw_w < 1 ||
        ctx->draw_w > ctx->dst_w || ctx->dst_w > VIDEO_SCALE_MAX_WIDTH ||
        ctx->game_h < 1 || ctx->game_h > VIDEO_SCALE_MAX_HEIGHT || dst_stride < (size_t)ctx->dst_w ||
        (format != VIDEO_SCALE_RGB565 && format != VIDEO_SCALE_RGB1555 && format != VIDEO_SCALE_XRGB8888)) return false;
    size_t bpp = format == VIDEO_SCALE_XRGB8888 ? 4u : 2u;
    if ((size_t)ctx->src_w > SIZE_MAX / bpp || pitch < (size_t)ctx->src_w * bpp ||
        ((size_t)ctx->src_h > 1u && pitch > SIZE_MAX / (size_t)ctx->src_h) ||
        dst_stride > SIZE_MAX / 2u / (size_t)ctx->game_h) return false;

    uint16_t line[VIDEO_SCALE_MAX_WIDTH] __attribute__((aligned(8)));
    const uint8_t *bytes = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    int last_sy = -1;
    for (int y = 0; y < ctx->game_h; y++) {
        int sy = ctx->ymap[y];
        uint8_t *drow = out + (size_t)y * dst_stride * 2u;
        if (sy != last_sy) {
            memset(line, 0, (size_t)ctx->dst_w * sizeof(*line));
            const uint8_t *srow = bytes + (size_t)sy * pitch;
            /* Preserve the existing 320 -> 480 RGB565 row specialization. */
            if (format == VIDEO_SCALE_RGB565 && ctx->draw_x0 == 0 && ctx->src_w % 4 == 0 &&
                ctx->draw_w * 2 == ctx->src_w * 3) {
                for (int x = 0; x < ctx->src_w; x += 4) {
                    uint16_t s[4];
                    memcpy(s, srow + (size_t)x * 2u, sizeof(s));
                    line[x * 3 / 2] = s[0]; line[x * 3 / 2 + 1] = s[0];
                    line[x * 3 / 2 + 2] = s[1]; line[x * 3 / 2 + 3] = s[2];
                    line[x * 3 / 2 + 4] = s[2]; line[x * 3 / 2 + 5] = s[3];
                }
            } else if (format == VIDEO_SCALE_RGB565 && ctx->draw_x0 == 0 &&
                       ctx->draw_w % ctx->src_w == 0 && ctx->draw_w / ctx->src_w >= 2 &&
                       ctx->draw_w / ctx->src_w <= 4) {
                /* Emit each pair of source pixels as the exact 2x/3x/4x
                 * replication pattern. The line buffer is aligned for words. */
                int factor = ctx->draw_w / ctx->src_w;
                int x = 0;
                video_scale_pair_t *pairs = (video_scale_pair_t *)line;
                for (; x + 1 < ctx->src_w; x += 2) {
                    uint16_t p[2];
                    memcpy(p, srow + (size_t)x * 2u, sizeof(p));
                    int o = x * factor;
                    uint32_t p0p0 = p[0] | ((uint32_t)p[0] << 16);
                    uint32_t p1p1 = p[1] | ((uint32_t)p[1] << 16);
                    pairs[o / 2] = p0p0;
                    if (factor == 2) {
                        pairs[o / 2 + 1] = p1p1;
                    } else if (factor == 3) {
                        pairs[o / 2 + 1] = p[0] | ((uint32_t)p[1] << 16);
                        pairs[o / 2 + 2] = p1p1;
                    } else {
                        pairs[o / 2 + 1] = p0p0;
                        pairs[o / 2 + 2] = p1p1;
                        pairs[o / 2 + 3] = p1p1;
                    }
                }
                if (x < ctx->src_w) {
                    uint16_t p;
                    memcpy(&p, srow + (size_t)x * 2u, sizeof(p));
                    for (int n = 0; n < factor; n++) line[x * factor + n] = p;
                }
            } else if (format == VIDEO_SCALE_RGB565 && ctx->draw_w % 2 == 0 &&
                       ctx->draw_x0 % 2 == 0) {
                /* Mapped RGB565 fallback: retain two destination pixels per store. */
                video_scale_pair_t *pairs = (video_scale_pair_t *)(line + ctx->draw_x0);
                for (int x = 0; x < ctx->draw_w; x += 2) {
                    uint16_t p[2];
                    memcpy(&p[0], srow + (size_t)ctx->xmap[x] * 2u, sizeof(p[0]));
                    memcpy(&p[1], srow + (size_t)ctx->xmap[x + 1] * 2u, sizeof(p[1]));
                    pairs[x / 2] = p[0] | ((uint32_t)p[1] << 16);
                }
            } else {
                for (int x = 0; x < ctx->draw_w; x++)
                    line[ctx->draw_x0 + x] = read_pixel(srow, ctx->xmap[x], format);
            }
            last_sy = sy;
        }
        /* memcpy is valid for aligned and unaligned targets and leaves stride padding intact. */
        memcpy(drow, line, (size_t)ctx->dst_w * sizeof(*line));
    }
    return true;
}
