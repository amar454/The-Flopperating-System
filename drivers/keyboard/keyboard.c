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
#include "../../task/sched.h"
#include <stdint.h>

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;

static const char kbd_map_normal[] = {
    0,   27,  '1', '2', '3',  '4', '5', '6',  '7', '8', '9',  '0', '-', '=', '\b', '\t', 'q', 'w',
    'e', 'r', 't', 'y', 'u',  'i', 'o', 'p',  '[', ']', '\n', 0,   'a', 's', 'd',  'f',  'g', 'h',
    'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c',  'v', 'b', 'n', 'm',  ',',  '.', '/',
    0,   '*', 0,   ' ', 0,    0,   0,   0,    0,   0,   0,    0,   0,   0,   0,    0,    0,   0,
    0,   0,   '-', 0,   0,    0,   '+', 0,    0,   0,   0,    0,   0,   0,   0,    0,    0,   0,
};

static const char kbd_map_shifted[] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&',  '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T',  'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G',  'H',  'J', 'K', 'L', ':',
    '\"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B',  'N', 'M', '<', '>', '?', 0,    '*',  0,   ' ',
};

keyboard_t keyboard;

const char* key_to_char(unsigned char key) {
    switch (key) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            shift_pressed = 1;
            return "";
        case KEY_LSHIFT_RELEASE:
        case KEY_RSHIFT_RELEASE:
            shift_pressed = 0;
            return "";
        case KEY_CTRL:
            ctrl_pressed = 1;
            return "";
        case KEY_CTRL_RELEASE:
            ctrl_pressed = 0;
            return "";
        case KEY_ALT:
            alt_pressed = 1;
            return "";
        case KEY_ALT_RELEASE:
            alt_pressed = 0;
            return "";
    }

    switch (key) {
        case KEY_ESC:
            return "Esc";
        case KEY_F1:
            return "F1";
        case KEY_F2:
            return "F2";
        case KEY_F3:
            return "F3";
        case KEY_F4:
            return "F4";
        case KEY_F5:
            return "F5";
        case KEY_F6:
            return "F6";
        case KEY_F7:
            return "F7";
        case KEY_F8:
            return "F8";
        case KEY_F9:
            return "F9";
        case KEY_F10:
            return "F10";
        case KEY_F11:
            return "F11";
        case KEY_F12:
            return "F12";
        case KEY_ARROW_UP:
            return "ArrowUp";
        case KEY_ARROW_LEFT:
            return "ArrowLeft";
        case KEY_ARROW_RIGHT:
            return "ArrowRight";
        case KEY_ARROW_DOWN:
            return "ArrowDown";
        case KEY_DELETE:
            return "Delete";
    }

    if (key < sizeof(kbd_map_normal)) {
        static char buf[2] = {0, 0};
        char c = shift_pressed ? kbd_map_shifted[key] : kbd_map_normal[key];

        if (c != 0) {
            buf[0] = c;
            return buf;
        }
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

void keyboard_handler() {
    unsigned char scancode = inb(0x60);
    const char* str = key_to_char(scancode);

    if (str[0] == '\0') {
        return;
    }
    if (str[0] == '\b') {
        echo("\b", BLACK);
    } else {
        echo((char*) str, WHITE);
    }
}

void keyboard_init() {
    keyboard.layout = QWERTY;
    keyboard.kbd_map_normal = kbd_map_normal;
    keyboard.kbd_map_shifted = kbd_map_shifted;
    log("keyboard: layout set to qwerty\n", GREEN);
    log("keyboard: init - ok\n", GREEN);
}
