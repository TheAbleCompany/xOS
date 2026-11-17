#include "../../lib/common.h"
#include "descriptor_tables.h"
#include "../isr.h"
 
extern void gdt_flush(u64int);
extern void idt_flush(u64int);
extern void tss_flush(u64int);
 
static void init_gdt(void);
static void init_idt(void);
static void gdt_set_gate(s32int, u32int, u32int, u8int, u8int);
static void idt_set_gate(u8int, u64int, u16int, u8int);
static void gdt_set_tss(int num, u64int base, u32int limit, u8int access);
 
/*
 * 9 обычных дескрипторов (как раньше) + один 64-битный TSS
 * дескриптор, который в силу своего 16-байтного размера занимает
 * ДВА слота по 8 байт (индексы 9 и 10).
 */
gdt_entry_t gdt_entries[11];
gdt_ptr_t   gdt_ptr;
idt_entry_t idt_entries[256];
idt_ptr_t   idt_ptr;
 
static tss_t kernel_tss;
 
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
     * TSS -- нужен исключительно ради аппаратной смены стека
     * ring3 -> ring0 (RSP0). iomap_base указывает за пределы
     * лимита сегмента, что равносильно отсутствию I/O bitmap
     * (userspace-приложениям порты ввода-вывода недоступны).
     */
    memset((u8int *)&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.iomap_base = sizeof(tss_t);
 
    gdt_set_tss(9, (u64int)&kernel_tss, sizeof(tss_t) - 1, 0x89);
 
    gdt_flush((u64int)&gdt_ptr);
 
    tss_flush(GDT_TSS);
}
 
static void gdt_set_tss(int num, u64int base, u32int limit, u8int access)
{
    /*
     * Системный дескриптор в 64-битном режиме занимает 16 байт,
     * то есть два соседних слота gdt_entry_t (num и num+1).
     * Компонуем его вручную побайтово -- gdt_entry_t/gdt_set_gate
     * рассчитаны только на 32-битный base.
     */
    u8int *d = (u8int *)&gdt_entries[num];
 
    d[0] = limit & 0xFF;
    d[1] = (limit >> 8) & 0xFF;
 
    d[2] = base & 0xFF;
    d[3] = (base >> 8) & 0xFF;
    d[4] = (base >> 16) & 0xFF;
 
    d[5] = access;
 
    d[6] = ((limit >> 16) & 0x0F);   /* G=0, AVL=0 для маленького TSS */
    d[7] = (base >> 24) & 0xFF;
 
    d[8]  = (base >> 32) & 0xFF;
    d[9]  = (base >> 40) & 0xFF;
    d[10] = (base >> 48) & 0xFF;
    d[11] = (base >> 56) & 0xFF;
 
    d[12] = 0;
    d[13] = 0;
    d[14] = 0;
    d[15] = 0;
}
 
void tss_set_kernel_stack(u64int rsp0)
{
    kernel_tss.rsp0 = rsp0;
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
 
    idt_flush((u64int)&idt_ptr);
}
 
static void idt_set_gate(u8int num, u64int base, u16int sel, u8int flags)
{
    idt_entries[num].base_low  = base & 0xFFFF;
    idt_entries[num].base_mid  = (base >> 16) & 0xFFFF;
    idt_entries[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt_entries[num].sel       = sel;
    idt_entries[num].ist       = 0;
    idt_entries[num].flags     = flags;
    idt_entries[num].zero      = 0;
}
