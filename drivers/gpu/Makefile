.POSIX:

CC = gcc
# Ядро ArtyomX собрано с -mno-sse -mno-sse2 -mno-mmx (FPU-состояние
# задач не сохраняется), поэтому ixg-драйвер обязан избегать
# SSE/FP-инструкций: -mno-sse превращает float-операции в x87.
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
         -fno-strict-aliasing -fno-stack-protector -ffreestanding \
         -m64 -mcmodel=kernel -mno-red-zone -fno-pic -fno-pie \
         -mno-sse -mno-sse2 -mno-mmx \
         -I. -I../../lib -I../../drivers
# Debian/Ubuntu GCC (WSL в том числе) при -O1+ автоматически включает
# _FORTIFY_SOURCE, из-за чего <string.h> редиректит memcpy/memset в
# __memcpy_chk/__memset_chk, а их в freestanding-ядре нет -- получаем
# "undefined reference to `__memcpy_chk'". Явно отключаем фортификацию.
CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

# ixg_shader_vm.o исключён из сборки ядра: float-return ABI требует SSE,
# ядро же собрано с -mno-sse; шейдерная VM в GL-пути AxGL не участвует.
# ixg_sass_binary.o -- полный дубль ixg_sass.o (multiple definition).
# ixg_compute.o исключён: float-return ABI несовместим с -mno-sse,
# для AxGL не нужен (backends имеют локальные compute-функции).
GPU_OBJS = ixg_driver.o ixg_isa.o ixg_isa_encode.o ixg_sass.o ixg_sass_encode.o \
            ixg_mutants.o ixg_backend_amd.o ixg_backend_intel.o \
            ixg_backend_nvidia.o ixg_shader_compile.o ixg_pman.o ixg_kernel_glue.o

all: $(GPU_OBJS)

clean:
	rm -f *.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
