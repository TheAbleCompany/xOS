#include "../drivers/monitor.h"
#include "elf.h"
#include "../lib/common.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define malloc kmalloc
#define free   kfree

extern void serial_puts_ax(const char *s);
extern void serial_putc_ax(char c);

void *memmove(void *dst, const void *src, unsigned long n);

/*
 * ------------------------------------------------------------
 * ELF constants
 *
 * Некоторые из них отсутствуют в нашем elf.h.
 * ELF64_R_* уже определены в elf.h, поэтому НЕ переопределяем.
 * ------------------------------------------------------------
 */

#ifndef EI_DATA
#define EI_DATA 5
#endif

#ifndef ELFDATA2LSB
#define ELFDATA2LSB 1
#endif

#ifndef EM_X86_64
#define EM_X86_64 62
#endif

#ifndef SHN_UNDEF
#define SHN_UNDEF 0
#endif

#ifndef ET_EXEC
#define ET_EXEC 2
#endif

#ifndef ET_DYN
#define ET_DYN 3
#endif

#ifndef PT_LOAD
#define PT_LOAD 1
#endif

#ifndef PT_DYNAMIC
#define PT_DYNAMIC 2
#endif

#ifndef PT_INTERP
#define PT_INTERP 3
#endif

#ifndef PT_PHDR
#define PT_PHDR 6
#endif

#ifndef PF_X
#define PF_X 1
#endif

#ifndef PF_W
#define PF_W 2
#endif

#ifndef PF_R
#define PF_R 4
#endif

#ifndef SHT_RELA
#define SHT_RELA 4
#endif

#ifndef SHT_DYNSYM
#define SHT_DYNSYM 11
#endif

#ifndef SHT_SYMTAB
#define SHT_SYMTAB 2
#endif

#ifndef DT_NULL
#define DT_NULL 0
#endif

#ifndef DT_NEEDED
#define DT_NEEDED 1
#endif

#ifndef DT_PLTRELSZ
#define DT_PLTRELSZ 2
#endif

#ifndef DT_PLTGOT
#define DT_PLTGOT 3
#endif

#ifndef DT_HASH
#define DT_HASH 4
#endif

#ifndef DT_STRTAB
#define DT_STRTAB 5
#endif

#ifndef DT_SYMTAB
#define DT_SYMTAB 6
#endif

#ifndef DT_RELA
#define DT_RELA 7
#endif

#ifndef DT_RELASZ
#define DT_RELASZ 8
#endif

#ifndef DT_RELAENT
#define DT_RELAENT 9
#endif

#ifndef DT_STRSZ
#define DT_STRSZ 10
#endif

#ifndef DT_SYMENT
#define DT_SYMENT 11
#endif

#ifndef DT_INIT
#define DT_INIT 12
#endif

#ifndef DT_FINI
#define DT_FINI 13
#endif

#ifndef DT_JMPREL
#define DT_JMPREL 23
#endif

#ifndef DT_PLTREL
#define DT_PLTREL 20
#endif

#ifndef DT_PLTREL
#define DT_PLTREL 20
#endif

/*
 * ------------------------------------------------------------
 * Relocation types
 *
 * ELF64_R_SYM / ELF64_R_TYPE находятся в elf.h.
 * ------------------------------------------------------------
 */

#ifndef R_X86_64_NONE
#define R_X86_64_NONE       0
#endif

#ifndef R_X86_64_64
#define R_X86_64_64         1
#endif

#ifndef R_X86_64_PC32
#define R_X86_64_PC32       2
#endif

#ifndef R_X86_64_GLOB_DAT
#define R_X86_64_GLOB_DAT   6
#endif

#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT  7
#endif

#ifndef R_X86_64_RELATIVE
#define R_X86_64_RELATIVE   8
#endif

#ifndef R_X86_64_32
#define R_X86_64_32         10
#endif

