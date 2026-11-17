include ../include.mk
.POSIX:
.PHONY: clean 

all: ldr.o tsk.o switch.o


tsk.o: task.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
ldr.o: loader.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
switch.o: switch.S
	$(CC) -c -m64 -no-pie '$<' -o '$@'
	
clean:
	rm -f  *.o 
