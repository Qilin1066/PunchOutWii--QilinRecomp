#include "qilin/rt/memory.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // MinGW defines it already
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace qilin::rt {

namespace {
constexpr uint64_t kSpace = uint64_t{1} << 32;
}

GuestMemory::~GuestMemory() {
    if (!base_) return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, kSpace);
#endif
}

bool GuestMemory::reserve() {
    if (base_) return true;
#if defined(_WIN32)
    void* p = VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return false;
#else
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
#endif
    base_ = static_cast<uint8_t*>(p);
    return true;
}

bool GuestMemory::commit(uint32_t address, uint32_t size) {
    if (!base_ || size == 0 || uint64_t{address} + size > kSpace) return false;
#if defined(_WIN32)
    return VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    // mprotect needs page-aligned bounds.
    const uint64_t page = 4096, lo = address & ~(page - 1), hi = (uint64_t{address} + size + page - 1) & ~(page - 1);
    return mprotect(base_ + lo, hi - lo, PROT_READ | PROT_WRITE) == 0;
#endif
}

}  // namespace qilin::rt
