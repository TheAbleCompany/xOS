#include "../drivers/monitor.h"
#include "elf.h"
#include "../lib/common.h"
#include "../mm/kheap.h"

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
    for (uint64_t i = 0; i < count; i++) {

        uint64_t type =
            ELF64_R_TYPE(rela[i].r_info);

        uint64_t sym_index =
            ELF64_R_SYM(rela[i].r_info);

        uint64_t *where =
            (uint64_t *)loaded_address(
                memory,
                load_base,
                rela[i].r_offset
            );

        uint64_t S = 0;

        if (sym_index != 0 && symtab) {

            Elf64_Sym *sym =
                &symtab[sym_index];

            if (sym->st_shndx != SHN_UNDEF) {

                S =
                    load_base +
                    sym->st_value;

            } else {

                /*
                 * Пока внешний символ не разрешаем.
                 *
                 * Для статически слинкованного newlib
                 * большинство символов newlib будут
                 * определены внутри самого ELF.
                 */

                const char *name = NULL;

                if (strtab)
                    name =
                        strtab + sym->st_name;

                serial_puts_ax(
                    "UNRESOLVED SYMBOL: "
                );

                if (name)
                    serial_puts_ax(name);

                serial_putc_ax('\n');

                return 0;
            }
        }

        int64_t A =
            rela[i].r_addend;

        uint64_t P =
            (uint64_t)where;

        switch (type) {

        case R_X86_64_NONE:
            break;

        /*
         * S + A
         */
        case R_X86_64_64:
            *where = S + A;
            break;

        /*
         * B + A
         *
         * Для ET_DYN base = load_base.
         */
        case R_X86_64_RELATIVE:
            *where =
                load_base + A;
            break;

        /*
         * S + A - P
         */
        case R_X86_64_PC32:
            *(uint32_t *)where =
                (uint32_t)(
                    (S + A) - P
                );
            break;

        /*
         * S
         */
        case R_X86_64_GLOB_DAT:
            *where = S;
            break;

        /*
         * S
         */
        case R_X86_64_JUMP_SLOT:
            *where = S;
            break;

        /*
         * 32-bit absolute
         */
        case R_X86_64_32:
            *(uint32_t *)where =
                (uint32_t)(S + A);
            break;

        /*
         * signed 32-bit absolute
         */
        case R_X86_64_32S:
            *(int32_t *)where =
                (int32_t)(S + A);
            break;

        default:

            serial_puts_ax(
                "UNSUPPORTED RELOCATION: "
            );

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
     * Выделяем память.
     *
     * ВАЖНО:
     *
     * memory соответствует min_vaddr.
     *
     * То есть:
     *
     * loaded address =
     *     memory + (vaddr - min_vaddr)
     *
     * --------------------------------------------------------
     */

    uint8_t *memory =
        (uint8_t *)malloc(
            image_size
        );

    if (!memory) {

        serial_puts_ax(
            "ELF64: allocation failed\n"
        );

        return NULL;
    }

    memset(
        memory,
        0,
        image_size
    );

    serial_puts_ax("BUFFER: ");
    print_hex64(
        (uint64_t)memory
    );
    serial_putc_ax('\n');

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

        free(memory);
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

        free(memory);
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

        free(memory);
        return NULL;
    }

    uint64_t entry_offset =
        hdr->e_entry - min_vaddr;

    void *entry =
        memory + entry_offset;

    serial_puts_ax(
        "ENTRY_VADDR: "
    );

    print_hex64(
        hdr->e_entry
    );

    serial_putc_ax('\n');

    serial_puts_ax(
        "ENTRY_OFFSET: "
    );

    print_hex64(
        entry_offset
    );

    serial_putc_ax('\n');

    serial_puts_ax(
        "FINAL_ENTRY: "
    );

    print_hex64(
        (uint64_t)entry
    );

    serial_putc_ax('\n');

    /*
     * memory намеренно НЕ освобождаем.
     *
     * entry указывает непосредственно внутрь
     * загруженного образа.
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

static void *image_load32(
    char *elf_start,
    unsigned int size
)
{
    (void)elf_start;
    (void)size;

    serial_puts_ax(
        "ELF32 loader not implemented\n"
    );

    return NULL;
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