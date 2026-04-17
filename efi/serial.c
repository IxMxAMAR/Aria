// efi/serial.c — COM1 serial output
// Works from EFI and bare-metal VMM — no OS dependencies
#include "serial.h"

extern unsigned char __inbyte(unsigned short port);
extern void __outbyte(unsigned short port, unsigned char val);

void serial_init(void) {
    __outbyte(COM1_PORT + 1, 0x00);
    __outbyte(COM1_PORT + 3, 0x80);
    __outbyte(COM1_PORT + 0, 0x03);
    __outbyte(COM1_PORT + 1, 0x00);
    __outbyte(COM1_PORT + 3, 0x03);
    __outbyte(COM1_PORT + 2, 0xC7);
    __outbyte(COM1_PORT + 4, 0x0B);
}

void serial_putc(char c) {
    while ((__inbyte(COM1_PORT + 5) & 0x20) == 0)
        ;
    __outbyte(COM1_PORT, (unsigned char)c);
    if (c == '\n') serial_putc('\r');
}

void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}

void serial_print_hex(unsigned long long val) {
    const char hex[] = "0123456789ABCDEF";
    serial_print("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
}

void serial_print_dec(unsigned long long val) {
    if (val == 0) { serial_putc('0'); return; }
    char buf[21];
    int i = 0;
    while (val > 0) {
        buf[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    while (--i >= 0) serial_putc(buf[i]);
}
