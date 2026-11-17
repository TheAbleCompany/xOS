#include "../lib/common.h"
#include "descriptor_tables.h"
#include "isr.h"

extern void gdt_flush(u64int);
extern void idt_flush(u64int);

static void init_gdt(void);
static void init_idt(void);
static void gdt_set_gate(s32int, u32int, u32int, u8int, u8int);
static void tss_set_gate(s32int, u64int, u32int, u8int);
static void idt_set_gate(u8int, u64int, u16int, u8int);

/*
 * GDT: 9 классических дескрипторов + 16-байтный дескриптор TSS
 * (занимает ДВА слота, индексы 9 и 10, селектор 0x48).
 */
gdt_entry_t gdt_entries[11];
gdt_ptr_t   gdt_ptr;
idt_entry_t idt_entries[256];
idt_ptr_t   idt_ptr;

/*
 * ============================================================
 * TSS + IST (Interrupt Stack Table)
 * ============================================================
 *
 * ЗАЧЕМ: все задачи у нас исполняются в ring 0, поэтому кадр
 * прерывания/сисколла CPU вытолкивает на ТЕКУЩИЙ стек задачи.
 * Пользовательские бинарники собраны TCC, который кладёт мелкие
 * локальные переменные в red zone ([rsp-1 .. rsp-128]) и НЕ
 * резервирует под них стек. Кадр int 0x80 (RIP/CS/RFLAGS/RSP/SS)
 * ложится ПОВЕРХ этих данных, а запись результата сисколла
 * (out-параметр, напр. ax_fb_info_t в gfx_init) затирала слоты
 * сохранённого кадра: width=1280 (0x500) попадала в CS ->
 * iretq -> #GP(0x500) ("wserver крашнулся сразу после
 * WSERVER_START").
 *
 * РЕШЕНИЕ (как в Linux/macOS): все векторы получают ist=1 -- CPU
 * кладёт кадр на ОТДЕЛЬНЫЙ ядерный стек IST1, хендлеры тоже
 * работают на нём. Стек задачи прерываниями не трогается вообще,
 * red zone пользователей неприкосновенна. #DF получает свой IST2.
 *
 * Стеки лежат в .bss ядра: верхняя половина адресов копируется в
 * КАЖДОЕ адресное пространство (vmm_new_address_space), поэтому
 * IST-стек валиден и когда CR3 указывает на пространство wserver.
 */
typedef struct {
    u32int reserved0;
    u64int rsp0, rsp1, rsp2;
    u64int ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64int reserved1;
    u16int reserved2;
    u16int iomap_base;
    u64int reserved3;          /* добиваем до 104 байт (limit >= 0x67) */
} __attribute__((packed)) tss_entry_t;

#define GDT_TSS_SEL   0x48
#define IST1_SIZE     (64 * 1024)   /* кадры + хендлеры + сисколлы  */
#define IST2_SIZE     (16 * 1024)   /* отдельный стек для #DF        */

static tss_entry_t kernel_tss;
static u8int ist1_stack[IST1_SIZE] __attribute__((aligned(16)));
static u8int ist2_stack[IST2_SIZE] __attribute__((aligned(16)));

extern isr_t interrupt_handlers[];

void init_descriptor_tables(void)
{
    init_gdt();
    init_idt();
    memset((u8int*)&interrupt_handlers, 0, sizeof(isr_t)*256);

    outb(0x21, 0xF9);
    outb(0xA1, 0xEF);
}

