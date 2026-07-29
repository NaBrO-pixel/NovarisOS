#include "keyboard.h"
#include "io.h"
#include "idt.h"

#define KBD_DATA_PORT 0x60

/* Left/right shift make and break codes (scancode set 1). */
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_CTRL    0x1D
#define SC_ALT     0x38
#define SC_CAPS    0x3A
#define SC_EXTENDED 0xE0

/* A simple ring buffer of translated ASCII characters. The IRQ handler
 * (producer) and keyboard_getchar() (consumer) share this; since we're
 * single-core with no scheduler yet, plain volatile is enough. */
#define KBD_BUFFER_SIZE 256
static volatile char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

/* The richer event queue the desktop reads (see keyboard.h). */
#define KEY_QUEUE_SIZE 64
static volatile key_event_t key_queue[KEY_QUEUE_SIZE];
static volatile uint32_t key_head = 0;
static volatile uint32_t key_tail = 0;

static volatile uint8_t mods = 0;
static int extended = 0;

/* Scancode set 1, unshifted US QWERTY. Index = make code. 0 = no mapping
 * (function keys, ctrl/alt, arrows, etc. - handled separately below). */
static const char scancode_ascii[128] = {
    0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
    /* remaining entries default to 0 */
};

static const char scancode_ascii_shift[128] = {
    0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ',
};

static void kbd_buffer_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) { /* silently drop if the buffer is full */
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

static void key_queue_push(uint16_t code, uint8_t ascii, uint8_t pressed) {
    uint32_t next = (key_head + 1) % KEY_QUEUE_SIZE;
    if (next == key_tail) return;
    key_queue[key_head].code = code;
    key_queue[key_head].ascii = ascii;
    key_queue[key_head].mods = mods;
    key_queue[key_head].pressed = pressed;
    key_head = next;
}

/* Maps the scancodes that have no ASCII to the KEY_* codes in keyboard.h.
 * `ext` distinguishes the E0-prefixed duplicates (the arrow cluster and
 * the right-hand modifiers) from the numeric keypad. */
static uint16_t special_key(uint8_t sc, int ext) {
    if (ext) {
        switch (sc) {
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            case 0x49: return KEY_PAGE_UP;
            case 0x51: return KEY_PAGE_DOWN;
            case 0x52: return KEY_INSERT;
            case 0x53: return KEY_DELETE;
            case 0x1D: return KEY_RCTRL;
            case 0x38: return KEY_RALT;
            case 0x5B: case 0x5C: return KEY_SUPER;
            default: return 0;
        }
    }
    switch (sc) {
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        case SC_LSHIFT: return KEY_LSHIFT;
        case SC_RSHIFT: return KEY_RSHIFT;
        case SC_CTRL:   return KEY_LCTRL;
        case SC_ALT:    return KEY_LALT;
        case SC_CAPS:   return KEY_CAPSLOCK;
        default: return 0;
    }
}

static void keyboard_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == SC_EXTENDED) {
        extended = 1;
        return;
    }
    int ext = extended;
    extended = 0;

    int pressed = !(scancode & 0x80);
    uint8_t sc = scancode & 0x7F;

    /* Modifiers update the shared state before anything is queued, so an
     * event always carries the modifier set that was live when it fired. */
    uint8_t bit = 0;
    if (!ext && (sc == SC_LSHIFT || sc == SC_RSHIFT)) bit = KEY_MOD_SHIFT;
    else if (sc == SC_CTRL) bit = KEY_MOD_CTRL;
    else if (sc == SC_ALT) bit = KEY_MOD_ALT;
    else if (ext && (sc == 0x5B || sc == 0x5C)) bit = KEY_MOD_SUPER;

    if (bit) {
        if (pressed) mods |= bit;
        else mods &= (uint8_t)~bit;
    } else if (sc == SC_CAPS && pressed) {
        mods ^= KEY_MOD_CAPS;
    }

    uint16_t special = special_key(sc, ext);
    if (special) {
        key_queue_push(special, 0, (uint8_t)pressed);
        return;
    }

    if (sc >= 128) return;
    char c = (mods & KEY_MOD_SHIFT) ? scancode_ascii_shift[sc] : scancode_ascii[sc];
    if ((mods & KEY_MOD_CAPS) && c >= 'a' && c <= 'z') c = (char)(c - 32);
    else if ((mods & KEY_MOD_CAPS) && (mods & KEY_MOD_SHIFT) && c >= 'A' && c <= 'Z') {
        c = (char)(c + 32);
    }
    if (!c) return;

    key_queue_push((uint16_t)(unsigned char)c, (uint8_t)c, (uint8_t)pressed);
    if (pressed) kbd_buffer_push(c);
}

void keyboard_install(void) {
    register_interrupt_handler(33, keyboard_handler); /* IRQ1 -> remapped vector 33 */
}

int keyboard_haschar(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    while (kbd_head == kbd_tail) {
        __asm__ __volatile__("hlt");
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

int keyboard_poll_event(key_event_t* out) {
    if (key_head == key_tail) return 0;
    key_event_t ev = key_queue[key_tail];
    key_tail = (key_tail + 1) % KEY_QUEUE_SIZE;
    if (out) *out = ev;
    return 1;
}

uint8_t keyboard_mods(void) { return mods; }
