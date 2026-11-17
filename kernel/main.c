// ============================================
// 🔬 PIC-only keyboard diagnostic main.c
// ============================================
 
#include <stdint.h>
#include <stddef.h>
 
#include "../drivers/monitor.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../drivers/xhci.h"
 
#include "descriptor_tables/descriptor_tables.h"
#include "descriptor_tables/fadt.h"
#include "isr.h"
#include "../boot/multiboot.h"
#include "../fs/fs.h"
#include "../fs/initrd.h"
#include "../fs/ix/ix_automount.h"
#include "../fs/sock.h"

void ax_selftest_run(void);
void ax_mm_report(const char *tag);
#include "../drivers/net/ixn_net.h"
#include "../drivers/disk.h"
#include "../drivers/blkdev.h"
#include "../drivers/pcix.h"
#include "../multitask/task.h"
#include "../lib/syscalls.h"
 
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
 
#include "../dev/console.h"
#include "../dev/pty.h"
 
#include "../flanterm/flanterm.h"
#include "../boot/limine.h"
 
#include "compat.h"
#include "abi.h"
 
#include "acpi.h"
extern struct flanterm_context *ft_ctx;
 
extern uint64_t framebuffer_addr;
extern uint32_t framebuffer_width;
extern uint32_t framebuffer_height;
extern uint32_t framebuffer_pitch;
extern uint32_t framebuffer_bpp;
 
#include <stdint.h>
 
#include <stdint.h>
 
#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66
 
#define EC_CMD_READ     0x80
 
#define EC_STAT_OBF     0x01 // Output Buffer Full (данные готовы для чтения)
#define EC_STAT_IBF     0x02 // Input Buffer Full (контроллер занят записью)
 
// Базовые функции работы с портами x86
/*static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
 
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}*/
 
// Ожидание очистки входного буфера EC (готов принять команду)
static void ec_wait_write(void) {
    int timeout = 100000; // Защита от вечного зависания
    while ((inb(EC_CMD_PORT) & EC_STAT_IBF) && timeout--) {
        __asm__ volatile("pause");
    }
}
 
// Ожидание заполнения выходного буфера EC (готов отдать данные)
static void ec_wait_read(void) {
    int timeout = 100000;
    while (!(inb(EC_CMD_PORT) & EC_STAT_OBF) && timeout--) {
        __asm__ volatile("pause");
    }
}
 
// Функция чтения регистра EC
uint8_t ec_read_register(uint8_t reg) {
    ec_wait_write();
    outb(EC_CMD_PORT, EC_CMD_READ); // Посылаем команду чтения
 
    ec_wait_write();
    outb(EC_DATA_PORT, reg);         // Посылаем адрес нужного регистра
 
    ec_wait_read();
    return inb(EC_DATA_PORT);        // Считываем результат
}
 
// Пример использования для ASUS
int get_battery_percentage_fast(void) {
    // На ASUS P553M процент обычно лежит в регистре 0x26 или 0x30
    uint8_t percent = ec_read_register(0xb0);//(0x26); 
    
    if (percent <= 100) {
        return percent;
    }
    
    // Если в 0x26 мусор, пробуем альтернативный регистр
    /*percent = ec_read_register(0x30);
    return (percent <= 100) ? percent : -1; */
}
/*void debug_dump_ec(void) {
    printf("\r\n--- EMBEDDED CONTROLLER DUMP ---\r\n");
    for (int reg = 0x00; reg <= 0xFF; reg++) {
        uint8_t val = ec_read_register(reg);
        
        // Выводим только те регистры, где значения похожи на правду (от 1 до 100)
        // Чтобы исключить пустые регистры (0x00) и ошибки (0xFF)
        if (val > 0 && val <= 100) {
            printf("Reg 0x%X = %d (0x%X)\r\n", reg, val, val);
        }
    }
    printf("--------------------------------\r\n");
}
 */
/*
 * ============================================
 * SERIAL DEBUG
 * ============================================
 */
 
void serial_putc(char c)
{
    debug_putc(c);      /* COM1 + копия в лог-буфер (storage.log) */
}
 
void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
 
void debug_msg(const char *msg)
{
    serial_puts("[DEBUG] ");
    serial_puts(msg);
    serial_puts("\n");
}
 
void debug_hex(uint64_t val)
{
    const char hex[] = "0123456789ABCDEF";
 
    serial_puts("0x");
 
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}
 
