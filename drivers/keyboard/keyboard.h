#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdbool.h>

typedef enum scancodes {
    KEY_ESC = 0x01,
    KEY_CTRL = 0x1D,
    KEY_CTRL_RELEASE = 0x9D,
    KEY_ALT = 0x38,
    KEY_ALT_RELEASE = 0xB8,
    KEY_LSHIFT = 0x2A,
    KEY_RSHIFT = 0x36,
    KEY_LSHIFT_RELEASE = 0xAA,
    KEY_RSHIFT_RELEASE = 0xB6,

    KEY_F1 = 0x3B,
    KEY_F2 = 0x3C,
    KEY_F3 = 0x3D,
    KEY_F4 = 0x3E,
    KEY_F5 = 0x3F,
    KEY_F6 = 0x40,
    KEY_F7 = 0x41,
    KEY_F8 = 0x42,
    KEY_F9 = 0x43,
    KEY_F10 = 0x44,
    KEY_F11 = 0x57,
    KEY_F12 = 0x58,

    KEY_ARROW_UP = 0x52,
    KEY_ARROW_DOWN = 0x55,
    KEY_ARROW_LEFT = 0x53,
    KEY_ARROW_RIGHT = 0x54,
    KEY_DELETE = 0x4D,
    KEY_HOME = 0x49,
    KEY_END = 0x4F,
    KEY_PAGE_UP = 0x4B,
    KEY_PAGE_DOWN = 0x51
} scancodes_t;

void keyboard_handler();
const char* key_to_char(unsigned char key);
unsigned char try_read_key(void);
char get_char(void);
char try_get_char(void);
#endif // KEYBOARD_H
