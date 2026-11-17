CC = gcc
LD = ld

CFLAGS = \
	-m64 \
	-nostdlib \
	-ffreestanding \
	-fno-stack-protector \
	-fno-builtin \
	-fPIE \
	-fpie \
	-O2 \
	-Wall \
	-Wextra \
	-I../mini-libc/include

LDFLAGS = \
	-m elf_x86_64 \
	-nostdlib \
	-e _start \
	-static \
	-pie

LIBC = ../mini-libc/libc.a

RUN_CFLAGS = -msse2 -mfpmath=sse -fno-math-errno

PROGS = ls rm cat echo hello run ping

.PHONY: all clean

all: $(PROGS)

%: %.c $(LIBC)
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(LD) $(LDFLAGS) $@.o $(LIBC) -o $@
	rm -f $@.o

# llama2.c: float-математика, нужен SSE2 и отсутствие math-errno
# (иначе sqrtf/expf вылезают libcall'ами).
run: run.c $(LIBC)
	$(CC) $(CFLAGS) $(RUN_CFLAGS) -c $< -o $@.o
	$(LD) $(LDFLAGS) $@.o $(LIBC) -o $@
	rm -f $@.o

clean:
	rm -f $(PROGS) *.o
