// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>

char *strcpy(char *destination, const char *source)
{
        /* TODO: Implement strcpy(). */
        int i = 0;
        while(source[i] != '\0') {
                destination[i] = source[i];
                i++;
        }
        destination[i] = '\0';
        return destination;
}

char *strncpy(char *destination, const char *source, size_t len)
{
        /*
         * POSIX: копируем не более len байт и ОСТАНАВЛИВАЕМСЯ на
         * завершающем '\0', добивая остаток нулями.
         *
         * Прежняя версия слепо копировала ровно len байт, читая далеко
         * за концом source.  Doom строит M_StringCopy() на strncpy();
         * при чтении списка сохранений это забивало буфер имени файла
         * мусором, fopen() получал битый путь, и слот оставался пустым
         * (сам файл сохранения при этом создавался и читался).
         */
        size_t i = 0;

        if (len == 0)
                return destination;

        while (i < len && source[i] != '\0') {
                destination[i] = source[i];
                i++;
        }

        while (i < len)
                destination[i++] = '\0';

        return destination;
}

char *strcat(char *destination, const char *source)
{
        /* TODO: Implement strcat(). */
        int i = strlen(destination);
        int j = 0;
        while(source[j] != '\0') {
                destination[i] = source[j];
                i++;
                j++;
        }
        destination[i] = '\0';
        return destination;
}

char *strncat(char *destination, const char *source, size_t len)
{
        /* TODO: Implement strncat(). */
        int i = strlen(destination);
        int j = 0;
        while(source[j] != '\0' && j < (int)len) {
                destination[i] = source[j];
                i++;
                j++;
        }
        destination[i] = '\0';
        return destination;
}

int strcmp(const char *str1, const char *str2)
{
        /* TODO: Implement strcmp(). */
        int i = 0;
        int ok;
        while (str1[i] != '\0' || str2[i] != '\0') {
                if (str1[i] > str2[i]) {
                        return 1;
                } else if (str1[i] < str2[i]) {
                        return -1;
                } else {
                        ok = 0;
                        i++;
                }
        }
        if (ok == 0) {
                return 0;
        }
        return 0;
}

int strncmp(const char *str1, const char *str2, size_t len)
{
        /* TODO: Implement strncmp(). */
        int i = 0;
        int ok;
        while (str1[i] != '\0' && i < (int)len) {
                if (str1[i] > str2[i]) {
                        return 1;
                } else if (str1[i] < str2[i]) {
                        return -1;
                } else {
                        ok = 0;
                        i++;
                }
        }
        if (ok == 0) {
                return 0;
        }
        return 0;
}

size_t strlen(const char *str)
{
        size_t i = 0;

        for (; *str != '\0'; str++, i++)
                ;

        return i;
}

char *strchr(const char *str, int c)
{
        /*
         * Замечание: strchr(s, 0) обязан вернуть указатель на
         * завершающий NUL (этот приём используется в lua-1.0 для
         * добавления в конец строки).
         */
        const char *p = str;

        for (;; p++) {
                if (*p == (char)c)
                        return (char *)p;

                if (*p == '\0')
                        return NULL;
        }
}

char *strrchr(const char *str, int c)
{
        /* Как и strchr, обязан находить и завершающий NUL. */
        const char *p = str;
        const char *last = NULL;

        for (;; p++) {
                if (*p == (char)c)
                        last = p;

                if (*p == '\0')
                        break;
        }

        return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
        /* TODO: Implement strstr(). */
        int i = 0;
        char* aux = (char *)haystack;
        while (aux[i] != '\0') {
                if (strncmp(aux + i, needle, strlen(needle)) == 0) {
                        return aux + i;
                }
                i++;
        }
        return NULL;
}

char *strrstr(const char *haystack, const char *needle)
{
        /* TODO: Implement strrstr(). */
        int i = 0;
        char* aux = NULL;
        while (haystack[i + strlen(needle)] != '\0') {
                if (strncmp(haystack + i, needle, strlen(needle)) == 0) {
                        aux = (char *)haystack + i;
                }
                i++;
        }
        return aux;
}

