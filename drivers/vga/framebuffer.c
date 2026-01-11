/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include "framebuffer.h"
#include <stddef.h>
#include <stdint.h>
#include "../../lib/flopmath.h"
#include "../../lib/str.h"
#include "../../mem/utils.h"
#include "../../multiboot/multiboot.h"
#include "../../flanterm/src/flanterm.h"
#include "../../flanterm/src/flanterm_backends/fb.h"

#include "vgahandler.h"

colors_t c;

typedef struct {
    uint32_t width;
    uint32_t height;
    void* screen;
    uint32_t* buffer;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t type;
    struct multiboot_color* palette;
    uint32_t palette_num_colors;
    uint32_t blue_mask_size;
    uint32_t blue_field_position;
    uint32_t red_mask_size;
    uint32_t red_field_position;
    uint32_t green_mask_size;
    uint32_t green_field_position;
} framebuffer_t;

framebuffer_t _fb_instance;

void init_colors() {
    c.black = 0x000000;
    c.white = 0xFFFFFF;
    c.red = 0xFF0000;
    c.green = 0x00FF00;
    c.blue = 0x0000FF;
    c.yellow = 0xFFFF00;
    c.cyan = 0x00FFFF;
    c.magenta = 0xFF00FF;
    c.gray = 0x808080;
    c.light_gray = 0xD3D3D3;
    c.dark_gray = 0xA9A9A9;
    c.brown = 0xA52A2A;
}

void framebuffer_set_pixel_buffer(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t) x >= _fb_instance.width || (uint32_t) y >= _fb_instance.height) {
        return;
    }

    uint8_t* fb = _fb_instance.screen;

    switch (_fb_instance.bpp) {
        case 8:
            FB_PUT_PIXEL_8(fb, x, y, color);
            break;
        case 15:
            FB_PUT_PIXEL_15(fb, x, y, color);
            break;
        case 16:
            FB_PUT_PIXEL_16(fb, x, y, color);
            break;
        case 24:
            FB_PUT_PIXEL_24(fb, x, y, color);
            break;
        case 32:
            FB_PUT_PIXEL_32(fb, x, y, color);
            break;
    }
}

uint32_t framebuffer_get_pixel_buffer(int x, int y) {
    if (x < 0 || y < 0 || (uint32_t) x >= _fb_instance.width || (uint32_t) y >= _fb_instance.height) {
        return 0;
    }

    uint8_t* fb = _fb_instance.screen;

    switch (_fb_instance.bpp) {
        case 8:
            return FB_GET_PIXEL_8(fb, x, y);
        case 15:
            return FB_GET_PIXEL_15(fb, x, y);
        case 16:
            return FB_GET_PIXEL_16(fb, x, y);
        case 24:
            return FB_GET_PIXEL_24(fb, x, y);
        case 32:
            return FB_GET_PIXEL_32(fb, x, y);
        default:
            return 0;
    }
}

void framebuffer_init(multiboot_info_t* mbi) {
    init_colors();
    _fb_instance.width = mbi->framebuffer_width;
    _fb_instance.height = mbi->framebuffer_height;
    _fb_instance.pitch = mbi->framebuffer_pitch;
    _fb_instance.bpp = mbi->framebuffer_bpp;
    _fb_instance.type = mbi->framebuffer_type;
    _fb_instance.screen = (void*) (uintptr_t) mbi->framebuffer_addr;
    _fb_instance.palette = (struct multiboot_color*) mbi->framebuffer_palette_addr;
    _fb_instance.palette_num_colors = mbi->framebuffer_palette_num_colors;
    _fb_instance.blue_mask_size = mbi->framebuffer_blue_mask_size;
    _fb_instance.blue_field_position = mbi->framebuffer_blue_field_position;
    _fb_instance.red_mask_size = mbi->framebuffer_red_mask_size;
    _fb_instance.red_field_position = mbi->framebuffer_red_field_position;
    _fb_instance.green_mask_size = mbi->framebuffer_green_mask_size;
    _fb_instance.green_field_position = mbi->framebuffer_green_field_position;

    uint32_t color = 0;
    for (uint32_t y = 0; y < _fb_instance.height; y++) {
        for (uint32_t x = 0; x < _fb_instance.width; x++) {
            framebuffer_set_pixel_buffer(x, y, color);
        }
    }
}

