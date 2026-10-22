#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_SCALE_MAX_WIDTH 1024
#define VIDEO_SCALE_MAX_HEIGHT 2048

typedef enum {
    VIDEO_SCALE_RGB565,
    VIDEO_SCALE_RGB1555,
    VIDEO_SCALE_XRGB8888
} video_scale_format_t;

typedef struct {
    int src_w, src_h;
    int dst_w, max_game_h;
    int draw_w, draw_x0, game_h;
    int xmap[VIDEO_SCALE_MAX_WIDTH];
    int ymap[VIDEO_SCALE_MAX_HEIGHT];
} video_scale_t;

/* Width-fit scaler with the frontend's integer aspect and nearest maps. */
bool video_scale_init(video_scale_t *ctx, int src_w, int src_h, int dst_w, int max_game_h);

/* dst_stride and pitch are in pixels and bytes respectively. Renders only the
 * game_h rows and dst_w pixels within each destination stride. */
bool video_scale_render(const video_scale_t *ctx, const void *src, size_t pitch,
                        video_scale_format_t format, void *dst, size_t dst_stride);

#endif
