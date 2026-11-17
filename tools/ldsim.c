/*
 * ldsim — симулятор загрузчика ELF ядра ArtyomX64 для хост-Linux.
 *
 * ТОЧНО повторяет алгоритм multitask/loader.c (image_load64):
 *   - get_load_range / load_segments (включая нулевой .bss)
 *   - process_dynamic: base_delta = memory - load_base,
 *     R_X86_64_RELATIVE: *where = base_delta + addend
 *     (+ GLOB_DAT/JUMP_SLOT/64/PC32 через symtab — как в ядре)
 *   - образ размещается по ПРОИЗВОЛЬНОМУ адресу (mmap с ASLR —
 *     эмулирует kmalloc кучи ядра)
 *   - вход в задачу: rsp 16-выровнен, на стеке фиктивный адрес
 *     возврата, RDI = exec_args_t* (как task_spawn)
 *
 * Отличие от реального ядра ровно одно: int $0x80 в загруженном
 * образе (список адресов даёт objdump) заменяется на ud2, а SIGILL
 * handler эмулирует таблицу syscall ArtyomX64 поверх Linux ABI.
 *
 * Сборка: gcc -O2 -o ldsim ldsim.c
 * Запуск: ./ldsim <elf> <int80-addr-list> <argv...>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ */
/* Минимальные ELF64-структуры (как в multitask/elf.h)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
} Phdr;

typedef struct {
    int64_t  d_tag;
    uint64_t d_un;      /* d_val / d_ptr */
} Dyn;

typedef struct {
    uint64_t r_offset, r_info;
    int64_t  r_addend;
} Rela;

typedef struct {
    uint32_t    st_name;
    unsigned char st_info, st_other;
    uint16_t    st_shndx;
    uint64_t    st_value, st_size;
} Sym;

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PF_X        1

#define DT_NULL     0
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9

#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_PLT32      4
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_X86_64   62
#define ET_EXEC     2
#define ET_DYN      3

/* ------------------------------------------------------------------ */
/* Трансляция syscall ArtyomX64 -> Linux (логика host_shim.c)          */
/* ------------------------------------------------------------------ */

#define AX_exit   1
#define AX_read   3
#define AX_write  4
#define AX_open   5
#define AX_close  6
#define AX_lseek  8
#define AX_mmap   9
#define AX_unlink 10
#define AX_exec   11
#define AX_yield  151

static long lx_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static long ax_syscall(long n, long a1, long a2, long a3, long a4, long a5)
{
    switch (n) {
    case AX_read:
        return lx_syscall(0, a1, a2, a3, 0, 0, 0);

    case AX_write:
        return lx_syscall(1, a1, a2, a3, 0, 0, 0);

    case AX_open:
        /* open(path, flags, mode) -> openat(AT_FDCWD, ...) */
        if (a2 & 0100)
            return lx_syscall(257, -100 /*AT_FDCWD*/, a1, a2, a3, 0, 0);
        return lx_syscall(257, -100, a1, a2, 0, 0, 0);

    case AX_close:
        return lx_syscall(3, a1, 0, 0, 0, 0, 0);

    case AX_lseek:
        return lx_syscall(8, a1, a2, a3, 0, 0, 0);

    case AX_mmap:
        /* на ОС не реализован ядром; если сработает — это баг порта */
        fprintf(stderr, "[ldsim] WARNING: mmap syscall used!\n");
        return lx_syscall(9, a1, a2, a3, a4, a5, 0);

    case AX_unlink:
        return lx_syscall(87, a1, 0, 0, 0, 0, 0);

    case AX_yield:
        return 0;

    case AX_exec:
        fprintf(stderr, "[ldsim] exec syscall (not emulated)\n");
        return -1;

    case AX_exit:
        fprintf(stderr, "\n[ldsim] EXIT(%ld)\n", a1);
        fflush(stderr);
        fflush(stdout);
        _exit(a1 == 0 ? 0 : 1);

    default:
        fprintf(stderr, "[ldsim] UNIMPLEMENTED syscall %ld\n", n);
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Состояние загруженного образа                                       */
/* ------------------------------------------------------------------ */

static uint8_t *image;          /* фактический адрес загрузки           */
static uint64_t load_base;      /* min_vaddr ELF-образа                 */
static uint64_t image_size;
static uint64_t entry_addr;

/* ------------------------------------------------------------------ */
/* Обработчики сигналов                                                */
/* ------------------------------------------------------------------ */

static void dump_regs(ucontext_t *c, const char *why)
{
    greg_t *g = c->uc_mcontext.gregs;
    fprintf(stderr,
            "\n[ldsim] %s\n"
            "  RIP=%llx RAX=%llx RBX=%llx RCX=%llx RDX=%llx\n"
            "  RSI=%llx RDI=%llx RBP=%llx RSP=%llx\n"
            "  R8 =%llx R9 =%llx R10=%llx R11=%llx\n",
            why,
            (unsigned long long)g[REG_RIP], (unsigned long long)g[REG_RAX],
            (unsigned long long)g[REG_RBX], (unsigned long long)g[REG_RCX],
            (unsigned long long)g[REG_RDX], (unsigned long long)g[REG_RSI],
            (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_RBP],
            (unsigned long long)g[REG_RSP], (unsigned long long)g[REG_R8],
            (unsigned long long)g[REG_R9],  (unsigned long long)g[REG_R10],
            (unsigned long long)g[REG_R11]);
    fflush(stderr);
    _exit(2);
}

static void sigill_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    ucontext_t *c = uc;
    greg_t *g = c->uc_mcontext.gregs;

    /* ud2 = 0f 0b -> эмулируем syscall ArtyomX64 */
    uint16_t insn;
    memcpy(&insn, (void *)(uintptr_t)g[REG_RIP], sizeof(insn));

    if (insn == 0x0b0f) {
        long ret = ax_syscall(
            (long)g[REG_RAX],
            (long)g[REG_RDI], (long)g[REG_RSI], (long)g[REG_RDX],
            (long)g[REG_RCX], (long)g[REG_R8]
        );
        g[REG_RAX] = (greg_t)ret;
        g[REG_RIP] += 2;            /* длина ud2 */
        return;
    }

    dump_regs(c, "SIGILL (не ud2)");
}

static void sigsegv_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    ucontext_t *c = uc;
    greg_t *g = c->uc_mcontext.gregs;

    if (g[REG_RIP] == 0) {
        fprintf(stderr, "\n[ldsim] FAIL: задача вернулась на адрес 0 "
                        "(нет exit?)\n");
        _exit(2);
    }
    dump_regs(c, "SIGSEGV (page fault — абсолютный адрес не переехал?)");
}