void *memcpy(void *destination, const void *source, size_t num)
{
        /* TODO: Implement memcpy(). */
        char* auxsrc = (char *)source;
        char* auxdest = (char *)destination;
        for (int i = 0; i < (int)num; i++) {
                auxdest[i] = auxsrc[i];
        }
        return destination;
}

void *memmove(void *destination, const void *source, size_t num)
{
        /*
         * Корректная обработка пересекающихся областей:
         *   - dest < src  -> копируем вперёд;
         *   - dest >= src -> копируем назад.
         */
        char *dst = (char *)destination;
        const char *src = (const char *)source;

        if (dst == src || num == 0)
                return destination;

        if (dst < src) {
                size_t i;

                for (i = 0; i < num; i++)
                        dst[i] = src[i];
        } else {
                size_t i = num;

                while (i-- > 0)
                        dst[i] = src[i];
        }

        return destination;
}

int memcmp(const void *ptr1, const void *ptr2, size_t num)
{
        const unsigned char *a = (const unsigned char *)ptr1;
        const unsigned char *b = (const unsigned char *)ptr2;
        size_t i;

        for (i = 0; i < num; i++) {
                if (a[i] != b[i])
                        return (int)a[i] - (int)b[i];
        }

        return 0;
}

void *memset(void *source, int value, size_t num)
{
        /* TODO: Implement memset(). */
        unsigned char* aux = source;
    int i = 0;
        while (i < (int)num)
    {
        aux[i] = (unsigned char)value;
                i++;
    }
        return source;
}


unsigned int is_delim(char c, char *delim)
{
    while(*delim != '\0')
    {
        if(c == *delim)
            return 1;
        delim++;
    }
    return 0;
}
char *strtok(char *srcString, char *delim)
{
    static char *backup_string; // start of the next search
    if(!srcString)
    {
        srcString = backup_string;
    }
    if(!srcString)
    {
        // user is bad user
        return 0;
    }
    // handle beginning of the string containing delims
    while(1)
    {
        if(is_delim(*srcString, delim))
        {
            srcString++;
            continue;
        }
        if(*srcString == '\0')
        {
            // we've reached the end of the string
            return 0; 
        }
        break;
    }
    char *ret = srcString;
    while(1)
    {
        if(*srcString == '\0')
        {
            /*end of the input string and
            next exec will return NULL*/
            backup_string = srcString;
            return ret;
        }
        if(is_delim(*srcString, delim))
        {
            *srcString = '\0';
            backup_string = srcString + 1;
            return ret;
        }
        srcString++;
    }
}
/*
 * strpbrk: первый символ s, входящий в набор accept.
 * Нужен tcc (tcctools/tiny_libmaker).
 */
char *strpbrk(const char *s, const char *accept)
{
        const char *a;

        if (s == NULL || accept == NULL)
                return NULL;

        for (; *s; s++) {
                for (a = accept; *a; a++) {
                        if (*s == *a)
                                return (char *)s;
                }
        }
        return NULL;
}

/*
 * strcasecmp/strncasecmp: ASCII-only сравнение без учёта регистра.
 * Их ждут quake (cmd.c, common.c, cd_common.c) и doom (doomtype.h
 * через <strings.h>).  Локаль не применяется -- в системе её нет.
 */
static int ax_fold_ascii(unsigned char c)
{
        if (c >= 'A' && c <= 'Z')
                return (int)c + ('a' - 'A');
        return (int)c;
}

int strcasecmp(const char *str1, const char *str2)
{
        unsigned char c1, c2;

        do {
                c1 = (unsigned char)*str1++;
                c2 = (unsigned char)*str2++;
                if (ax_fold_ascii(c1) != ax_fold_ascii(c2))
                        return ax_fold_ascii(c1) - ax_fold_ascii(c2);
        } while (c1 && c2);

        return 0;
}

int strncasecmp(const char *str1, const char *str2, size_t len)
{
        unsigned char c1, c2;

        while (len--) {
                c1 = (unsigned char)*str1++;
                c2 = (unsigned char)*str2++;
                if (ax_fold_ascii(c1) != ax_fold_ascii(c2))
                        return ax_fold_ascii(c1) - ax_fold_ascii(c2);
                if (!c1)
                        return 0;
        }

        return 0;
}
