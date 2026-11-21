// Flush a complete write to the backing file before replacing the previous copy.
#pragma once
#include <cstdio>
#include <cstdint>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace qilin::rt {
inline bool seek_file(FILE* file, int64_t offset, int whence) {
#if defined(_WIN32)
    return _fseeki64(file, offset, whence) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), whence) == 0;
#endif
}
inline int64_t tell_file(FILE* file) {
#if defined(_WIN32)
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}
inline bool sync_file(FILE* file) {
    if (std::fflush(file) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
}  // namespace qilin::rt