#ifndef R_X86_64_32S
#define R_X86_64_32S        11
#endif


/*
 * ------------------------------------------------------------
 * Debug helpers
 * ------------------------------------------------------------
 */

static void print_hex64(uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';

    for (int i = 0; i < 16; i++) {
        int shift = 60 - i * 4;
        buf[2 + i] = hex[(value >> shift) & 0xF];
    }

    buf[18] = 0;

    serial_puts_ax(buf);
}

static void print_dec_u64(uint64_t value)
{
    char buf[32];
    int n = 0;

    if (value == 0) {
        serial_putc_ax('0');
        return;
    }

    while (value > 0) {
        buf[n++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (n > 0)
        serial_putc_ax(buf[--n]);
}


/*
 * ------------------------------------------------------------
 * ELF validation
 * ------------------------------------------------------------
 */

static int elf_valid64(Elf64_Ehdr *hdr)
{
    if (!hdr)
        return 0;

    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E'  ||
        hdr->e_ident[2] != 'L'  ||
        hdr->e_ident[3] != 'F')
        return 0;

    if (hdr->e_ident[4] != ELFCLASS64)
        return 0;

    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return 0;

    if (hdr->e_machine != EM_X86_64)
        return 0;

    if (hdr->e_type != ET_EXEC &&
        hdr->e_type != ET_DYN)
        return 0;

    return 1;
}


/*
 * ------------------------------------------------------------
 * Range helpers
 * ------------------------------------------------------------
 */

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) &
           ~(alignment - 1);
}


/*
 * ------------------------------------------------------------
 * Find PT_LOAD range
 * ------------------------------------------------------------
 */

static int get_load_range(
    Elf64_Ehdr *hdr,
    Elf64_Phdr *phdr,
    uint64_t *min_vaddr,
    uint64_t *max_vaddr
)
{
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    int found = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {

        if (phdr[i].p_type != PT_LOAD)
            continue;

        if (phdr[i].p_filesz > phdr[i].p_memsz)
            return 0;

        if (phdr[i].p_offset + phdr[i].p_filesz == 0)
            return 0;

        if (phdr[i].p_vaddr < min)
            min = phdr[i].p_vaddr;

        if (phdr[i].p_vaddr + phdr[i].p_memsz > max)
            max = phdr[i].p_vaddr +
                  phdr[i].p_memsz;

        found = 1;
    }

    if (!found)
        return 0;

    *min_vaddr = align_down(min, 0x1000);
    *max_vaddr = align_up(max, 0x1000);

    return 1;
}


/*
 * ------------------------------------------------------------
 * Load PT_LOAD segments
 * ------------------------------------------------------------
 */

