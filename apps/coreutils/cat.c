#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static char buf[512];

static void cat(int fd)
{
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, (unsigned int)n) != n) {
            printf("cat: write error\n");
            return;
        }
    }

    if (n < 0)
        printf("cat: read error\n");
}

int main(int argc, char *argv[])
{
    int fd;
    int i;

    if (argc <= 1) {
        cat(0);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        fd = open(argv[i], O_RDONLY, 0);

        if (fd < 0) {
            printf("cat: cannot open %s\n", argv[i]);
            return 1;
        }

        cat(fd);
        close(fd);
    }

    return 0;
}