/*
 * ============================================
 * BOOT STAGE (r12-bootfix)
 * ============================================
 *
 * Печатает этап загрузки И в serial (COM1), И на экранный
 * консоль (flanterm).  Раньше на экране появлялись только
 * две строки -- и если ядро зависало на конкретном железе,
 * пользователь не видел, ГДЕ именно.  Теперь экранное
 * консольное окно отладки всегда показывает последний
 * успешно пройденный этап инициализации.
 */
static size_t boot_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
 
void boot_stage(const char *msg)
{
    serial_puts("[BOOT] ");
    serial_puts(msg);
    serial_puts("\n");
 
    if (ft_ctx) {
        flanterm_write(ft_ctx, msg, boot_strlen(msg));
        flanterm_write(ft_ctx, "\r\n", 2);
    }
}
 
 
/*
 * ============================================
 * FRAMEBUFFER
 * ============================================
 */
 
void fb_fill(uint32_t color)
{
    if (framebuffer_addr == 0)
        return;
 
    uint8_t *fb = (uint8_t *)(uintptr_t)framebuffer_addr;
 
    for (uint32_t y = 0; y < framebuffer_height; y++) {
 
        uint32_t *row =
            (uint32_t *)(fb + y * framebuffer_pitch);
 
        for (uint32_t x = 0; x < framebuffer_width; x++)
            row[x] = color;
    }
}
 
 
/*
 * ============================================
 * PIC 8259
 * ============================================
 *
 * Master PIC:
 *   command = 0x20
 *   data    = 0x21
 *
 * Slave PIC:
 *   command = 0xA0
 *   data    = 0xA1
 *
 * Remapping:
 *
 *   IRQ0  -> INT 0x20
 *   IRQ1  -> INT 0x21   <-- keyboard
 *   IRQ2  -> INT 0x22
 *   ...
 *
 *   IRQ8  -> INT 0x28
 *   ...
 *   IRQ15 -> INT 0x2F
 */
 
static void pic_remap(void)
{
    uint8_t master_mask = inb(0x21);
    uint8_t slave_mask  = inb(0xA1);
 
    /*
     * ICW1:
     * 0x11 = initialize + ICW4 required
     */
 
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
 
    /*
     * ICW2:
     *
     * Master IRQ0-7 -> 0x20-0x27
     * Slave  IRQ8-15 -> 0x28-0x2F
     */
 
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
 
    /*
     * ICW3:
     *
     * Master IRQ2 has slave connected.
     * Slave is connected to IRQ2.
     */
 
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
 
    /*
     * ICW4:
     * 8086/88 mode
     */
 
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
 
    /*
     * Restore masks first.
     */
 
    outb(0x21, master_mask);
    outb(0xA1, slave_mask);
 
    debug_msg("PIC remapped: IRQ0-7 -> 0x20-0x27");
    debug_msg("PIC remapped: IRQ8-15 -> 0x28-0x2F");
}
 
 
/*
 * ============================================
 * PIC IRQ MASK
 * ============================================
 */
 
static void pic_mask_all(void)
{
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}
 
static void pic_unmask_keyboard_mouse(void)
{
    /*
     * Master PIC:
     *
     * IRQ0 = PIT (системные миллисекунды, см. init_timer(1000))
     * IRQ1 = keyboard
     * IRQ2 = cascade from slave PIC
     */
 
    uint8_t master = inb(0x21);
 
    master &= ~(1 << 0); // IRQ0 PIT timer
    master &= ~(1 << 1); // IRQ1 keyboard
    master &= ~(1 << 2); // IRQ2 cascade
 
    outb(0x21, master);
 
 
    /*
     * Slave PIC:
     *
     * IRQ12 = PS/2 mouse
     *
     * IRQ12 corresponds to bit 4
     * on the slave PIC.
     */
 
    uint8_t slave = inb(0xA1);
 
    slave &= ~(1 << 4); // IRQ12 mouse
 
    outb(0xA1, slave);
 
    debug_msg("PIC: IRQ1 keyboard enabled");
    debug_msg("PIC: IRQ12 mouse enabled");
    debug_msg("PIC: IRQ2 cascade enabled");
}
/*
 * ============================================
 * PIC EOI
 * ============================================
 */
 
void pic_send_eoi(uint8_t irq)
{
    /*
     * IRQ8-15 came through slave PIC,
     * so slave must receive EOI first.
     */
 
    if (irq >= 8)
        outb(0xA0, 0x20);
 
    /*
     * Every IRQ eventually passes through
     * master PIC.
     */
 
    outb(0x20, 0x20);
}
 
 
/*
 * ============================================
 * KEYBOARD POLLING TEST
 * ============================================
 *
 * This is NOT the main test.
 *
 * It is useful to prove that the PS/2 controller
 * itself works independently from interrupts.
 */
 
