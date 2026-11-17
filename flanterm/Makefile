include ../include.mk
.POSIX:
.PHONY: clean 

all: ft.o fb.o


ft.o: flanterm.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'
	
fb.o: flanterm_backends/fb.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'


clean:
	rm -f  *.o 
