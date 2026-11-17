.POSIX:
.PHONY: all clean run run-img run-limine limine-iso grub-iso

LIMINE_BIN := limine-binary

# r12-bootfix: выбор хостового инструмента Limine по ОС.
# На Linux используется limine-binary/limine-tool (собирается так:
#   make -C limine-binary   или:  gcc -O2 -std=c99 limine-binary/limine.c -o limine-binary/limine-tool
# бинарник уже собран и лежит в репозитории).
# На Windows -- прежний limine.exe.
#UNAME_S := $(shell uname -s 2>/dev/null || echo Windows_NT)
#ifeq ($(UNAME_S),Linux)
#LIMINE_TOOL := $(LIMINE_BIN)/limine-tool
#else
LIMINE_TOOL := $(LIMINE_BIN)/limine-tool-windows-x86/limine.exe
#endif

LIMINE_ISO_DIR := limine-iso-root

all:
	$(MAKE) -C flanterm
	$(MAKE) -C kernel
	$(MAKE) -C drivers
	$(MAKE) -C drivers/gpu
	#$(MAKE) -C drivers/usb
	$(MAKE) -C fs
	$(MAKE) -C lib
	$(MAKE) -C mm
	$(MAKE) -C multitask
	$(MAKE) -C libs/mesa
	$(MAKE) main.elf
	$(MAKE) -C server

main.iso: limine-iso

# GPU-стек ixg (NVIDIA/AMD/Intel) + glue malloc/free -> kmalloc/kfree.
# NOTE: ixg_sass_binary.o НЕ линкуем -- это дубль ixg_sass.o (multiple definition).
# NOTE: ixg_shader_vm.o НЕ линкуем -- float-return ABI требует SSE,
#       а ядро -mno-sse; для AxGL шейдерная VM не нужна.
# ixg нужен для GPU_SUBMIT_RT (kernel/ipc.c): сырой сабмит
# треугольников без понятия "окон" (окна -- забота wserver).
IXG_OBJS = drivers/gpu/ixg_driver.o \
           drivers/gpu/ixg_isa.o \
           drivers/gpu/ixg_isa_encode.o \
           drivers/gpu/ixg_sass.o \
           drivers/gpu/ixg_sass_encode.o \
           drivers/gpu/ixg_mutants.o \
           drivers/gpu/ixg_backend_amd.o \
           drivers/gpu/ixg_backend_intel.o \
           drivers/gpu/ixg_backend_nvidia.o \
           drivers/gpu/ixg_shader_compile.o \
           drivers/gpu/ixg_pman.o \
           drivers/gpu/ixg_kernel_glue.o