/* ------------------------------------------------------------------ */
/* Загрузка: реплика image_load64                                      */
/* ------------------------------------------------------------------ */

static uint8_t *file_data;
static uint64_t file_size;

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up  (uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int get_load_range(Ehdr *hdr, Phdr *ph,
                          uint64_t *min_v, uint64_t *max_v)
{
    uint64_t min = UINT64_MAX, max = 0;
    int found = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        fprintf(stderr, "[dbg] LOAD%u: off=0x%llx vaddr=0x%llx "
                        "filesz=0x%llx memsz=0x%llx\n", i,
                (unsigned long long)ph[i].p_offset,
                (unsigned long long)ph[i].p_vaddr,
                (unsigned long long)ph[i].p_filesz,
                (unsigned long long)ph[i].p_memsz);
        if (ph[i].p_filesz > ph[i].p_memsz) {
            fprintf(stderr, "[dbg] fail: filesz > memsz\n");
            return 0;
        }
        if (ph[i].p_offset + ph[i].p_filesz == 0) {
            fprintf(stderr, "[dbg] fail: offset+filesz == 0\n");
            return 0;
        }
        if (ph[i].p_vaddr < min) min = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max)
            max = ph[i].p_vaddr + ph[i].p_memsz;
        found = 1;
    }
    if (!found) return 0;

    *min_v = align_down(min, 0x1000);
    *max_v = align_up(max, 0x1000);
    return 1;
}

static int load_segments(Ehdr *hdr, Phdr *ph)
{
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_filesz > p->p_memsz) return 0;

        uint64_t off = p->p_vaddr - load_base;
        uint8_t *dst = image + off;
        const uint8_t *src = file_data + p->p_offset;

        if (p->p_filesz)
            memmove(dst, src, p->p_filesz);
        if (p->p_memsz > p->p_filesz)
            memset(dst + p->p_filesz, 0, p->p_memsz - p->p_filesz);

        fprintf(stderr, "LOAD segment: vaddr=0x%llx filesz=0x%llx "
                        "memsz=0x%llx\n",
                (unsigned long long)p->p_vaddr,
                (unsigned long long)p->p_filesz,
                (unsigned long long)p->p_memsz);
    }
    return 1;
}

static uint64_t dynamic_value(Dyn *dyn, int64_t tag)
{
    if (!dyn) return 0;
    for (;;) {
        if (dyn->d_tag == DT_NULL) break;
        if (dyn->d_tag == tag) return dyn->d_un;
        dyn++;
    }
    return 0;
}

