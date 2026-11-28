// Flush a complete write to the backing file before replacing the previous copy.
#pragma once
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace qilin::rt {
// std::fopen on a path: through its UTF-16 name on Windows, where a narrow
// name is read in the ANSI code page (a folder named in Japanese, say).
inline FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
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