static void init_gdt(void)
{
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 11) - 1;
    gdt_ptr.base  = (u64int)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0, 0x9A, 0xA0);
    gdt_set_gate(2, 0, 0, 0x92, 0xA0);
    gdt_set_gate(3, 0, 0, 0xFA, 0xA0);
    gdt_set_gate(4, 0, 0, 0xF2, 0xA0);
    gdt_set_gate(5, 0, 0, 0x9A, 0x80);
    gdt_set_gate(6, 0, 0, 0x92, 0x80);
    gdt_set_gate(7, 0, 0, 0x9A, 0x00);
    gdt_set_gate(8, 0, 0, 0x92, 0x00);

    /*
     * TSS нужен только ради IST: задачи ring 0, переключение
     * контекста -- программное (switch.S), RSP0 не используется.
     */
    memset((u8int *)&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ist1 = (u64int)(ist1_stack + IST1_SIZE);
    kernel_tss.ist2 = (u64int)(ist2_stack + IST2_SIZE);
    kernel_tss.iomap_base = sizeof(kernel_tss);

    tss_set_gate(9, (u64int)&kernel_tss, sizeof(kernel_tss) - 1, 0x89);

    gdt_flush((u64int)&gdt_ptr);

    /* Загружаем TR селектором TSS (0x48). */
    __asm__ volatile(
        "mov $0x48, %%ax\n"
        "ltr %%ax"
        :
        :
        : "ax", "memory"
    );
}

/*
 * 64-битный дескриптор TSS занимает 16 байт -- ДВА слота GDT
 * (индексы num и num+1). Базовый адрес -- полный 64-битный.
 */
static void tss_set_gate(s32int num, u64int base, u32int limit, u8int access)
{
    u64int lo = 0;
    u64int hi = 0;

    lo |= (u64int)(limit & 0xFFFF);              /* limit  0-15  */
    lo |= ((u64int)base & 0xFFFFFFULL) << 16;    /* base   0-23  */
    lo |= ((u64int)access) << 40;                /* P|DPL|type   */
    lo |= ((u64int)((limit >> 16) & 0xF)) << 48; /* limit 16-19  */
    lo |= ((u64int)((base >> 24) & 0xFF)) << 56; /* base  24-31  */

    hi = (u64int)((base >> 32) & 0xFFFFFFFFULL); /* base  32-63  */

    *(u64int *)&gdt_entries[num]     = lo;
    *(u64int *)&gdt_entries[num + 1] = hi;
}