main.elf: flanterm/ft.o \
	flanterm/fb.o \
	dev/pty.o \
	dev/console.o \
	fs/sock.o \
	kernel/selftest.o \
	kernel/mm_stats.o \
	drivers/net/ixn_kernel_port.o \
	kasm.o \
	boot/limine_boot.o \
	lib/com.o \
	lib/sc.o \
	lib/libc.o \
	lib/kalloc.o \
	drivers/fb.o \
	drivers/fnt.o \
	drivers/tm.o \
	drivers/kb.o \
	drivers/vga.o \
	drivers/mouse.o \
	drivers/xhci.o \
	fs/fs.o \
	fs/ird.o \
	drivers/disk.o \
	drivers/pcix.o \
	drivers/blkdev.o \
	drivers/ahci.o \
	drivers/nvme.o \
	fs/ix/ixk_storage.o \
	fs/ix/ix_fs_detect.o \
	fs/ix/ix_fs_fat.o \
	fs/ix/ix_fs_ext.o \
	fs/ix/ix_fs_ntfs.o \
	fs/ix/ix_automount.o \
	multitask/ldr.o \
	multitask/tsk.o \
	multitask/switch.o \
	mm/kheap.o \
	mm/vmm.o \
	mm/pmm.o \
	kernel/kern.o \
	kernel/abi.o \
	kernel/fpu.o \
	kernel/ipc.o \
	kernel/dt.o \
	kernel/gdt.o \
	kernel/isr.o \
	kernel/int.o  \
	kernel/vgafnt.o \
	kernel/thunk.o \
	kernel/tls.o \
	kernel/mesa_hooks.o \
	kernel/mesa_glue.o \
	fs/rw.o \
	fs/open.o \
	fs/exec.o \
		drivers/net/ixn_netdev.o \
		drivers/net/ixn_skb.o \
		drivers/net/ixn_l2l3.o \
		drivers/net/ixn_l4.o \
		drivers/net/ixn_dhcp_dns.o \
		drivers/net/ixn_socket_api.o \
		drivers/net/ixn_pci.o \
		drivers/net/ixn_e1000.o \
		drivers/net/ixn_rtl8139.o \
		drivers/net/ixn_eth_misc.o \
		drivers/net/ixn_tls.o \
		drivers/net/ixn_net.o \
		drivers/net/ixn_boot_report.o \
		drivers/net/ixn_http_ws.o \
		drivers/net/ixn_usbnet.o \
		drivers/net/ixn_wifi.o \
		$(IXG_OBJS)
	ld -m elf_x86_64 -nostdlib -z norelro -T link.ld $^ \
		--start-group libs/mesa/build/libmesa_port.a libs/mesa/build/libmesa.a --end-group \
		-L$(shell gcc -print-file-name=libgcc.a | xargs dirname) -lgcc \
		-o '$@'

kasm.o: boot/itdo.asm
	nasm -f elf64 -w-label-redef-late -w-implicit-abs-deprecated '$<' -o '$@'

MESA_GL_CFLAGS = -m64 -mcmodel=kernel -mno-red-zone -ffreestanding \
	-fno-builtin -fno-stack-protector -fno-pic -fno-pie -msse2 -O2 -w \
	-Ilibs/mesa/include

kernel/mesa_glue.o: kernel/mesa_glue.c kernel/mesa_glue.h libs/mesa/port/src/port_gl_api.h
	$(CC) $(MESA_GL_CFLAGS) -c '$<' -o '$@'

kernel/mesa_hooks.o: kernel/mesa_hooks.c kernel/mesa_glue.h fs/fs.h fs/file.h multitask/task.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

kernel/tls.o: kernel/tls.c kernel/tls.h mm/kheap.h
kernel/abi.o: kernel/abi.c kernel/abi.h kernel/isr.h multitask/task.h fs/task.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

	$(CC) $(CFLAGS) -c '$<' -o '$@'

boot/limine_boot.o: boot/limine_boot.c boot/limine.h boot/multiboot.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'

dev/console.o: dev/console.c
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
dev/pty.o: dev/pty.c
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'

fs/sock.o: fs/sock.c fs/sock.h multitask/task.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

drivers/net/ixn_kernel_port.o: drivers/net/ixn_kernel_port.c drivers/net/ixn_kernel_port.h
	$(CC) $(CFLAGS) -c '$<' -o '$@'

# --- fs/ix: автоматическое монтирование разделов (FAT/ext/NTFS) ---
fs/ix/ixk_storage.o: fs/ix/ixk_storage.c fs/ix/kernel.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
fs/ix/ix_fs_detect.o: fs/ix/ix_fs_detect.c fs/ix/ix_fs_detect.h fs/ix/ixstd_fs.h fs/ix/kernel.h fs/ix/ixk_string.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
fs/ix/ix_fs_fat.o: fs/ix/ix_fs_fat.c fs/ix/ix_fs_fat.h fs/ix/ixstd_fs.h fs/ix/ix_fs_detect.h fs/ix/kernel.h fs/ix/ixk_string.h fs/ix/ixk_video.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
fs/ix/ix_fs_ext.o: fs/ix/ix_fs_ext.c fs/ix/ix_fs_ext.h fs/ix/ixstd_fs.h fs/ix/kernel.h fs/ix/ixk_string.h fs/ix/ixk_video.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
fs/ix/ix_fs_ntfs.o: fs/ix/ix_fs_ntfs.c fs/ix/ix_fs_ntfs.h fs/ix/ixstd_fs.h fs/ix/kernel.h fs/ix/ixk_string.h fs/ix/ixk_video.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'
fs/ix/ix_automount.o: fs/ix/ix_automount.c fs/ix/ix_automount.h fs/ix/ix_fs_detect.h fs/ix/ix_fs_fat.h fs/ix/ix_fs_ext.h fs/ix/ix_fs_ntfs.h fs/ix/ixstd_fs.h fs/ix/kernel.h fs/ix/ixk_string.h fs/fs.h
	gcc -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -Wall -Wextra -Wno-incompatible-pointer-types -c '$<' -o '$@'


