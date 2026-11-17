#include "../lib/unistd.h"

int _start() {
    char name[20];
    
    write(1, "Hello, world!\n", 14);
    write(1, "What's your name?: ", 19);
    
    read(0, name,20);
    write(1,name,20);
    return 0;
}