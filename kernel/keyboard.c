#include "keyboard.h"
#include "io.h"
#include "idt.h"

#define KBD_DATA_PORT 0x60

/* Left/right shift make and break codes (scancode set 1). */
#define SC_LSHIFT_MAKE  0x2A
#define SC_RSHIFT_MAKE  0x36
#define SC_LSHIFT_BREAK 0xAA
#define SC_RSHIFT_BREAK 0xB6

/* A simple ring buffer of translated ASCII characters. The IRQ handler
 * (producer) and keyboard_getchar() (consumer) share this; since we're
 * single-core with no scheduler yet, plain volatile is enough. */
#define KBD_BUFFER_SIZE 256
static volatile char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static int shift_held = 0;

/* Scancode set 1, unshifted US QWERTY. Index = make code. 0 = no mapping
 * (function keys, ctrl/alt, arrows, etc. - not handled yet). */
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

static void keyboard_handler(registers_t* regs) {
    (void)regs;
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == SC_LSHIFT_MAKE || scancode == SC_RSHIFT_MAKE) {
        shift_held = 1;
        return;
    }
    if (scancode == SC_LSHIFT_BREAK || scancode == SC_RSHIFT_BREAK) {
        shift_held = 0;
        return;
    }

    /* Bit 7 set means a key release; we only translate presses. */
    if (scancode & 0x80) return;

    if (scancode < 128) {
        char c = shift_held ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
        if (c) kbd_buffer_push(c);
    }
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
