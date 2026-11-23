#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <deque>
#define F(x) x
#define SERIAL_8N1 0
extern uint64_t g_now;
inline unsigned long millis() { return (unsigned long)g_now; }
inline void delay(unsigned long ms) { g_now += ms; }
inline unsigned long micros() { return (unsigned long)(g_now * 1000); }
inline long random(long n) { return n > 0 ? rand() % n : 0; }
struct Canal;
struct HS {
  // USB: entrada de comandos e saida de log
  std::deque<uint8_t> in; std::string out;
  // Radio
  HS *peer = nullptr; Canal *canal = nullptr; size_t pend = 0; uint32_t baud = 1200; bool radio = false;
  void begin(unsigned long b, int = 0, int = 0, int = 0, bool = false, unsigned long = 0, int = 0) { baud = b; }
  void setRxBufferSize(size_t) {} void setRxTimeout(int) {}
  int available() { return (int)in.size(); }
  int read() { if (in.empty()) return -1; int c = in.front(); in.pop_front(); return c; }
  size_t write(const uint8_t *b, size_t n);
  size_t write(uint8_t c) { return write(&c, 1); }
  void flush() { g_now += (uint64_t)(pend * 10000.0 / baud); pend = 0; }
  void print(const char *s) { out += s; } void print(char c) { out += c; }
  void println(const char *s = "") { out += s; out += '\n'; }
  int printf(const char *f, ...) { char b[512]; va_list a; va_start(a, f); int r = vsnprintf(b, sizeof b, f, a); va_end(a); out += b; return r; }
};
