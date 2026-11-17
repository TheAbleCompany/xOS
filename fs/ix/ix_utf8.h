// ix_utf8.h -- UTF-16LE <-> UTF-8 и регистронезависимое сравнение имён.
//
// Имена на диске: FAT LFN и NTFS -- UTF-16, ext -- байты (обычно UTF-8).
// Ядро и VFS работают с UTF-8.  Раньше любой символ >= 128 заменялся на '_':
// кириллические файлы получали имена "_________", по которым их нельзя
// было найти (и разные файлы сливались в одно имя).

#pragma once

#include <stdint.h>

// Дописать кодпоинт как UTF-8 в out[pos..]; вернуть новую позицию или -1,
// если не хватает места (нужно <= 4 байт + место под NUL остаётся за вызывающим).
static inline int ix_utf8_put(uint32_t cp, char* out, int pos, int cap) {
  if (cp < 0x80u) {
    if (pos + 1 > cap - 1) return -1;
    out[pos++] = (char)cp;
  } else if (cp < 0x800u) {
    if (pos + 2 > cap - 1) return -1;
    out[pos++] = (char)(0xC0u | (cp >> 6));
    out[pos++] = (char)(0x80u | (cp & 0x3Fu));
  } else if (cp < 0x10000u) {
    if (pos + 3 > cap - 1) return -1;
    out[pos++] = (char)(0xE0u | (cp >> 12));
    out[pos++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[pos++] = (char)(0x80u | (cp & 0x3Fu));
  } else {
    if (pos + 4 > cap - 1) return -1;
    out[pos++] = (char)(0xF0u | (cp >> 18));
    out[pos++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[pos++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[pos++] = (char)(0x80u | (cp & 0x3Fu));
  }
  return pos;
}

// UTF-16 (n кодовых единиц) -> UTF-8, NUL-терминированная строка в out[cap].
// Суррогатные пары склеиваются; одиночные суррогаты -> U+FFFD.
// Возвращает длину в байтах.
static inline int ix_utf16_to_utf8(const uint16_t* u, int n, char* out, int cap) {
  int pos = 0;
  for (int i = 0; i < n; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < n &&
        u[i + 1] >= 0xDC00u && u[i + 1] <= 0xDFFFu) {
      cp = 0x10000u + ((cp - 0xD800u) << 10) + (u[i + 1] - 0xDC00u);
      i++;
    } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
      cp = 0xFFFDu;
    }
    int np = ix_utf8_put(cp, out, pos, cap);
    if (np < 0) break;
    pos = np;
  }
  out[pos] = 0;
  return pos;
}

// То же, но из сырых байт UTF-16LE (невыровненный указатель).
static inline int ix_utf16le_to_utf8(const uint8_t* p, int n, char* out, int cap) {
  uint16_t tmp[256];
  if (n > 256) n = 256;
  for (int i = 0; i < n; i++) tmp[i] = (uint16_t)(p[i * 2] | ((uint16_t)p[i * 2 + 1] << 8));
  return ix_utf16_to_utf8(tmp, n, out, cap);
}

// Следующий кодпоинт из UTF-8 (двигает *pp). Невалидный байт возвращается
// как есть (Latin-1), чтобы строка не "застревала".
static inline uint32_t ix_utf8_next(const char** pp) {
  const uint8_t* s = (const uint8_t*)*pp;
  uint32_t c = s[0];
  if (c < 0x80u) { *pp += 1; return c; }
  if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
    *pp += 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
  }
  if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
    *pp += 3; return ((c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
  }
  if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u &&
      (s[3] & 0xC0u) == 0x80u) {
    *pp += 4;
    return ((c & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
           ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
  }
  *pp += 1;
  return c;
}

// Простое сворачивание регистра: ASCII, Latin-1, греческий, кириллица.
static inline uint32_t ix_cp_fold(uint32_t c) {
  if (c < 0x80u) return (c >= 'A' && c <= 'Z') ? c + 32u : c;
  if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u) return c + 32u;
  if (c >= 0x391u && c <= 0x3A9u && c != 0x3A2u) return c + 32u;
  if (c >= 0x410u && c <= 0x42Fu) return c + 32u;
  if (c >= 0x400u && c <= 0x40Fu) return c + 80u;
  return c;
}

// Сравнение двух UTF-8 имён без учёта регистра; '\' приравнен к '/'.
static inline int ix_utf8_casecmp(const char* a, const char* b) {
  while (*a && *b) {
    uint32_t ca = ix_utf8_next(&a), cb = ix_utf8_next(&b);
    if (ca == '\\') ca = '/';
    if (cb == '\\') cb = '/';
    ca = ix_cp_fold(ca); cb = ix_cp_fold(cb);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
