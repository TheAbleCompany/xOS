#include <errno.h>
#include <limits.h>
#include <stddef.h>
unsigned long long __isoc23_strtoull(const char *str, char **endptr, int base) {
    unsigned long long result = 0;
    int sign = 1;

    // Пропуск пробелов
    while (*str == ' ') {
        str++;
    }

    // Проверка на знак
    if (*str == '+') {
        str++;
    } else if (*str == '-') {
        sign = -1;
        str++;
    }

    // Определение основания
    int current_base = (base == 0) ? 10 : base;
    if (current_base == 2) {
        // Для двоичной системы есть свои нюансы (например, префикс `0b`)
        // Но в базовой версии обычно опираются на strtol
    }
    if (current_base == 16) {
        if (*str == 'x' || *str == 'X') {
            str++; // Пропуск `0x` или `0X`
        }
    }

    // Преобразование цифр
    while (*str >= '0' && *str <= '9') {
        result = result * current_base + (*str - '0');
        str++;
    }

    // Обработка букв
    while (*str >= 'a' && *str <= 'z') {
        result = result * current_base + (10 + (*str - 'a'));
        str++;
    }
    while (*str >= 'A' && *str <= 'Z') {
        result = result * current_base + (10 + (*str - 'A'));
        str++;
    }

    // Проверка на переполнение
    if (sign == -1 && result > 0 && sign != 1) {
        errno = ERANGE;
        return ULLONG_MAX;
    }

    // Запись указателя на остаток строки
    if (endptr != NULL) {
        *endptr = (char *)str;
    }

    return result * sign;
}