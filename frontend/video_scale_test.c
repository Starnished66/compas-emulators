#include "video_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static uint16_t refpixel(const uint8_t *row, int x, video_scale_format_t f) {
    if (f == VIDEO_SCALE_XRGB8888) {
        uint32_t p; memcpy(&p, row + (size_t)x * 4, 4);
        return (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x7e0) | ((p >> 3) & 0x1f));
    }
    uint16_t p; memcpy(&p, row + (size_t)x * 2, 2);
    return f == VIDEO_SCALE_RGB565 ? p : (uint16_t)(((p & 0x7fe0) << 1) | (p & 0x1f));
}

static void one_case(int sw, int sh, int dw, int maxh, video_scale_format_t f, int odd) {
    video_scale_t c;
    CHECK(video_scale_init(&c, sw, sh, dw, maxh), "init %dx%d -> %d maxh %d", sw, sh, dw, maxh);
    if (fails) return;
    size_t bpp = f == VIDEO_SCALE_XRGB8888 ? 4 : 2;
    size_t pitch = (size_t)sw * bpp + (odd ? 3 : 0);
    size_t stride = (size_t)dw + (odd ? 3 : 0);
    uint8_t *srcmem = malloc((size_t)sh * pitch + 3), *dstmem = malloc((size_t)c.game_h * stride * 2 + 16);
    CHECK(srcmem && dstmem, "allocation"); if (!srcmem || !dstmem) { free(srcmem); free(dstmem); return; }
    uint8_t *src = srcmem + (odd ? 1 : 0), *dst = dstmem + 3;
    memset(srcmem, 0xa5, (size_t)sh * pitch + 3);
    memset(dstmem, 0x6d, (size_t)c.game_h * stride * 2 + 16);
    for (int y=0;y<sh;y++) for (int x=0;x<sw;x++) {
        if (f == VIDEO_SCALE_XRGB8888) {
            uint32_t p = 0xff000000u | ((uint32_t)((x*17+y*9)&255)<<16) | ((uint32_t)((x*7+y*13)&255)<<8) | (uint32_t)((x*3+y*19)&255);
            memcpy(src + (size_t)y*pitch + (size_t)x*4, &p, 4);
        } else {
            uint16_t p = (uint16_t)((x*257u + y*733u) ^ (f == VIDEO_SCALE_RGB1555 ? 0x8000u : 0u));
            memcpy(src + (size_t)y*pitch + (size_t)x*2, &p, 2);
        }
    }
    CHECK(video_scale_render(&c, src, pitch, f, dst, stride), "render %dx%d format %d", sw,sh,f);
    for (int y=0;y<c.game_h;y++) for (int x=0;x<dw;x++) {
        uint16_t got; memcpy(&got,dst+((size_t)y*stride+x)*2,2);
        uint16_t want = x >= c.draw_x0 && x < c.draw_x0+c.draw_w ?
            refpixel(src+(size_t)c.ymap[y]*pitch,c.xmap[x-c.draw_x0],f) : 0;
        CHECK(got==want,"pixel mismatch %dx%d->%dx%d f%d @%d,%d got %04x want %04x",sw,sh,dw,c.game_h,f,x,y,got,want);
    }
    for (int y=0;y<c.game_h;y++) for (size_t x=(size_t)dw*2;x<stride*2;x++)
        CHECK(dst[(size_t)y*stride*2+x]==0x6d,"stride padding modified");
    for (size_t x=0;x<3;x++) CHECK(dstmem[x]==0x6d,"leading guard modified");
    for (size_t x=3+(size_t)c.game_h*stride*2;x<(size_t)c.game_h*stride*2+16;x++)
        CHECK(dstmem[x]==0x6d,"trailing guard modified");
    free(srcmem); free(dstmem);
}

int main(void) {
    static const int widths[] = {2,3,4};
    for (int k=0;k<3;k++) for (int f=0;f<3;f++) {
        int mul=widths[k]; one_case(32,18,32*mul,18*mul,(video_scale_format_t)f,k&1);
    }
    one_case(320,240,480,800,VIDEO_SCALE_RGB565,0);
    one_case(160,144,480,800,VIDEO_SCALE_RGB565,1);
    one_case(13,7,26,28,VIDEO_SCALE_RGB565,1);  /* paired integer path with odd tail */
    one_case(13,7,39,28,VIDEO_SCALE_RGB565,1);
    one_case(13,7,52,28,VIDEO_SCALE_RGB565,1);
    one_case(256,192,480,120,VIDEO_SCALE_XRGB8888,1); /* capped and centered */
    one_case(13,11,19,17,VIDEO_SCALE_RGB1555,1); /* odd dimensions/pitch */
    video_scale_t c;
    CHECK(!video_scale_init(&c,0,1,480,400),"zero source accepted");
    CHECK(!video_scale_init(&c,1,1,VIDEO_SCALE_MAX_WIDTH+1,400),"oversized target accepted");
    if (fails) return 1;
    puts("video scaler tests passed"); return 0;
}
