OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

SECTIONS
{
    . = 0xffffffff80200000;

    .limine_requests_start : ALIGN(4096)
    {
        KEEP(*(.limine_requests_start))
    }

    .limine_requests : ALIGN(4096)
    {
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }

    .multiboot : ALIGN(4096)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4096)
    {
        *(.text*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    }

    .data : ALIGN(4096)
    {
        *(.data*)
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss*)
    }

    .tls : ALIGN(16)
    {
        _tls_start = .;
        *(.tdata*)
        *(.tbss*)
        . = ALIGN(16);
        _tls_end = .;
    }

    end = .; _end = .; __end = .;

    .heap (NOLOAD) : ALIGN(4096)
    {
        _heap_start = .;
        . = . + 64 * 1024 * 1024;
        _heap_end = .;
    }

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
