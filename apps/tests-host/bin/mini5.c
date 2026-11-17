extern long __artym_host_syscall(long, long, long, long, long, long, long);
int main(void){ long r = __artym_host_syscall(4, 2, (long)"E5\n", 3, 0, 0, 0); return r==3?0:1; }