# --- drivers/net: ixn TCP/IP stack (BSD sockets via fs/sock.c) ---
NET_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
	-fno-strict-aliasing -fno-stack-protector -ffreestanding \
	-m64 -mcmodel=kernel -mno-red-zone -fno-pic -fno-pie \
	-mno-sse -mno-sse2 -mno-mmx -Idrivers/net -Idrivers -Ilib

drivers/net/%.o: drivers/net/%.c
	$(CC) $(NET_CFLAGS) -c '$<' -o '$@'
limine-iso: main.elf
	rm -rf $(LIMINE_ISO_DIR)
	mkdir -p $(LIMINE_ISO_DIR)/boot/limine
	cp main.elf $(LIMINE_ISO_DIR)/boot/
	cp apps/initrd.img $(LIMINE_ISO_DIR)/boot/
	cp boot/limine/limine.conf $(LIMINE_ISO_DIR)/boot/limine/
	cp $(LIMINE_BIN)/limine-bios.sys $(LIMINE_ISO_DIR)/boot/limine/
	cp $(LIMINE_BIN)/limine-bios-cd.bin $(LIMINE_ISO_DIR)/boot/limine/
	cp $(LIMINE_BIN)/limine-uefi-cd.bin $(LIMINE_ISO_DIR)/boot/limine/
	mkdir -p $(LIMINE_ISO_DIR)/EFI/BOOT
	cp $(LIMINE_BIN)/BOOTX64.EFI $(LIMINE_ISO_DIR)/EFI/BOOT/
	cp $(LIMINE_BIN)/BOOTIA32.EFI $(LIMINE_ISO_DIR)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	--efi-boot boot/limine/limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label \
	$(LIMINE_ISO_DIR) -o main.iso
	$(LIMINE_TOOL) bios-install main.iso

grub-iso: main.elf
	cp '$<' iso/boot
	grub-mkrescue -o main-grub.iso iso

clean:
	rm -f *.elf *.o iso/boot/*.elf iso/boot/*.img *.img main-grub.iso
	find ./kernel -name "*.o" -type f -delete
	find ./fs -name "*.o" -type f -delete
	find ./drivers -name "*.o" -type f -delete
	find ./lib -name "*.o" -type f -delete
	find ./mm -name "*.o" -type f -delete
	find ./boot -name "*.o" -type f -delete
	$(MAKE) -C drivers/gpu clean
	rm -rf $(LIMINE_ISO_DIR)

run: main.elf
	qemu-system-x86_64 -kernel '$<' -initrd apps/initrd.img  -machine type=pc-i440fx-3.1

#run-img: main.iso
#       qemu-system-x86_64 -cdrom '$<' \
#       -m 256M \
#       -vga std

run-limine: main.iso
	qemu-system-x86_64  \
	-cdrom '$<' \
	-m 512M \
	-serial stdio \
	-device qemu-xhci \
	-machine  vmport=off

run-disk:
	qemu-system-x86_64 -M pc -m 512M -cdrom main.iso -hda testdisk.img -boot d -serial stdio
#-device virtio-gpu-gl \
#-display sdl,gl=on
#-device usb-ehci,id=ehci \
#-device piix3-usb-uhci

