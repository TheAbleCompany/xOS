#include <internal/syscall.h>
int main(void){ long r = syscall(4, 2, (long)"E6\n", 3); return r==3?0:1; }
