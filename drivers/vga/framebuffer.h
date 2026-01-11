#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include "../../multiboot/multiboot.h"
#include "vgahandler.h"
#define FB_PUT_PIXEL_8(fb, x, y, color)                                                                                \
    do {                                                                                                               \
        uint8_t* p = (uint8_t*) (fb) + _fb_instance.pitch * (y) + (x);                                                 \
        *p = (uint8_t) ((color) & 0xFF);                                                                               \
    } while (0)

#define FB_PUT_PIXEL_15(fb, x, y, color)                                                                               \
    do {                                                                                                               \
        uint16_t* p = (uint16_t*) ((fb) + _fb_instance.pitch * (y) + 2 * (x));                                         \
        uint16_t r = ((color) >> 16) & 0x1F;                                                                           \
        uint16_t g = ((color) >> 8) & 0x1F;                                                                            \
        uint16_t b = ((color) >> 0) & 0x1F;                                                                            \
        *p = (r << 10) | (g << 5) | b;                                                                                 \
    } while (0)

#define FB_PUT_PIXEL_16(fb, x, y, color)                                                                               \
    do {                                                                                                               \
        uint16_t* p = (uint16_t*) ((fb) + _fb_instance.pitch * (y) + 2 * (x));                                         \
        uint16_t r = ((color) >> 16) & 0x1F;                                                                           \
        uint16_t g = ((color) >> 8) & 0x3F;                                                                            \
        uint16_t b = ((color) >> 0) & 0x1F;                                                                            \
        *p = (r << 11) | (g << 5) | b;                                                                                 \
    } while (0)

#define FB_PUT_PIXEL_24(fb, x, y, color)                                                                               \
    do {                                                                                                               \
        uint8_t* p = (uint8_t*) (fb) + _fb_instance.pitch * (y) + 3 * (x);                                             \
        p[0] = ((color) >> 0) & 0xFF;  /* B */                                                                         \
        p[1] = ((color) >> 8) & 0xFF;  /* G */                                                                         \
        p[2] = ((color) >> 16) & 0xFF; /* R */                                                                         \
    } while (0)

#define FB_PUT_PIXEL_32(fb, x, y, color)                                                                               \
    do {                                                                                                               \
        uint32_t* p = (uint32_t*) ((fb) + _fb_instance.pitch * (y) + 4 * (x));                                         \
        *p = (uint32_t) (color);                                                                                       \
    } while (0)
#define FB_GET_PIXEL_8(fb, x, y)                                                                                       \
    ({                                                                                                                 \
        uint8_t* p = (uint8_t*) (fb) + _fb_instance.pitch * (y) + (x);                                                 \
        (uint32_t) (*p);                                                                                               \
    })

#define FB_GET_PIXEL_15(fb, x, y)                                                                                      \
    ({                                                                                                                 \
        uint16_t* p = (uint16_t*) ((fb) + _fb_instance.pitch * (y) + 2 * (x));                                         \
        uint16_t v = *p;                                                                                               \
        uint32_t r = (v >> 10) & 0x1F;                                                                                 \
        uint32_t g = (v >> 5) & 0x1F;                                                                                  \
        uint32_t b = (v >> 0) & 0x1F;                                                                                  \
        (r << 16) | (g << 8) | b;                                                                                      \
    })

#define FB_GET_PIXEL_16(fb, x, y)                                                                                      \
    ({                                                                                                                 \
        uint16_t* p = (uint16_t*) ((fb) + _fb_instance.pitch * (y) + 2 * (x));                                         \
        uint16_t v = *p;                                                                                               \
        uint32_t r = (v >> 11) & 0x1F;                                                                                 \
        uint32_t g = (v >> 5) & 0x3F;                                                                                  \
        uint32_t b = (v >> 0) & 0x1F;                                                                                  \
        (r << 16) | (g << 8) | b;                                                                                      \
    })

#define FB_GET_PIXEL_24(fb, x, y)                                                                                      \
    ({                                                                                                                 \
        uint8_t* p = (uint8_t*) (fb) + _fb_instance.pitch * (y) + 3 * (x);                                             \
        (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);                                            \
    })

#define FB_GET_PIXEL_32(fb, x, y)                                                                                      \
    ({                                                                                                                 \
        uint32_t* p = (uint32_t*) ((fb) + _fb_instance.pitch * (y) + 4 * (x));                                         \
        *p;                                                                                                            \
    })

typedef struct colors {
    uint32_t black;
    uint32_t white;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t yellow;
    uint32_t cyan;
    uint32_t magenta;
    uint32_t gray;
    uint32_t light_gray;
    uint32_t dark_gray;
    uint32_t brown;

} colors_t;

extern colors_t c;
void init_colors(void);
void framebuffer_init(multiboot_info_t* mbi);
void framebuffer_put_pixel(int x, int y, uint32_t color);
void framebuffer_draw_line(int x1, int y1, int x2, int y2, uint32_t color);
void framebuffer_draw_rectangle(int x, int y, int width, int height, uint32_t color);
void framebuffer_fill_screen(uint32_t color);
void framebuffer_test_triangle();
void framebuffer_test_rectangle();
void framebuffer_test_circle();
void framebuffer_test_pattern();

void framebuffer_draw_char(int x, int y, char c, uint32_t color);
void framebuffer_print_string(int x, int y, const char* str, uint32_t color);

void framebuffer_term_init();
void framebuffer_term_write(const char* str);
#endif // FRAMEBUFFER_H
