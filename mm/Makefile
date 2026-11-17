include ../include.mk
.POSIX:
.PHONY: clean

all: pmm.o kheap.o vmm.o

pmm.o: pmm.c pmm.h
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

kheap.o: kheap.c kheap.h
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

vmm.o: vmm.c vmm.h
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

clean:
	rm -f *.o all
