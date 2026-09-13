#define COM1 0x3F8

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void serial_puts(const char *s) {
    while (*s) {
        outb(COM1, (unsigned char)*s++);
    }
}

/*
 * _start est le point d'entrée.
 * On configure la pile explicitement puis on appelle le code C.
 */
__attribute__((naked)) void _start(void) {
    __asm__ volatile (
        "movl $0x200000, %esp\n"   /* stack à 2 Mo */
        "call _main\n"
        "hlt\n"
    );
}

void _main(void) {
    serial_puts("Hello from VM!\n");
    serial_puts("MiniOS running in protected mode.\n");
}
