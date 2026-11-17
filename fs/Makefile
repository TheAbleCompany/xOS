include ../include.mk
.POSIX:
.PHONY: clean 

all: fs.o ird.o rw.o open.o exec.o ldr.o switch.o


fs.o: fs.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
	
ird.o: initrd.c initrd.h
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

rw.o: rw.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

open.o: open.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

exec.o: exec.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

tsk.o: task.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
ldr.o: loader.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
switch.o: switch.S
	$(CC) -c -m64 -no-pie '$<' -o '$@'
	
clean:
	rm -f  *.o 
