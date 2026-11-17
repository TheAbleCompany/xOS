// SPDX-License-Identifier: BSD-3-Clause

#include <errno.h>
#include <stdio.h>
#include <string.h>

int errno;
#define ERANGE 34
#define EINVAL 22

/*
 * strerror: короткие описания главных кодов ошибок ядра
 * + числовой fallback для остальных ("Unknown error N").
 */
char *strerror(int errnum)
{
        static char buf[32];

        switch (errnum) {
        case 1:  return "Operation not permitted";
        case 2:  return "No such file or directory";
        case 4:  return "Interrupted system call";
        case 5:  return "Input/output error";
        case 9:  return "Bad file descriptor";
        case 11: return "Resource temporarily unavailable";
        case 12: return "Cannot allocate memory";
        case 13: return "Permission denied";
        case 14: return "Bad address";
        case 16: return "Device or resource busy";
        case 17: return "File exists";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 24: return "Too many open files";
        case 27: return "File too large";
        case 28: return "No space left on device";
        case 29: return "Illegal seek";
        case 30: return "Read-only file system";
        case 34: return "Numerical result out of range";
        case 36: return "Name too long";
        case 38: return "Function not implemented";
        case 39: return "Directory not empty";
        default:
                snprintf(buf, sizeof(buf), "Unknown error %d", errnum);
                return buf;
        }
}
