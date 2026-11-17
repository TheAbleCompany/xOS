/*
 * buildinfo_artyom.c -- константы сборки TyrQuake для ArtyomX64.
 *
 * Заменяет common/buildinfo.c: там Build_DateString() вызывает
 * gmtime_r(), которого нет в mini-libc.  Здесь строка версии
 * фиксированная, поэтому strftime/gmtime не нужны.
 */

#include <stdint.h>

#include "common.h"

const char *build_version = stringify(TYR_VERSION);
const int64_t build_version_timestamp = TYR_VERSION_TIME;

const char *
Build_DateString()
{
    /* Строка постоянная: дата сборки для ArtyomX64-порта. */
    return "2026-10-24 00:00:00";
}