static void gdt_set_gate(s32int num, u32int base, u32int limit, u8int access, u8int gran)
{
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

static void init_idt(void)
{
    idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
    idt_ptr.base  = (u64int)&idt_entries;

    memset((u8int*)&idt_entries, 0, sizeof(idt_entry_t)*256);

    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    idt_set_gate( 0, (u64int)isr0 , 0x08, 0x8E);
    idt_set_gate( 1, (u64int)isr1 , 0x08, 0x8E);
    idt_set_gate( 2, (u64int)isr2 , 0x08, 0x8E);
    idt_set_gate( 3, (u64int)isr3 , 0x08, 0x8E);
    idt_set_gate( 4, (u64int)isr4 , 0x08, 0x8E);
    idt_set_gate( 5, (u64int)isr5 , 0x08, 0x8E);
    idt_set_gate( 6, (u64int)isr6 , 0x08, 0x8E);
    idt_set_gate( 7, (u64int)isr7 , 0x08, 0x8E);
    idt_set_gate( 8, (u64int)isr8 , 0x08, 0x8E);
    idt_set_gate( 9, (u64int)isr9 , 0x08, 0x8E);
    idt_set_gate(10, (u64int)isr10, 0x08, 0x8E);
    idt_set_gate(11, (u64int)isr11, 0x08, 0x8E);
    idt_set_gate(12, (u64int)isr12, 0x08, 0x8E);
    idt_set_gate(13, (u64int)isr13, 0x08, 0x8E);
    idt_set_gate(14, (u64int)isr14, 0x08, 0x8E);
    idt_set_gate(15, (u64int)isr15, 0x08, 0x8E);
    idt_set_gate(16, (u64int)isr16, 0x08, 0x8E);
    idt_set_gate(17, (u64int)isr17, 0x08, 0x8E);
    idt_set_gate(18, (u64int)isr18, 0x08, 0x8E);
    idt_set_gate(19, (u64int)isr19, 0x08, 0x8E);
    idt_set_gate(20, (u64int)isr20, 0x08, 0x8E);
    idt_set_gate(21, (u64int)isr21, 0x08, 0x8E);
    idt_set_gate(22, (u64int)isr22, 0x08, 0x8E);
    idt_set_gate(23, (u64int)isr23, 0x08, 0x8E);
    idt_set_gate(24, (u64int)isr24, 0x08, 0x8E);
    idt_set_gate(25, (u64int)isr25, 0x08, 0x8E);
    idt_set_gate(26, (u64int)isr26, 0x08, 0x8E);
    idt_set_gate(27, (u64int)isr27, 0x08, 0x8E);
    idt_set_gate(28, (u64int)isr28, 0x08, 0x8E);
    idt_set_gate(29, (u64int)isr29, 0x08, 0x8E);
    idt_set_gate(30, (u64int)isr30, 0x08, 0x8E);
    idt_set_gate(31, (u64int)isr31, 0x08, 0x8E);
    idt_set_gate(32, (u64int)irq0, 0x08, 0x8E);
    idt_set_gate(33, (u64int)irq1, 0x08, 0x8E);
    idt_set_gate(34, (u64int)irq2, 0x08, 0x8E);
    idt_set_gate(35, (u64int)irq3, 0x08, 0x8E);
    idt_set_gate(36, (u64int)irq4, 0x08, 0x8E);
    idt_set_gate(37, (u64int)irq5, 0x08, 0x8E);
    idt_set_gate(38, (u64int)irq6, 0x08, 0x8E);
    idt_set_gate(39, (u64int)irq7, 0x08, 0x8E);
    idt_set_gate(40, (u64int)irq8, 0x08, 0x8E);
    idt_set_gate(41, (u64int)irq9, 0x08, 0x8E);
    idt_set_gate(42, (u64int)irq10, 0x08, 0x8E);
    idt_set_gate(43, (u64int)irq11, 0x08, 0x8E);
    idt_set_gate(44, (u64int)irq12, 0x08, 0x8E);
    idt_set_gate(45, (u64int)irq13, 0x08, 0x8E);
    idt_set_gate(46, (u64int)irq14, 0x08, 0x8E);
    idt_set_gate(47, (u64int)irq15, 0x08, 0x8E);
    idt_set_gate(128, (u64int)isr128, 0x08, 0xEE);
    idt_set_gate(129, (u64int)isr129, 0x08, 0xEE);
    idt_set_gate(130, (u64int)isr130, 0x08, 0xEE);
    idt_set_gate(131, (u64int)isr131, 0x08, 0xEE);

    idt_flush((u64int)&idt_ptr);
}

static void idt_set_gate(u8int num, u64int base, u16int sel, u8int flags)
{
    idt_entries[num].base_low  = base & 0xFFFF;
    idt_entries[num].base_mid  = (base >> 16) & 0xFFFF;
    idt_entries[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt_entries[num].sel       = sel;
    /*
     * IRQ/исключения (0-47) -- на IST1: кадр прерывания не должен
     * попадать на стек задачи (TCC-red-zone, см. комментарий выше).
     * Double fault (#DF, вектор 8) -- на отдельный IST2.
     *
     * Векторы 128-130 (сисколлы) пока остаются на стеке задачи:
     * хендлеры сисколлов могут уходить в yield()/switchTask,
     * и их контекст обязан жить на стеке задачи. Кадр int 0x80
     * чинится отдельно (реплика кадра в interrupt.asm).
     */
    if (num == 8)
        idt_entries[num].ist = 2;
    else if (num >= 128)
        idt_entries[num].ist = 0;
    else
        idt_entries[num].ist = 1;
    idt_entries[num].flags     = flags;
    idt_entries[num].zero      = 0;
}
