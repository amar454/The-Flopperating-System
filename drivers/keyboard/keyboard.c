/*

keyboard.c - keyboard driver for FloppaOS. It uses the io library to read and scan for keyboard interrupts, with tracking for shift to allow capital letters and additional symbols.

Copyright 2024 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https:

*/

#include "keyboard.h"
#include "../../apps/echo.h"
#include "../vga/vgahandler.h"
#include "../io/io.h"
#include <stdint.h>

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;

int command_ready = 0;

const char* key_to_char(unsigned char key) {
    switch (key) {
        case 0x2A:
        case 0x36:
            shift_pressed = 1;
            return "";
        case 0xAA:
        case 0xB6:
            shift_pressed = 0;
            return "";
        case 0x1D:
            ctrl_pressed = 1;
            return "";
        case 0x9D:
            ctrl_pressed = 0;
            return "";
        case 0x38:
            alt_pressed = 1;
            return "";
        case 0xB8:
            alt_pressed = 0;
            return "";
    }

    if (key >= 0x02 && key <= 0x39) {
        const char* c = "";
        switch (key) {
            case 0x02:
                c = shift_pressed ? "!" : "1";
                break;
            case 0x03:
                c = shift_pressed ? "@" : "2";
                break;
            case 0x04:
                c = shift_pressed ? "#" : "3";
                break;
            case 0x05:
                c = shift_pressed ? "$" : "4";
                break;
            case 0x06:
                c = shift_pressed ? "%" : "5";
                break;
            case 0x07:
                c = shift_pressed ? "^" : "6";
                break;
            case 0x08:
                c = shift_pressed ? "&" : "7";
                break;
            case 0x09:
                c = shift_pressed ? "*" : "8";
                break;
            case 0x0A:
                c = shift_pressed ? "(" : "9";
                break;
            case 0x0B:
                c = shift_pressed ? ")" : "0";
                break;
            case 0x0C:
                c = shift_pressed ? "_" : "-";
                break;
            case 0x0D:
                c = shift_pressed ? "+" : "=";
                break;

            case 0x10:
                c = shift_pressed ? "Q" : "q";
                break;
            case 0x11:
                c = shift_pressed ? "W" : "w";
                break;
            case 0x12:
                c = shift_pressed ? "E" : "e";
                break;
            case 0x13:
                c = shift_pressed ? "R" : "r";
                break;
            case 0x14:
                c = shift_pressed ? "T" : "t";
                break;
            case 0x15:
                c = shift_pressed ? "Y" : "y";
                break;
            case 0x16:
                c = shift_pressed ? "U" : "u";
                break;
            case 0x17:
                c = shift_pressed ? "I" : "i";
                break;
            case 0x18:
                c = shift_pressed ? "O" : "o";
                break;
            case 0x19:
                c = shift_pressed ? "P" : "p";
                break;
            case 0x1E:
                c = shift_pressed ? "A" : "a";
                break;
            case 0x1F:
                c = shift_pressed ? "S" : "s";
                break;
            case 0x20:
                c = shift_pressed ? "D" : "d";
                break;
            case 0x21:
                c = shift_pressed ? "F" : "f";
                break;
            case 0x22:
                c = shift_pressed ? "G" : "g";
                break;
            case 0x23:
                c = shift_pressed ? "H" : "h";
                break;
            case 0x24:
                c = shift_pressed ? "J" : "j";
                break;
            case 0x25:
                c = shift_pressed ? "K" : "k";
                break;
            case 0x26:
                c = shift_pressed ? "L" : "l";
                break;
            case 0x2C:
                c = shift_pressed ? "Z" : "z";
                break;
            case 0x2D:
                c = shift_pressed ? "X" : "x";
                break;
            case 0x2E:
                c = shift_pressed ? "C" : "c";
                break;
            case 0x2F:
                c = shift_pressed ? "V" : "v";
                break;
            case 0x30:
                c = shift_pressed ? "B" : "b";
                break;
            case 0x31:
                c = shift_pressed ? "N" : "n";
                break;
            case 0x32:
                c = shift_pressed ? "M" : "m";
                break;

            case 0x39:
                c = " ";
                break;
            case 0x0E:
                c = "\b";
                break;
            case 0x1C:
                c = "\n";
                break;
        }
        return c;
    }

    switch (key) {
        case 0x01:
            return "Esc";
        case 0x3B:
            return "F1";
        case 0x3C:
            return "F2";
        case 0x3D:
            return "F3";
        case 0x3E:
            return "F4";
        case 0x3F:
            return "F5";
        case 0x40:
            return "F6";
        case 0x41:
            return "F7";
        case 0x42:
            return "F8";
        case 0x43:
            return "F9";
        case 0x44:
            return "F10";
        case 0x57:
            return "F11";
        case 0x58:
            return "F12";
        case 0x0F:
            return "Tab";
        case 0x35:
            return "Insert";
        case 0x49:
            return "Home";
        case 0x4B:
            return "PageUp";
        case 0x4D:
            return "Delete";
        case 0x4F:
            return "End";
        case 0x51:
            return "PageDown";
        case 0x52:
            return "ArrowUp";
        case 0x53:
            return "ArrowLeft";
        case 0x54:
            return "ArrowRight";
        case 0x55:
            return "ArrowDown";
    }

    return "";
}

unsigned char try_read_key(void) {
    if (inb(0x64) & 0x1) {
        return inb(0x60);
    }
    return 0;
}

char try_get_char(void) {
    unsigned char scancode = try_read_key();
    if (scancode != 0) {
        return *key_to_char(scancode);
    }
    return 0;
}

void keyboard_task(void* arg) {
    static int pos = 0;
    char c = try_get_char();

    if (c == 0) {
        return;
    }

    if (c == '\b' && pos > 0) {
        pos--;
        vga_index--;
        echo(" ", BLACK);
        vga_index--;
        uint16_t x = vga_index % VGA_WIDTH;
        uint16_t y = vga_index / VGA_WIDTH;
        vga_set_foreground_color(WHITE, vga_index / VGA_WIDTH, WHITE);
        vga_set_cursor_position(x, y);

    } else if (c == '\n') {
        echo("\n", WHITE);
        command_ready = 1;
        pos = 0;
        vga_set_foreground_color(WHITE, vga_index / VGA_WIDTH, WHITE);
        vga_set_cursor_position(0, (vga_index / VGA_WIDTH));

    } else if (c >= 32 && c <= 126 && pos < 40 - 1) {
        put_char(c, WHITE);
        uint16_t x = vga_index % VGA_WIDTH;
        uint16_t y = vga_index / VGA_WIDTH;
        vga_set_foreground_color(WHITE, vga_index / VGA_WIDTH, WHITE);
        vga_set_cursor_position(x, y);
    }
}