void framebuffer_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || (uint32_t) x >= _fb_instance.width || y < 0 || (uint32_t) y >= _fb_instance.height) {
        return;
    }
    framebuffer_set_pixel_buffer(x, y, color);
}

void framebuffer_draw_line(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    int e2;

    while (1) {
        framebuffer_put_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
        e2 = err;
        if (e2 > -dx) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dy) {
            err += dx;
            y1 += sy;
        }
    }
}

void framebuffer_draw_rectangle(int x, int y, int width, int height, uint32_t color) {
    for (int i = 0; i < width; i++) {
        for (int j = 0; j < height; j++) {
            framebuffer_put_pixel(x + i, y + j, color);
        }
    }
}

void framebuffer_fill_screen(uint32_t color) {
    for (int i = 0; i < _fb_instance.width; i++) {
        for (int j = 0; j < _fb_instance.height; j++) {
            framebuffer_put_pixel(i, j, color);
        }
    }
}

void framebuffer_draw_circle(int x_center, int y_center, int radius, uint32_t color) {
    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        framebuffer_put_pixel(x_center + x, y_center + y, color);
        framebuffer_put_pixel(x_center + y, y_center + x, color);
        framebuffer_put_pixel(x_center - y, y_center + x, color);
        framebuffer_put_pixel(x_center - x, y_center + y, color);
        framebuffer_put_pixel(x_center - x, y_center - y, color);
        framebuffer_put_pixel(x_center - y, y_center - x, color);
        framebuffer_put_pixel(x_center + y, y_center - x, color);
        framebuffer_put_pixel(x_center + x, y_center - y, color);

        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

void framebuffer_draw_triangle(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color) {
    framebuffer_draw_line(x1, y1, x2, y2, color);
    framebuffer_draw_line(x2, y2, x3, y3, color);
    framebuffer_draw_line(x3, y3, x1, y1, color);
}

#include <stdbool.h>

void framebuffer_test_rectangle() {
    framebuffer_draw_rectangle(100, 104, 507, 30, c.red);
    framebuffer_draw_rectangle(80, 20, 90, 20, c.green);
    framebuffer_draw_rectangle(30, 30, 41, 10, c.blue);
}

void framebuffer_test_circle() {
    framebuffer_draw_circle(100, 100, 50, c.red);
    framebuffer_draw_circle(200, 200, 40, c.green);
    framebuffer_draw_circle(300, 300, 30, c.blue);
}

void framebuffer_test_triangle() {
    framebuffer_draw_triangle(100, 100, 200, 200, 300, 100, c.red);
    framebuffer_draw_triangle(200, 200, 300, 300, 400, 200, c.green);
    framebuffer_draw_triangle(300, 300, 400, 400, 500, 300, c.blue);
}

void framebuffer_test_pattern() {
    int checker_size = 100; // Size of each checker square

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            uint32_t color = ((row + col) % 2 == 0) ? c.white : c.brown;
            int x = col * checker_size;
            int y = row * checker_size;
            framebuffer_draw_rectangle(x, y, checker_size, checker_size, color);
        }
    }
}

struct flanterm_context* ft_ctx;

void framebuffer_term_init() {
    ft_ctx = flanterm_fb_init(NULL,
                              NULL,
                              _fb_instance.screen,
                              _fb_instance.width,
                              _fb_instance.height,
                              _fb_instance.pitch,
                              _fb_instance.red_mask_size,
                              _fb_instance.red_field_position,
                              _fb_instance.green_mask_size,
                              _fb_instance.green_field_position,
                              _fb_instance.blue_mask_size,
                              _fb_instance.blue_field_position,
                              NULL, // bullshit
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              0,
                              0,
                              1,
                              0,
                              0,
                              0);
}

void framebuffer_term_write(const char* str) {
    flanterm_write(ft_ctx, str, flopstrlen(str));
}
