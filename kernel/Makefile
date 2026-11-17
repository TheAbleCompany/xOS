include ../include.mk
.POSIX:
.PHONY: clean

# Модель NeXT/macOS: GUI-код (axshell*, axgl, ksh) УШЁЛ из ядра.
# Ядро: загрузка, задачи, FS, драйверы + примитивы IPC (ipc.c).
all: kern.o selftest.o mm_stats.o dt.o isr.o gdt.o int.o vgafnt.o thunk.o ipc.o \
     compat.o compat_asm.o tls.o mesa_hooks.o mesa_glue.o


selftest.o: selftest.c ../fs/sock.h ../mm/kheap.h ../mm/pmm.h ../mm/vmm.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

mm_stats.o: mm_stats.c ../mm/pmm.h ../mm/kheap.h ../mm/vmm.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

kern.o: main.c
	$(CC) $(CFLAGS) -c '$<' -o '$@'

# Примитивы IPC: порты, SHM, tty, ввод, FB_INFO, GPU_SUBMIT_RT
fpu.o: fpu.c fpu.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

ipc.o: ipc.c ipc.h ../lib/axport.h ../lib/axipc.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

dt.o: descriptor_tables.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

isr.o: isr.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

vgafnt.o: vgafnt.c
	$(CC) -c $(CFLAGS) -c '$<' -o '$@'

gdt.o: asm/gdt.asm
	nasm -f elf64 '$<' -o '$@'

int.o: asm/interrupt.asm
	nasm -f elf64 '$<' -o '$@'

thunk.o: asm/thunk.asm
	nasm -f elf64 '$<' -o '$@'

compat.o: compat.c compat.h macho.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

compat_asm.o: asm/compat.asm
	nasm -f elf64 '$<' -o '$@'

tls.o: tls.c tls.h ../mm/kheap.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

mesa_hooks.o: mesa_hooks.c ../fs/fs.h ../fs/file.h ../multitask/task.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

MESA_GL_CFLAGS := -m64 -mcmodel=kernel -mno-red-zone -ffreestanding \
	-fno-builtin -fno-stack-protector -fno-pic -fno-pie -msse2 -O2 -w \
	-I../libs/mesa/include

mesa_glue.o: mesa_glue.c mesa_glue.h ../libs/mesa/port/src/port_gl_api.h
	$(CC) $(MESA_GL_CFLAGS) -c '$<' -o '$@'

clean:
	rm -f  *.o
