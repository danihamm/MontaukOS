/*
 * cxxrt.cpp
 * Minimal C++ runtime support for freestanding MontaukOS apps.
 * Provides operator new/delete backed by montauk::malloc/mfree.
 */

#include <cstddef>
#include <montauk/heap.h>

void* operator new(size_t size) { return montauk::malloc(size); }
void* operator new[](size_t size) { return montauk::malloc(size); }
void operator delete(void* ptr) noexcept { montauk::mfree(ptr); }
void operator delete[](void* ptr) noexcept { montauk::mfree(ptr); }
void operator delete(void* ptr, size_t) noexcept { montauk::mfree(ptr); }
void operator delete[](void* ptr, size_t) noexcept { montauk::mfree(ptr); }
