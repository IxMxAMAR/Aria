// efi/serial.h — COM1 serial output for bare-metal debugging
#pragma once

#define COM1_PORT 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_print(const char* s);
void serial_print_hex(unsigned long long val);
void serial_print_dec(unsigned long long val);

// Debug macros — compiled out in RELEASE builds
#ifdef ARIA_DEBUG
#define DBG_PRINT(s)    serial_print(s)
#define DBG_HEX(v)      serial_print_hex(v)
#define DBG_DEC(v)      serial_print_dec(v)
#else
#define DBG_PRINT(s)    ((void)0)
#define DBG_HEX(v)      ((void)0)
#define DBG_DEC(v)      ((void)0)
#endif
