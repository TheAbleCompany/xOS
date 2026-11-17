#include <stdio.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

// Реализация fgetc с буферизацией
int fgetc(FILE *stream) {
    // Проверка на валидность указателя
    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }
    
    // Проверка, что файл открыт для чтения
    if (!(stream->_flags & _IO_READ)) {
        errno = EBADF;
        return EOF;
    }
    
    // Если буфер пуст, заполняем его
    if (stream->_r <= 0) {
        // Читаем данные из файла в буфер
        ssize_t nread = read(stream->_fileno, stream->_buf, stream->_bufsize);
        
        if (nread <= 0) {
            // Если конец файла или ошибка
            if (nread == 0) {
                stream->_flags |= _IO_EOF;  // Устанавливаем флаг EOF
            } else {
                stream->_flags |= _IO_ERR;  // Устанавливаем флаг ошибки
            }
            return EOF;
        }
        
        // Обновляем счетчики буфера
        stream->_r = nread;
        stream->_p = stream->_buf;
    }
    
    // Читаем один символ из буфера
    int c = *stream->_p++;
    stream->_r--;
    
    return c;
}

// Альтернативная реализация без буферизации (для сравнения)
int fgetc_simple(FILE *stream) {
    unsigned char ch;
    
    if (stream == NULL) {
        return EOF;
    }
    
    // Читаем один байт напрямую
    if (read(stream->_fileno, &ch, 1) == 1) {
        return ch;
    }
    
    return EOF;
}