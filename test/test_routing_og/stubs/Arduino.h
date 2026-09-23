// Host stub of the Arduino core - just enough for the OG router to compile.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef uint8_t byte;
typedef bool boolean;
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1

extern bool g_quiet;

class Stream {
public:
    template <typename T> int print(T v) { if (g_quiet) return 1; printv(v); return 1; }
    template <typename T> int println(T v) { if (g_quiet) return 1; printv(v); putchar('\n'); return 1; }
    void println() { if (g_quiet) return; putchar('\n'); }
    void printf(const char* fmt, ...) {
        if (g_quiet) return;
        va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    }
    void flush() {}
    int available() { return 0; }
    int read() { return -1; }
    void write(char c) { if (!g_quiet) putchar(c); }
private:
    void printv(const char* s) { fputs(s ? s : "(null)", stdout); }
    void printv(char* s) { fputs(s ? s : "(null)", stdout); }
    void printv(char c) { putchar(c); }
    void printv(int v) { printf("%d", v); }
    void printv(unsigned int v) { printf("%u", v); }
    void printv(long v) { printf("%ld", v); }
    void printv(unsigned long v) { printf("%lu", v); }
    void printv(long long v) { printf("%lld", v); }
    void printv(unsigned long long v) { printf("%llu", v); }
    void printv(short v) { printf("%d", v); }
    void printv(unsigned short v) { printf("%u", v); }
    void printv(signed char v) { printf("%d", v); }
    void printv(unsigned char v) { printf("%u", v); }
    void printv(bool v) { printf("%d", v); }
    void printv(double v) { printf("%g", v); }
    void printv(float v) { printf("%g", v); }
};
extern Stream Serial;

static inline unsigned long micros() { return 0; }
static inline unsigned long millis() { return 0; }
static inline void delay(unsigned long) {}
static inline void delayMicroseconds(unsigned int) {}
static inline void digitalWrite(int, int) {}
static inline int digitalRead(int) { return 1; }
static inline void pinMode(int, int) {}
static inline long random(long n) { return rand() % (n ? n : 1); }
static inline int get_core_num() { return 0; }