static int process_rela(Rela *rela, uint64_t count, Sym *symtab,
                        const char *strtab)
{
    /* base_delta — как в ядре: разница фактического адреса и ELF-vaddr */
    uint64_t base_delta = (uint64_t)image - load_base;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t type = rela[i].r_info & 0xffffffff;
        uint64_t symi = rela[i].r_info >> 32;

        uint64_t *where = (uint64_t *)(image +
            (rela[i].r_offset - load_base));

        uint64_t S = 0;
        if (symi != 0 && symtab) {
            Sym *sym = &symtab[symi];
            if (sym->st_shndx == 0) {
                fprintf(stderr, "[ldsim] UNRESOLVED SYMBOL: %s\n",
                        strtab ? strtab + sym->st_name : "?");
                return 0;
            }
            S = (uint64_t)(image + (sym->st_value - load_base));
        }

        int64_t A = rela[i].r_addend;
        uint64_t P = (uint64_t)where;

        switch (type) {
        case 0: break;                                   /* NONE      */
        case R_X86_64_64:       *where = S + A; break;
        case R_X86_64_RELATIVE: *where = base_delta + A; break;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            *(uint32_t *)where = (uint32_t)((S + A) - P);
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: *where = S; break;
        default:
            fprintf(stderr, "[ldsim] UNSUPPORTED RELOCATION: %llu\n",
                    (unsigned long long)type);
            return 0;
        }
    }
    return 1;
}

static int process_dynamic(Ehdr *hdr, Phdr *ph)
{
    Dyn *dyn = NULL;
    for (uint16_t i = 0; i < hdr->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (Dyn *)(file_data + ph[i].p_offset);
            break;
        }
    if (!dyn) {
        fprintf(stderr, "[ldsim] нет PT_DYNAMIC — статический ET_EXEC, "
                        "релокаций не будет (так и случается зависание)\n");
        return 1;
    }

    uint64_t rela_addr  = dynamic_value(dyn, DT_RELA);
    uint64_t rela_size  = dynamic_value(dyn, DT_RELASZ);
    uint64_t rela_ent   = dynamic_value(dyn, DT_RELAENT);
    uint64_t symtab_a   = dynamic_value(dyn, DT_SYMTAB);
    uint64_t strtab_a   = dynamic_value(dyn, DT_STRTAB);

    if (!rela_addr || !rela_size) return 1;
    if (!rela_ent) rela_ent = sizeof(Rela);

    uint64_t count = rela_size / rela_ent;
    fprintf(stderr, "DYNAMIC RELA count=%llu\n",
            (unsigned long long)count);

    return process_rela(
        (Rela *)(image + (rela_addr - load_base)),
        count,
        symtab_a ? (Sym *)(image + (symtab_a - load_base)) : NULL,
        strtab_a ? (const char *)(image + (strtab_a - load_base)) : NULL
    );
}

/* ------------------------------------------------------------------ */
/* Патч int $0x80 (cd 80) -> ud2 (0f 0b)                               */
/* ------------------------------------------------------------------ */

static int patch_int80(const char *listfile)
{
    FILE *f = fopen(listfile, "r");
    if (!f) {
        perror("patch list");
        return 0;
    }

    char line[128];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t vaddr = strtoull(line, NULL, 16);
        uint8_t *p = image + (vaddr - load_base);
        uint16_t cur;
        memcpy(&cur, p, 2);
        if (cur != 0x80cd) {
            fprintf(stderr, "[ldsim] patch: по 0x%llx не cd 80 (%04x)\n",
                    (unsigned long long)vaddr, cur);
            fclose(f);
            return 0;
        }
        uint16_t ud2 = 0x0b0f;
        memcpy(p, &ud2, 2);
        n++;
    }
    fclose(f);
    fprintf(stderr, "[ldsim] patched %d int $0x80 -> ud2\n", n);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Вход в задачу (аналог switchTask + стартовых условий task_spawn)    */
/* ------------------------------------------------------------------ */

