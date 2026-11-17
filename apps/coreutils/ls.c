#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define DENTS_BUF 1024

int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "initrd";
    struct ax_dirent *de;
    char buf[DENTS_BUF];
    int fd;
    int n;
    int pos;

    fd = open(path, O_RDONLY, 0);

    if (fd < 0) {
        printf("ls: cannot open %s\n", path);
        return 1;
    }

    for (;;) {
        n = getdents(fd, buf, sizeof(buf));

        if (n <= 0)
            break;

        pos = 0;

        while (pos < n) {
            de = (struct ax_dirent *)(buf + pos);

            if (de->d_reclen < 9)
                break;

            puts(de->d_name);
            pos += de->d_reclen;
        }
    }

    close(fd);

    return 0;
}
