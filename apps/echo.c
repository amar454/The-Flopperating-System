#include "echo.h"
#include "../drivers/vga/vgahandler.h"
#include "../drivers/vga/framebuffer.h"
#include "../flanterm/src/flanterm.h"
#include "../flanterm/src/flanterm_backends/fb.h"
#include "../lib/str.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#define VGA_ADDRESS 0xB8000

void get_terminal_content(char* buffer, size_t buffer_size) {
    unsigned int index = 0;

    if (buffer_size < VGA_WIDTH * VGA_HEIGHT + 1) {
        return;
    }

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        unsigned short entry = terminal_buffer[i];
        char character = entry & 0xFF;

        buffer[index++] = character;
    }

    buffer[index] = '\0';
}

void put_char(char c, unsigned char color) {
    if (c == '\n') {
        vga_index += VGA_WIDTH - (vga_index % VGA_WIDTH);
    } else {
        terminal_buffer[vga_index++] = (color << 8) | (unsigned char) c;
    }

    if (vga_index >= VGA_WIDTH * VGA_HEIGHT) {
        for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++) {
            terminal_buffer[i] = terminal_buffer[i + VGA_WIDTH];
        }

        for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++) {
            terminal_buffer[i] = (color << 8) | ' ';
        }
        vga_index -= VGA_WIDTH;
    }
}

void echo(const char* str, unsigned char color) {
    unsigned char ansi_color;
    switch (color) {
        case BLACK:
            ansi_color = 0;
            break;
        case BLUE:
            ansi_color = 4;
            break;
        case GREEN:
            ansi_color = 2;
            break;
        case CYAN:
            ansi_color = 6;
            break;
        case RED:
            ansi_color = 1;
            break;
        case MAGENTA:
            ansi_color = 5;
            break;
        case BROWN:
            ansi_color = 3;
            break;
        case LIGHT_GRAY:
            ansi_color = 7;
            break;
        case DARK_GRAY:
            ansi_color = 8;
            break;
        case LIGHT_BLUE:
            ansi_color = 12;
            break;
        case LIGHT_GREEN:
            ansi_color = 10;
            break;
        case LIGHT_CYAN:
            ansi_color = 14;
            break;
        case LIGHT_RED:
            ansi_color = 9;
            break;
        case LIGHT_MAGENTA:
            ansi_color = 13;
            break;
        case WHITE:
            ansi_color = 15;
            break;
        case YELLOW:
            ansi_color = 3;
            break;
        default:
            ansi_color = 7;
            break;
    }

    char buf[8192];
    int len = flopsnprintf(buf, sizeof(buf), "\x1b[38;5;%um%s\x1b[0m", ansi_color, str);
    if (len > 0 && len < (int) sizeof(buf)) {
        console_write(buf);
    }
}

void echo_bold(const char* str, unsigned char color) {
    color = color | 0x08;

    while (*str) {
        put_char(*str++, color);
    }
}

void echo_f(const char* format, int color, ...) {
    char buffer[256];
    va_list args;

    va_start(args, color);

    int result = flopsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    if (result < 0) {
        buffer[0] = '\0';
    } else if ((size_t) result >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0';
    }

    echo(buffer, color);
}

void retrieve_terminal_buffer(char* buffer, uint8_t* colors) {
    const unsigned short* terminal_buffer = (unsigned short*) VGA_ADDRESS;

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        unsigned short entry = terminal_buffer[i];

        buffer[i] = (char) (entry & 0xFF);

        colors[i] = (uint8_t) ((entry >> 8) & 0xFF);
    }
}

void restore_terminal_buffer(const char* buffer, const uint8_t* colors) {
    unsigned short* terminal_buffer = (unsigned short*) VGA_ADDRESS;

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        if (buffer[i] == '\0') {
            terminal_buffer[i] = (colors[i] << 8) | ' ';
        } else {
            terminal_buffer[i] = (colors[i] << 8) | buffer[i];
        }
    }
}