__attribute__((naked)) static void call_entry(void)
{
    /* rdi = entry, rsi = exec_args*, rdx = stack_top (16-выровнен) */
    __asm__ volatile(
        "mov %rdi, %rax\n"      /* entry                                   */
        "mov %rdx, %rsp\n"      /* переключаемся на стек задачи            */
        "pushq $0\n"            /* sentinel-адрес возврата: rsp ≡ 8 (mod 16),
                                   как после task_spawn; если задача вернётся
                                   — RIP=0 → SIGSEGV → диагностика        */
        "mov %rsi, %rdi\n"      /* RDI = exec_args*                        */
        "jmp *%rax\n"
    );
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: ldsim <elf> <int80list> [args...]\n");
        return 1;
    }

    /* --- читаем файл --- */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file_data = malloc(file_size);
    if (fread(file_data, 1, file_size, f) != file_size) { perror("read"); return 1; }
    fclose(f);

    Ehdr *hdr = (Ehdr *)file_data;
    if (hdr->e_ident[0] != 0x7f || hdr->e_ident[4] != ELFCLASS64 ||
        hdr->e_machine != EM_X86_64) {
        fprintf(stderr, "[ldsim] не ELF64 x86-64\n");
        return 1;
    }
    fprintf(stderr, "[ldsim] ELF e_type=%u (%s) entry=0x%llx\n",
            hdr->e_type,
            hdr->e_type == ET_DYN ? "DYN/PIE" :
            hdr->e_type == ET_EXEC ? "EXEC (!) — зависание как на ОС" : "?",
            (unsigned long long)hdr->e_entry);

    Phdr *ph = (Phdr *)(file_data + hdr->e_phoff);

    /* --- реплика get_load_range + выделение памяти --- */
    uint64_t min_v, max_v;
    if (!get_load_range(hdr, ph, &min_v, &max_v)) {
        fprintf(stderr, "[ldsim] invalid PT_LOAD\n");
        return 1;
    }
    load_base = min_v;
    image_size = max_v - min_v;

    fprintf(stderr, "LOAD_ADDR: 0x%llx\n", (unsigned long long)min_v);
    fprintf(stderr, "MAX_ADDR:  0x%llx\n", (unsigned long long)max_v);
    fprintf(stderr, "MEM_SIZE:  0x%llx\n", (unsigned long long)image_size);

    /* kmalloc ядра -> произвольный адрес; здесь — ASLR mmap */
    image = mmap(NULL, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image == MAP_FAILED) { perror("mmap"); return 1; }
    memset(image, 0, image_size);
    fprintf(stderr, "BUFFER:    0x%llx\n", (unsigned long long)(uintptr_t)image);

    if (!load_segments(hdr, ph)) {
        fprintf(stderr, "[ldsim] segment loading failed\n");
        return 1;
    }

    if (!process_dynamic(hdr, ph)) {
        fprintf(stderr, "[ldsim] relocation failed\n");
        return 1;
    }

    if (hdr->e_entry < min_v || hdr->e_entry >= max_v) {
        fprintf(stderr, "[ldsim] entry outside image\n");
        return 1;
    }
    entry_addr = (uint64_t)image + (hdr->e_entry - min_v);
    fprintf(stderr, "FINAL_ENTRY: 0x%llx\n", (unsigned long long)entry_addr);

    /* --- патч int $0x80 -> ud2 --- */
    if (!patch_int80(argv[2])) {
        fprintf(stderr, "[ldsim] патч syscall не удался\n");
        return 1;
    }

    /* --- сигналы --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGILL, &sa, NULL);
    sa.sa_sigaction = sigsegv_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* --- exec_args (как в ядре: {int argc; pad; char **argv}) --- */
    int nargs = argc - 3;               /* ldsim, elf, int80list, потом argv */
    char **uargv = calloc(nargs + 1, sizeof(char *));
    for (int i = 3; i < argc; i++)
        uargv[i - 3] = argv[i];
    uargv[nargs] = NULL;

    struct { int32_t argc; int32_t pad; char **argv; } *eargs =
        calloc(1, sizeof(*eargs));
    eargs->argc = nargs;
    eargs->argv = uargv;

    /* --- стек: как task_spawn, STACK_SIZE = 16384 (верность ядру!) --- */
#define STACK_SIZE 16384
    uint8_t *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) { perror("stack mmap"); return 1; }
    uint64_t top = ((uint64_t)stack + STACK_SIZE) & ~0xFULL;
    /* адрес возврата положит call_entry: на входе rsp ≡ 8 (mod 16) */

    fprintf(stderr, "[ldsim] запуск: entry=0x%llx rsp=0x%llx "
                    "args={argc=%d argv=%p}\n\n",
            (unsigned long long)entry_addr, (unsigned long long)top,
            nargs, (void *)uargv);

    fflush(stderr);
    fflush(stdout);

    /* call_entry(entry, args, stack_top): rdi/rsi/rdx */
    __asm__ volatile(
        "movq %0, %%rax\n"
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq %3, %%rdx\n"
        "call *%%rax\n"
        :
        : "r"((uint64_t)(uintptr_t)&call_entry),
          "r"(entry_addr),
          "r"((uint64_t)(uintptr_t)eargs),
          "r"(top)
        : "rax", "rdi", "rsi", "rdx", "memory"
    );

    fprintf(stderr, "\n[ldsim] call_entry вернулся (не должен)\n");
    return 2;
}
