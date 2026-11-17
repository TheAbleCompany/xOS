#include <stddef.h>
#include <stdint.h>
#include <unordered_map>

extern "C" {
void *malloc(size_t n);
void free(void *p);
}

void *operator new(size_t n)
{
    return malloc(n ? n : 1);
}

void *operator new[](size_t n)
{
    return malloc(n ? n : 1);
}

void operator delete(void *p) noexcept
{
    free(p);
}

void operator delete[](void *p) noexcept
{
    free(p);
}

void operator delete(void *p, size_t) noexcept
{
    free(p);
}

void operator delete[](void *p, size_t) noexcept
{
    free(p);
}

namespace std {

namespace __detail {

std::pair<bool, std::size_t>
_Prime_rehash_policy::_M_need_rehash(std::size_t, std::size_t,
                                     std::size_t) const
{
    return std::pair<bool, std::size_t>(false, 0);
}

}



static void cxx_trap(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void __throw_bad_alloc()
{
    cxx_trap();
}

void __throw_length_error(const char *)
{
    cxx_trap();
}

void __throw_bad_array_new_length()
{
    cxx_trap();
}

}