static void keyboard_poll_test(void)
{
    debug_msg("Keyboard polling test started");
 
    for (;;) {
 
        uint8_t status = inb(0x64);
 
        if (!(status & 1))
            continue;
 
        uint8_t scancode = inb(0x60);
 
        serial_puts("KBD POLL SCANCODE = ");
        debug_hex(scancode);
        serial_puts("\n");
 
        if (ft_ctx) {
            flanterm_write(
                ft_ctx,
                "KEY\r\n",
                5
            );
        }
    }
}
 
 
/*
 * ============================================
 * MAIN
 * ============================================
 */
 
int main(struct multiboot *mboot_ptr)
{
    debug_msg("================================");
    debug_msg("ArtyomXOS PIC-ONLY TEST");
    debug_msg("================================");

    /*
     * ----------------------------------------
     * BUILD MARKER (tcc-exec-fix)
     *
     * Однозначная идентификация сборки ядра.
     * Если этой строки НЕТ в serial-логе при
     * загрузке — работает СТАРОЕ ядро без
     * фикса exec (per-task address spaces),
     * и tcc будет падать с page fault
     * RIP=CR2=0x402C80 (call main).
     * ----------------------------------------
     */
    debug_msg("KERNEL BUILD: tccfix2-20250831 (exec=per-task-AS, exc-handlers=yes)");
 
 
    /*
     * ----------------------------------------
     * FLANTERM
     * ----------------------------------------
     */
 
    if (ft_ctx) {
 
        flanterm_write(
            ft_ctx,
            "ArtyomXOS PIC keyboard test\r\n",
            29
        );

        flanterm_write(
            ft_ctx,
            "kernel build: tccfix2-20250831 (exec per-task AS)\r\n",
            52
        );
 
    } else {
 
        debug_msg("WARNING: ft_ctx == NULL");
    }
 
 
    /*
     * ----------------------------------------
     * INTERRUPTS OFF
     * ----------------------------------------
     */
 
    __asm__ volatile("cli");
 
    debug_msg("Interrupts disabled");
 
 
    /*
     * ----------------------------------------
     * GDT / IDT
     * ----------------------------------------
     */
 
    debug_msg("Initializing descriptor tables");
 
    init_descriptor_tables();
 
    debug_msg("Descriptor tables initialized");

    /*
     * ----------------------------------------
     * CPU EXCEPTIONS (vectors 0..31)
     *
     * Обработчики обязаны стоять ДО запуска любых задач:
     * без них любое исключение в задаче бесконечно
     * повторяется (iret на ту же инструкцию) и система
     * выглядит зависшей.
     * ----------------------------------------
     */

    initialise_exception_handlers();
 
 
    /*
     * ----------------------------------------
     * SYSCALLS
     * ----------------------------------------
     */
 
    initialise_syscalls();

    /*
     * ABI-маршрутизация системных вызовов: int 0x80 для Linux i386
     * и FreeBSD, вектор 0x81 для особого режима Linux x86-64.
     * Перекрывает обработчик 0x80, поставленный initialise_syscalls().
     */
    abi_init();
 
 
    /*
     * ----------------------------------------
     * INITRD
     * ----------------------------------------
     */
 
    boot_stage("stage: PMM");
    pmm_init();
    boot_stage("stage: KHEAP");
    kheap_init();
    boot_stage("stage: VMM");
    vmm_init();
    boot_stage("stage: VMM OK");

    /*if (init_acpi_system()) {
    debug_msg("ACPI OK");
} else {
    debug_msg("WARNING: ACPI initialization failed");
}*/
//enableACPI();
    /*
     * ----------------------------------------
     * INITRD
     * ----------------------------------------
     */
 
    if (mboot_ptr != NULL && mboot_ptr->mods_count > 0) {
 
        u64int initrd_location =
            *((u64int *)(uintptr_t)mboot_ptr->mods_addr);
 
        boot_stage("stage: INITRD");
 
        fs_root =
            initialise_initrd(initrd_location);
 
        boot_stage("stage: INITRD OK");
    } else {
 
        /*
         * r12-bootfix: раньше при mods_count == 0 читали
         * нулевой указатель (mods_addr) -- тройной фолт и
         * "зависшая" машина.  Теперь просто работаем без
         * initrd: exec("wserver") вернёт ошибку и на экране
         * останется текстовая консоль.
         */
        debug_msg("WARNING: no initrd module loaded");
 
        if (ft_ctx) {
            flanterm_write(ft_ctx,
                "WARNING: no initrd module loaded\r\n",
                36);
        }
    }

    /*
     * ----------------------------------------
     * РЕАЛЬНЫЕ ДИСКИ (IDE + AHCI/SATA + NVMe) + АВТОДЕТЕКТ ФС
     * ----------------------------------------
     * blkdev_init() обходит весь PCI (все шины за мостами) и находит
     * накопители: IDE (compat/native), SATA через любое число AHCI-
     * контроллеров и NVMe.  Затем automount_init() разбирает таблицу разделов
     * (MBR с EBR или GPT, иначе superfloppy) на КАЖДОМ диске,
     * определяет тип ФС каждого раздела (FAT12/16/32, ext2/3/4,
     * NTFS) и монтирует их как disk0..diskN.  Если дисков нет,
     * ничего не монтируется и загрузка продолжается.  Список
     * монтирований дублируется в файл "disks.info" в initrd --
     * его читает приложение Files.
     */

    /*
     * ----------------------------------------
     * СЕТЬ (ixn: PCI-NIC -> l2/l3 -> TCP/UDP -> BSD-сокеты)
     * ----------------------------------------
     *
     * ixn_net_init() поднимает драйверы e1000/rtl8139/прочие NIC,
     * слой ARP/IPv4/ICMP, TCP/UDP и TLS.  Сокеты для userspace
     * висят на сисколлах 200..212 (fs/sock.c).
     *
     * DHCP в boot-пути НЕ ждём: конфигурацию делает пользователь
     * (утилита/браузер) через свой запрос -- иначе загрузка без
     * сети висла бы на 15 секунд.
     */
    boot_stage("stage: network stack (ixn)");
    ixn_net_init();
    ax_net_boot_report();

    boot_stage("stage: storage probe (ATA/AHCI/NVMe)");

    blkdev_init();

    boot_stage("stage: legacy ATA init");

    hd_initialize();

    {
        int ndisks = automount_init();

        if (ndisks > 0) {
            boot_stage("stage: automount ok");

            /* disks.info: по строке на диск: "disk0 FAT32". */
            if (fs_root) {
                char info[256];
                int  pos = 0;

                for (int i = 0; i < ndisks && pos < (int)sizeof(info) - 20; i++) {
                    const char *nm = automount_disk_name(i);
                    const char *lb = automount_fs_label(i);

                    for (int k = 0; nm[k]; k++) info[pos++] = nm[k];
                    info[pos++] = ' ';
                    for (int k = 0; lb[k]; k++) info[pos++] = lb[k];
                    info[pos++] = '\n';
                }
                info[pos] = 0;

                fs_node_t *f = initrd_create("disks.info");
                if (f) {
                    write_fs(f, 0, (u32int)pos, (u8int *)info);
                }
            }

            /*
             * Boot-отчёт чтения: первые записи корневого каталога
             * каждого смонтированного диска в COM1.  Проверяет всю
             * вертикаль чтения (VFS -> automount -> драйвер ФС -> ATA)
             * ещё до старта GUI.
             */
            for (int i = 0; i < ndisks; i++) {
                fs_node_t *root = automount_get_root(i);
                if (!root) continue;

                serial_puts("[FS] ");
                serial_puts(automount_disk_name(i));
                serial_puts(" (");
                serial_puts(automount_fs_label(i));
                serial_puts("): ");

                int shown = 0;
                for (u32int k = 0; k < 8; k++) {
                    struct dirent *de = readdir_fs(root, k);
                    if (!de) break;
                    if (shown > 0) serial_puts(", ");
                    serial_puts(de->name);
                    shown++;
                }
                serial_puts(shown ? "\n" : "(empty)\n");
            }

#ifdef FS_WRITE_PROBE
            /*
             * Проверка записи (сборка: make clean && make main.elf
             * CFLAGS_EXTRA="-DFS_WRITE_PROBE").  На каждом доступном
             * для записи диске: truncate -> write -> read-back -> сверка.
             */
            {
                static const char probe_msg[] = "artyom write probe ok";
                char rbuf[64];

                for (int i = 0; i < ndisks; i++) {
                    serial_puts("[PROBE] ");
                    serial_puts(automount_disk_name(i));
                    serial_puts(": ");

                    if (!automount_writeable(i)) {
                        serial_puts("read-only, skipped\n");
                        continue;
                    }

                    fs_node_t *pf = automount_create_at(i, "wprobe.txt");
                    if (!pf) { serial_puts("create FAILED\n"); continue; }

                    if (automount_truncate_at(i, pf) != 0) {
                        serial_puts("truncate FAILED\n");
                        continue;
                    }

                    u32int w = write_fs(pf, 0,
                        (u32int)(sizeof(probe_msg) - 1), (u8int *)probe_msg);

                    int rb = (int)read_fs(pf, 0, (u32int)sizeof(rbuf) - 1,
                                          (u8int *)rbuf);
                    int okw = (w == sizeof(probe_msg) - 1) &&
                              (rb == (int)(sizeof(probe_msg) - 1));
                    if (okw) {
                        for (int c = 0; c < (int)(sizeof(probe_msg) - 1); c++) {
                            if (rbuf[c] != probe_msg[c]) { okw = 0; break; }
                        }
                    }

                    serial_puts(okw ? "write+readback OK\n"
                                    : "write/readback MISMATCH\n");
                }
            }
#endif
        } else {
            boot_stage("stage: no mountable fs -- continuing");
        }

        /*
         * ОТЧЁТ О ХРАНИЛИЩЕ (для машин без COM-порта):
         *   - краткая сводка на экран;
         *   - полный лог (PCI, ATA/AHCI/NVMe, разделы) в файл "storage.log"
         *     в корне initrd: из терминала `cat storage.log`;
         *   - если что-то не определилось -- пауза, чтобы успеть прочесть.
         */
        {
            blkdev_report(boot_stage);

            if (fs_root) {
                uint32_t llen = 0;
                const char *ldata = debug_log_data(&llen);
                fs_node_t *lf = initrd_create("storage.log");
                if (lf && llen) {
                    u32int w = write_fs(lf, 0, (u32int)llen, (u8int *)ldata);
                    if (w == (u32int)llen) {
                        boot_stage("storage log: storage.log");
                    } else {
                        boot_stage("storage log: write FAILED (short)");
                    }
                    serial_puts("[LOGFILE] storage.log bytes=");
                    debug_putnum((uint64_t)w);
                    serial_puts(" of ");
                    debug_putnum((uint64_t)llen);
                    serial_puts("\n");
                }
            }

#ifdef FS_TREE_SELFTEST
            { extern void automount_selftest(void); automount_selftest(); }
#endif
            if (ndisks == 0 || ndisks < blkdev_count() || blkdev_count() == 0) {
                boot_stage("storage: problem detected -- see storage.log");
                pcix_mdelay(8000);
            }
        }
    }
 
 
    /*
     * ----------------------------------------
     * CONSOLE / PTY
     * ----------------------------------------
 */
 
    console_install();
 
    pty_subsystem_init();
 
    boot_stage("stage: console + PTY");
 
 
    /*
     * ========================================
     * PURE LEGACY PIC
     * ========================================
 */
 
    debug_msg("--------------------------------");
    debug_msg("INITIALIZING LEGACY PIC");
    debug_msg("--------------------------------");
 
 
    /*
     * Make absolutely sure interrupts remain
     * disabled during PIC setup.
     */
 
    __asm__ volatile("cli");
 
    boot_stage("stage: PIC setup");
 
 
    /*
     * Mask everything before reconfiguration.
     */
 
    pic_mask_all();
 
    debug_msg("PIC: all IRQs masked");
 
 
    /*
     * Remap PIC vectors.
     */
 
    pic_remap();
 
 
    /*
     * IMPORTANT:
     *
     * Only IRQ1 is enabled.
     *
     * IRQ0 timer remains disabled.
     * IRQ2-15 remain disabled.
     */
 
   /*
 * ========================================
 * PS/2 DEVICES
 * ========================================
 */
 
debug_msg("--------------------------------");
debug_msg("INITIALIZING PS/2 DEVICES");
debug_msg("--------------------------------");
 
boot_stage("stage: PS/2 init (kbd + mouse)");
 
__asm__ volatile("cli");
 
 
/*
 * Keyboard:
 *
 * IRQ1 -> INT 0x21
 */
 
init_keyboard(0);
 
debug_msg("Keyboard initialized");
 
 
/*
 * Mouse:
 *
 * IRQ12 -> INT 0x2C
 */
 
mouse_install();
 
debug_msg("Mouse initialized");
 

/*
 * ========================================
 * ENABLE PIC IRQs
 * ========================================
 */
 
pic_unmask_keyboard_mouse();

/*
 * PIT: 1000 Гц -> tick == 1 мс.  Даёт системе настоящий
 * монотонный таймер (сисколл TICKS_MS = 196), который нужен
 * играм (DOOM) для игрового цикла.  IRQ0 уже размаскирован
 * в pic_unmask_keyboard_mouse().
 */
init_timer(1000);
debug_msg("PIT: 1000 Hz timer enabled (IRQ0)");
 
 
/*
 * ========================================
 * INTERRUPTS
 * ========================================
 */
 
debug_msg("--------------------------------");
debug_msg("ENABLING INTERRUPTS");
debug_msg("--------------------------------");
 
__asm__ volatile("sti");
 
debug_msg("STI executed");
 
boot_stage("stage: IRQ enabled");
 
 
if (ft_ctx) {
 
    flanterm_write(
        ft_ctx,
        "PIC READY - keyboard + mouse\r\n",
        31
    );
}
 
 
serial_puts("\n");
serial_puts("================================\n");
serial_puts("PIC PS/2 TEST READY\n");
serial_puts("IRQ1  -> INT 0x21 = KEYBOARD\n");
serial_puts("IRQ12 -> INT 0x2C = MOUSE\n");
serial_puts("================================\n");
 
 
/*
 * ========================================
 * PURE INTERRUPT TEST
 * ========================================
 */
boot_stage("stage: xHCI (USB 3) init");
xhci_init();
 
boot_stage("stage: tasking init");
initTasking();
 
boot_stage("stage: launching wserver (GUI)...");
 
/*
 * ---- TEMPORARY smoke test for the new ring3 exec() path ----
 * (verifies GDT/TSS/VMM/loader/task_spawn_user before wiring the
 * rest of the system; removed once confirmed working)
 */
/*{
    extern int exec(const char *path);
    fs_node_t *__con = finddir_fs(fs_root, "console");
    if (__con) task_set_ctty(runningTask->pid, __con);
    serial_puts("SMOKETEST: calling exec(hello)\n");
    int __pid = exec("hi");
    serial_puts("SMOKETEST: exec() returned pid=");
    char __b[16]; int_to_str(__pid, __b, sizeof(__b)); serial_puts(__b);
    serial_puts("\n");
    for (int __i = 0; __i < 20 && task_is_alive(__pid); __i++) {
        yield();
    }
    serial_puts("SMOKETEST: done\n");
}*/
 
/*
 * ========================================
 * WSERVER: ЗАПУСК ОКОННОГО СЕРВЕРА
 * ========================================
 *
 * Модель NeXTSTEP / MacOS X: GUI живёт в ОТДЕЛЬНОМ процессе
 * (wserver -- аналог WindowServer).  Ядро только загружает его
 * ELF из initrd тем же exec(), каким приложения запускают друг
 * друга.  Дальше wserver:
 *
 *   - создаёт именованный порт "wserver" (аналог Mach bootstrap);
 *   - берёт framebuffer через FB_INFO и рисует сам;
 *   - принимает сообщения клиентов (ax_surface, commit, events...);
 *   - запускает приложения из дока через exec().
 *
 * Если wserver нет в initrd -- на экране остаётся текстовая
 * консоль Limine, ядро продолжает работать.
 * ========================================
 */
{
    extern int exec(const char *path, int argc, char *argv[]);

    ax_net_dhcp_try(4000u);
    ax_net_boot_report();
    ax_mm_report("boot");
    ax_selftest_run();
    ax_mm_report("selftest");

    serial_puts("BOOT: spawning wserver (GUI server)\n");

    int wspid = exec("wserver", 0, 0);

    if (wspid < 0) {
        serial_puts("BOOT: wserver not found in initrd -- no GUI\n");
        if (ft_ctx) {
            flanterm_write(ft_ctx,
                "wserver not found in initrd (GUI disabled)\r\n",
                45);
        }
    } else {
        serial_puts("BOOT: wserver spawned pid=");
        debug_putnum((uint64_t)wspid);
        serial_puts("\n");
    }

}

/*
 * Root-задача (pid 1) теперь просто спит: она больше не
 * композитор.  Планировщик кооперативный -- отдаём кванты.
 */
for (;;) {
    xhci_deferred();   /* USB hot-plug / recovery */
    yield();
}
 
return 0;
}
