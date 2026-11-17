#include "../lib/common.h"

#include <stddef.h>
#include <stdint.h>

#define TCFLSH 21515

// unistd.h - используйте обычный read без yield

// unistd.h - замените read на блокирующую версию
void yield(void){
     int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(151)
        : "memory"
    );
}
int read(int fd, void *buf, size_t count) {
    char *p = (char*)buf;
    size_t total = 0;
    
    while (total < count) {
        int ret;
        __asm__ volatile(
            "int $0x80"
            : "=a"(ret)
            : "a"(0), "D"((long)fd), "S"((long)(p + total)), "d"((long)(count - total))
            : "memory"
        );
        
        if (ret > 0) {
            total += ret;
            // Если получили новую строку - завершаем чтение
            if (p[total - 1] == '\n') break;
        } else if (ret == 0) {
            // Нет данных - ждем
            yield();  // Передаем управление другим задачам
        } else {
            // Ошибка
            return -1;
        }
    }
    
    // Добавляем null-терминатор
    if (total < count) p[total] = 0;
    return total;
}

// Если хотите блокирующий read, оставьте старую версию
// Но для консоли лучше не блокировать

int write(int fd, void *buf, size_t count) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "D"((long)fd), "S"((long)buf), "d"((long)count)
        : "memory"
    );
    return ret;
}