static int load_segments(
    Elf64_Ehdr *hdr,
    Elf64_Phdr *phdr,
    const uint8_t *image,
    uint8_t *memory,
    uint64_t load_base
)
{
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {

        Elf64_Phdr *p = &phdr[i];

        if (p->p_type != PT_LOAD)
            continue;

        if (p->p_filesz > p->p_memsz)
            return 0;

        uint64_t offset =
            p->p_vaddr - load_base;

        uint8_t *dst =
            memory + offset;

        const uint8_t *src =
            image + p->p_offset;

        /*
         * .text/.rodata/.data
         */
        if (p->p_filesz != 0) {
            memmove(
                dst,
                src,
                p->p_filesz
            );
        }

        /*
         * .bss
         */
        if (p->p_memsz > p->p_filesz) {

            memset(
                dst + p->p_filesz,
                0,
                p->p_memsz - p->p_filesz
            );
        }

        serial_puts_ax("LOAD segment: vaddr=");
        print_hex64(p->p_vaddr);

        serial_puts_ax(" filesz=");
        print_hex64(p->p_filesz);

        serial_puts_ax(" memsz=");
        print_hex64(p->p_memsz);

        serial_putc_ax('\n');
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * Find dynamic section
 * ------------------------------------------------------------
 */

static Elf64_Dyn *find_dynamic(
    Elf64_Ehdr *hdr,
    Elf64_Phdr *phdr,
    const uint8_t *image
)
{
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {

        if (phdr[i].p_type != PT_DYNAMIC)
            continue;

        return (Elf64_Dyn *)
            (image + phdr[i].p_offset);
    }

    return NULL;
}


/*
 * ------------------------------------------------------------
 * Find dynamic tag
 * ------------------------------------------------------------
 */

static uint64_t dynamic_value(
    Elf64_Dyn *dyn,
    int64_t tag
)
{
    if (!dyn)
        return 0;

    for (;;) {

        if (dyn->d_tag == DT_NULL)
            break;

        if (dyn->d_tag == tag)
            return dyn->d_un.d_val;

        dyn++;
    }

    return 0;
}


/*
 * ------------------------------------------------------------
 * Convert virtual ELF address to loaded address
 * ------------------------------------------------------------
 */

static void *loaded_address(
    uint8_t *memory,
    uint64_t load_base,
    uint64_t address
)
{
    return memory + (address - load_base);
}


/*
 * ------------------------------------------------------------
 * Process RELA
 * ------------------------------------------------------------
 */

static int process_rela(
    Elf64_Rela *rela,
    uint64_t count,
    Elf64_Sym *symtab,
    const char *strtab,
    uint8_t *memory,
    uint64_t load_base
)
{
    /*
     * "Base delta" (B в терминах ELF gABI) — разница между
     * фактическим адресом загрузки образа и ELF'овым min_vaddr.
     *
     * Для PIE (min_vaddr == 0), загруженного по адресу memory:
     *     base_delta = (uint64_t)memory
     *
     * Именно это значение прибавляется к addend'ам R_X86_64_RELATIVE
     * и к st_value символов в абсолютных reloc-формах.
     *
     * РАНЬШЕ использовался load_base (= min_vaddr = 0 для PIE),
     * relocation не делал ничего — приложение падало на первой
     * абсолютной ссылке.
     */
    /*
     * Образ отображается task_spawn() по ЛИНКОВОЧНОМУ адресу
     * [min_vaddr, max_vaddr), поэтому реальная база загрузки в
     * адресном пространстве задачи == load_base, и delta == 0.
     * Раньше здесь стоял (memory - load_base), где memory -- HHDM-алиас:
     * RELATIVE-релокации записывали в данные HHDM-адреса, и после
     * первого вызова через указатель код исполнялся по алиасу
     * (адреса глобалов "плавали", списки зацикливались).
     */
    uint64_t base_delta = 0;
    (void)memory;

    for (uint64_t i = 0; i < count; i++) {

        uint64_t type =
            ELF64_R_TYPE(rela[i].r_info);

        uint64_t sym_index =
            ELF64_R_SYM(rela[i].r_info);

        uint64_t *where =
            (uint64_t *)loaded_address(
                memory, load_base, rela[i].r_offset);

        uint64_t S = 0;

        if (sym_index != 0 && symtab) {

            Elf64_Sym *sym = &symtab[sym_index];

            if (sym->st_shndx != SHN_UNDEF) {

                /*
                 * Фактический адрес символа в загруженном образе.
                 * loaded_address(memory, load_base, sym->st_value)
                 *   == memory + (sym->st_value - load_base)
                 *   == base_delta + sym->st_value
                 */
                S = (uint64_t)loaded_address(
                    memory, load_base, sym->st_value);

            } else {

                const char *name =
                    strtab ? strtab + sym->st_name : NULL;

                serial_puts_ax("UNRESOLVED SYMBOL: ");
                if (name) serial_puts_ax(name);
                serial_putc_ax('\n');

                return 0;
            }
        }

        int64_t A = rela[i].r_addend;
        uint64_t P = (uint64_t)where;

        switch (type) {

        case R_X86_64_NONE:
            break;

        /* S + A */
        case R_X86_64_64:
            *where = S + A;
            break;

        /* B + A — B = base_delta */
        case R_X86_64_RELATIVE:
            *where = base_delta + A;
            break;

        /* S + A - P — PC-relative, memory сокращается */
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            *(uint32_t *)where =
                (uint32_t)((S + A) - P);
            break;

        /* S */
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = S;
            break;

        /* 32-bit absolute */
        case R_X86_64_32:
            *(uint32_t *)where =
                (uint32_t)(S + A);
            break;

        /* signed 32-bit absolute */
        case R_X86_64_32S:
            *(int32_t *)where =
                (int32_t)(S + A);
            break;

        default:
            serial_puts_ax("UNSUPPORTED RELOCATION: ");
            print_hex64(type);
            serial_putc_ax('\n');
            return 0;
        }
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * Process dynamic relocations
 * ------------------------------------------------------------
 */

static int process_dynamic(
    Elf64_Ehdr *hdr,
    Elf64_Phdr *phdr,
    const uint8_t *image,
    uint8_t *memory,
    uint64_t load_base
)
{
    Elf64_Dyn *dyn =
        find_dynamic(
            hdr,
            phdr,
            image
        );

    /*
     * Нет PT_DYNAMIC.
     *
     * Это нормально для полностью
     * статически слинкованного ELF.
     */
    if (!dyn)
        return 1;

    uint64_t rela_addr =
        dynamic_value(
            dyn,
            DT_RELA
        );

    uint64_t rela_size =
        dynamic_value(
            dyn,
            DT_RELASZ
        );

    uint64_t rela_ent =
        dynamic_value(
            dyn,
            DT_RELAENT
        );

    uint64_t symtab_addr =
        dynamic_value(
            dyn,
            DT_SYMTAB
        );

    uint64_t strtab_addr =
        dynamic_value(
            dyn,
            DT_STRTAB
        );

    Elf64_Rela *rela = NULL;
    Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;

    if (rela_addr) {

        rela =
            (Elf64_Rela *)
            loaded_address(
                memory,
                load_base,
                rela_addr
            );
    }

    if (symtab_addr) {

        symtab =
            (Elf64_Sym *)
            loaded_address(
                memory,
                load_base,
                symtab_addr
            );
    }

    if (strtab_addr) {

        strtab =
            (const char *)
            loaded_address(
                memory,
                load_base,
                strtab_addr
            );
    }

    if (rela && rela_size) {

        if (rela_ent == 0)
            rela_ent =
                sizeof(Elf64_Rela);

        uint64_t count =
            rela_size / rela_ent;

        serial_puts_ax(
            "DYNAMIC RELA count="
        );

        print_dec_u64(count);
        serial_putc_ax('\n');

        if (!process_rela(
                rela,
                count,
                symtab,
                strtab,
                memory,
                load_base))
        {
            return 0;
        }
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * Pending image state
 *
 * Заполняется image_load64(), потребляется task_spawn()
 * (aximage_pending_pop). Ядро однопоточное/кооперативное,
 * между image_load() и task_spawn() планирование невозможно,
 * поэтому единого набора статических переменных достаточно.
 * ------------------------------------------------------------
 */

static int      ax_pending_valid = 0;
static uint64_t ax_pending_lo    = 0;
static uint64_t ax_pending_hi    = 0;
static uint64_t ax_pending_phys  = 0;
static int      ax_pending_abi   = 0;

int aximage_pending_pop(
    uint64_t *lo,
    uint64_t *hi,
    uint64_t *phys
)
{
    if (!ax_pending_valid)
        return 0;

    if (lo)
        *lo = ax_pending_lo;
    if (hi)
        *hi = ax_pending_hi;
    if (phys)
        *phys = ax_pending_phys;

    ax_pending_valid = 0;
    ax_pending_lo    = 0;
    ax_pending_hi    = 0;
    ax_pending_phys  = 0;
    ax_pending_abi   = 0;

    return 1;
}

void aximage_pending_discard(void)
{
    if (ax_pending_valid) {

        pmm_free_pages(
            ax_pending_phys,
            (ax_pending_hi - ax_pending_lo) >> 12
        );

        ax_pending_valid = 0;
        ax_pending_lo    = 0;
        ax_pending_hi    = 0;
        ax_pending_phys  = 0;
        ax_pending_abi   = 0;
    }
}

void aximage_free(
    uint64_t lo,
    uint64_t hi,
    uint64_t phys
)
{
    (void)lo;

    if (phys && hi > lo)
        pmm_free_pages(
            phys,
            (hi - lo) >> 12
        );
}

/*
 * ------------------------------------------------------------
 * ELF64 loader
 * ------------------------------------------------------------
 */

static void *image_load64(
    char *elf_start,
    unsigned int size
)
{
    if (!elf_start || size < sizeof(Elf64_Ehdr))
        return NULL;

    Elf64_Ehdr *hdr =
        (Elf64_Ehdr *)elf_start;

    if (!elf_valid64(hdr)) {

        serial_puts_ax(
            "ELF64: invalid header\n"
        );

        return NULL;
    }

    /*
     * Проверяем program header table.
     */

    if (hdr->e_phoff == 0 ||
        hdr->e_phnum == 0)
    {
        serial_puts_ax(
            "ELF64: no program headers\n"
        );

        return NULL;
    }

    Elf64_Phdr *phdr =
        (Elf64_Phdr *)
        (elf_start + hdr->e_phoff);

    /*
     * --------------------------------------------------------
     * Определяем диапазон PT_LOAD.
     * --------------------------------------------------------
     */

    uint64_t min_vaddr;
    uint64_t max_vaddr;

    if (!get_load_range(
            hdr,
            phdr,
            &min_vaddr,
            &max_vaddr))
    {
        serial_puts_ax(
            "ELF64: invalid PT_LOAD\n"
        );

        return NULL;
    }

    uint64_t image_size =
        max_vaddr - min_vaddr;

    serial_puts_ax("LOAD_ADDR: ");
    print_hex64(min_vaddr);
    serial_putc_ax('\n');

    serial_puts_ax("MAX_ADDR: ");
    print_hex64(max_vaddr);
    serial_putc_ax('\n');

    serial_puts_ax("MEM_SIZE: ");
    print_hex64(image_size);
    serial_putc_ax('\n');

    /*
     * --------------------------------------------------------
     * Выделяем память под образ.
     *
     * Non-PIE ET_EXEC (все наши приложения) содержат абсолютные
     * адреса, привязанные к линковочному vaddr (0x400000),
     * поэтому образ ОБЯЗАН быть отображён по этому адресу.
     *
     * Здесь мы только резервируем физически непрерывный блок
     * через PMM и копируем сегменты в него через HHDM-алиас.
     * task_spawn() затем создаёт задаче собственное адресное
     * пространство и отображает блок на [min_vaddr, max_vaddr).
     *
     * Старая схема (kmalloc в куче ядра + запуск прямо из кучи)
     * работала только пока код не трогал абсолютные адреса:
     * первая же jump-таблица уводила CPU на линковочный адрес
     * (0x4xxxxx), который в адресном пространстве ядра не
     * отображён -> бесконечный page fault ("tcc зависает").
     * --------------------------------------------------------
     */

    uint64_t npages = image_size >> 12;

    if (npages == 0) {

        serial_puts_ax(
            "ELF64: empty image\n"
        );

        return NULL;
    }

    uint64_t phys_base =
        pmm_alloc_pages(npages);

    if (!phys_base) {

        serial_puts_ax(
            "ELF64: phys allocation failed\n"
        );

        return NULL;
    }

    uint8_t *memory =
        (uint8_t *)vmm_phys_to_virt(
            phys_base
        );

    memset(
        memory,
        0,
        image_size
    );

    serial_puts_ax("PHYS_BASE: ");
    print_hex64(phys_base);
    serial_putc_ax('\n');

    /*
     * Публикуем образ для task_spawn().
     */
    ax_pending_valid = 1;
    ax_pending_lo    = min_vaddr;
    ax_pending_hi    = max_vaddr;
    ax_pending_phys  = phys_base;

    /*
     * --------------------------------------------------------
     * Загружаем PT_LOAD.
     * --------------------------------------------------------
     */

    if (!load_segments(
            hdr,
            phdr,
            (const uint8_t *)elf_start,
            memory,
            min_vaddr))
    {
        serial_puts_ax(
            "ELF64: segment loading failed\n"
        );

        ax_pending_valid = 0;
        pmm_free_pages(phys_base, npages);
        return NULL;
    }

    /*
     * --------------------------------------------------------
     * Dynamic relocations.
     * --------------------------------------------------------
     */

    if (!process_dynamic(
            hdr,
            phdr,
            (const uint8_t *)elf_start,
            memory,
            min_vaddr))
    {
        serial_puts_ax(
            "ELF64: relocation failed\n"
        );

        ax_pending_valid = 0;
        pmm_free_pages(phys_base, npages);
        return NULL;
    }

    /*
     * --------------------------------------------------------
     * ENTRY
     * --------------------------------------------------------
     */

    if (hdr->e_entry < min_vaddr ||
        hdr->e_entry >= max_vaddr)
    {
        serial_puts_ax(
            "ELF64: entry outside image\n"
        );

        ax_pending_valid = 0;
        pmm_free_pages(phys_base, npages);
        return NULL;
    }

    /*
     * ENTRY - пользователеvский виртуальный адрес (тот, что
     * записан в ELF). Он станет корректным, как только
     * task_spawn() отобразит образ на [min_vaddr, max_vaddr)
     * в адресном пространстве новой задачи.
     */

    void *entry =
        (void *)(uintptr_t)hdr->e_entry;

    serial_puts_ax(
        "ENTRY_VADDR: "
    );

    print_hex64(
        hdr->e_entry
    );

    serial_putc_ax('\n');

    serial_puts_ax(
        "FINAL_ENTRY (user vaddr): "
    );

    print_hex64(
        (uint64_t)entry
    );

    serial_putc_ax('\n');

    /*
     * Физический блок намеренно НЕ освобождаем здесь:
     * он станет образом новой задачи и будет отображён
     * task_spawn() по линковочному адресу.
     */

    return entry;
}


/*
 * ------------------------------------------------------------
 * ELF32
 *
 * Пока оставляем существующий 32-bit loader отдельно.
 * ------------------------------------------------------------
 */

#ifndef EM_386
#define EM_386 3
#endif

#ifndef ELFOSABI_LINUX
#define ELFOSABI_LINUX 3
#endif

#ifndef ELFOSABI_FREEBSD
#define ELFOSABI_FREEBSD 9
#endif

#ifndef EI_OSABI
#define EI_OSABI 7
#endif

#ifndef EI_CLASS
#define EI_CLASS 4
#endif

#ifndef ELFCLASS32
#define ELFCLASS32 1
#endif

#ifndef ELFCLASS64
#define ELFCLASS64 2
#endif


/*
 * ------------------------------------------------------------
 * Личность ABI по заголовку образа
 * ------------------------------------------------------------
 *
 * Возвращает AX_ABI_* из multitask/task.h.  Для ELF64 различает
 * родную программу и особый режим Linux x86-64.
 */
static int elf_abi_of(const uint8_t *ident)
{
    unsigned char osabi = ident[EI_OSABI];

    if (ident[EI_CLASS] == ELFCLASS64) {

        if (osabi == ELFOSABI_LINUX)
            return AX_ABI_LINUX64;

        return AX_ABI_NATIVE;
    }

    if (ident[EI_CLASS] == ELFCLASS32) {

        if (osabi == ELFOSABI_FREEBSD)
            return AX_ABI_FREEBSD32;

        /* SysV / Linux / пустой OSABI на i386 -- Linux. */
        return AX_ABI_LINUX32;
    }

    return AX_ABI_NATIVE;
}


/*
 * ------------------------------------------------------------
 * ELF32
 * ------------------------------------------------------------
 *
 * i386-образы Linux/BSD -- ET_EXEC без интерпретатора, грузятся
 * по своему линковочному адресу.  Схема та же, что у ELF64:
 * физически непрерывный блок от PMM копируется через HHDM-алиас,
 * task_spawn() создаёт адресное пространство задачи и отображает
 * блок на [min_vaddr, max_vaddr).
 *
 * Адреса i386 обязаны лежать ниже 4 ГиБ: 32-битный код считает
 * абсолютные адреса как unsigned int.
 */

#define ELF32_USER_MAX 0x100000000ULL

static void *image_load32(
    char *elf_start,
    unsigned int size
)
{
    Elf32_Ehdr *hdr =
        (Elf32_Ehdr *)elf_start;

    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' ||
        hdr->e_ident[3] != 'F' ||
        hdr->e_ident[EI_CLASS] != ELFCLASS32)
    {
        serial_puts_ax("ELF32: invalid ident\n");
        return NULL;
    }

    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {

        serial_puts_ax("ELF32: big-endian unsupported\n");
        return NULL;
    }

    if (hdr->e_machine != EM_386) {

        serial_puts_ax("ELF32: only i386 supported\n");
        return NULL;
    }

    if (hdr->e_type != ET_EXEC) {

        /*
         * ET_DYN требует релокаций, а ELF32-путь их пока не
         * умеет: сообщаем честно.
         */
        serial_puts_ax("ELF32: only ET_EXEC\n");
        return NULL;
    }

    if (hdr->e_phoff == 0 ||
        hdr->e_phnum == 0)
    {
        serial_puts_ax("ELF32: no program headers\n");
        return NULL;
    }

    Elf32_Phdr *phdr =
        (Elf32_Phdr *)(elf_start + hdr->e_phoff);

    /*
     * Диапазон [min_vaddr, max_vaddr) по PT_LOAD.
     */

    uint64_t min_vaddr = ~0ULL;
    uint64_t max_vaddr = 0;
    int      found     = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {

        if (phdr[i].p_type == PT_INTERP) {

            serial_puts_ax(
                "ELF32: dynamic binary unsupported\n"
            );

            return NULL;
        }

        if (phdr[i].p_type != PT_LOAD)
            continue;

        if (phdr[i].p_filesz > phdr[i].p_memsz) {

            serial_puts_ax("ELF32: filesz > memsz\n");
            return NULL;
        }

        uint64_t lo = phdr[i].p_vaddr;
        uint64_t hi = (uint64_t)phdr[i].p_vaddr +
                      phdr[i].p_memsz;

        if (hi > ELF32_USER_MAX) {

            serial_puts_ax("ELF32: segment above 4 GiB\n");
            return NULL;
        }

        if ((uint64_t)phdr[i].p_offset + phdr[i].p_filesz > size) {

            serial_puts_ax("ELF32: segment outside file\n");
            return NULL;
        }

        if (lo < min_vaddr)
            min_vaddr = lo;

        if (hi > max_vaddr)
            max_vaddr = hi;

        found = 1;
    }

    if (!found) {

        serial_puts_ax("ELF32: no PT_LOAD\n");
        return NULL;
    }

    min_vaddr &= ~0xFFFULL;
    max_vaddr  = (max_vaddr + 0xFFF) & ~0xFFFULL;

    if (max_vaddr > ELF32_USER_MAX)
        max_vaddr = ELF32_USER_MAX;

    uint64_t image_size = max_vaddr - min_vaddr;
    uint64_t npages     = image_size >> 12;

    if (npages == 0) {

        serial_puts_ax("ELF32: empty image\n");
        return NULL;
    }

    uint64_t phys_base = pmm_alloc_pages(npages);

    if (!phys_base) {

        serial_puts_ax("ELF32: phys allocation failed\n");
        return NULL;
    }

    uint8_t *memory = (uint8_t *)vmm_phys_to_virt(phys_base);

    memset(memory, 0, image_size);

    /*
     * Копируем сегменты.  p_vaddr - min_vaddr -- смещение
     * внутри непрерывного блока.
     */

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {

        Elf32_Phdr *p = &phdr[i];

        if (p->p_type != PT_LOAD)
            continue;

        uint64_t off = (uint64_t)p->p_vaddr - min_vaddr;

        if (p->p_filesz != 0) {

            memmove(
                memory + off,
                (const uint8_t *)elf_start + p->p_offset,
                p->p_filesz
            );
        }

        serial_puts_ax("LOAD32 segment: vaddr=");
        print_hex64(p->p_vaddr);
        serial_puts_ax(" filesz=");
        print_hex64(p->p_filesz);
        serial_puts_ax(" memsz=");
        print_hex64(p->p_memsz);
        serial_putc_ax('\n');
    }

    if (hdr->e_entry < min_vaddr ||
        hdr->e_entry >= max_vaddr)
    {
        serial_puts_ax("ELF32: entry outside image\n");

        pmm_free_pages(phys_base, npages);
        return NULL;
    }

    /*
     * Публикуем образ для task_spawn(): он отобразит блок
     * страницами PAGE_USER и переключит CR3.
     */
    ax_pending_valid = 1;
    ax_pending_lo    = min_vaddr;
    ax_pending_hi    = max_vaddr;
    ax_pending_phys  = phys_base;
    ax_pending_abi   = elf_abi_of(hdr->e_ident);

    void *entry = memory + (hdr->e_entry - min_vaddr);

    serial_puts_ax("ELF32 ENTRY_VADDR: ");
    print_hex64(hdr->e_entry);
    serial_puts_ax(" ABI: ");
    print_dec_u64((uint64_t)ax_pending_abi);
    serial_putc_ax('\n');

    return entry;
}


/*
 * ------------------------------------------------------------
 * Public entry
 * ------------------------------------------------------------
 */

void *image_load(
    char *elf_start,
    unsigned int size
)
{
    serial_puts_ax(
        "IMAGE_LOAD: size="
    );

    print_dec_u64(size);
    serial_putc_ax('\n');

    if (!elf_start ||
        size < 4)
    {
        serial_puts_ax(
            "INVALID ELF\n"
        );

        return NULL;
    }

    uint8_t *ident =
        (uint8_t *)elf_start;

    if (ident[0] != 0x7F ||
        ident[1] != 'E' ||
        ident[2] != 'L' ||
        ident[3] != 'F')
    {
        serial_puts_ax(
            "INVALID ELF MAGIC\n"
        );

        return NULL;
    }

    if (ident[4] == ELFCLASS64) {

        serial_puts_ax(
            "ELF64\n"
        );

        return image_load64(
            elf_start,
            size
        );
    }

    if (ident[4] == ELFCLASS32) {

        serial_puts_ax(
            "ELF32\n"
        );

        return image_load32(
            elf_start,
            size
        );
    }

    serial_puts_ax(
        "UNKNOWN ELF CLASS\n"
    );

    return NULL;
}

/*
 * Личность ABI последнего загруженного образа (AX_ABI_*).
 * fs/exec.c забирает её сразу после image_load() и ставит задаче.
 */
int aximage_pending_abi(void)
{
    return ax_pending_abi;
}
