// Starting the program again, for the game's reset (OSRestart, os.cpp): a new
// process with the original command line replaces this one. The game's state,
// threads, window and sound go with the old process; saves are already on disk
// (NAND writes are synchronous).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX  // MinGW defines it already
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "os.hpp"
#include "qilin/rt/system.hpp"

namespace punchout {
namespace {
std::vector<std::string> g_args;  // argv, with argv[0] made absolute if it was a path
}

void set_command_line(int argc, char** argv) {
    g_args.assign(argv, argv + argc);
    // A relative path to the program would break if anything changed folder;
    // a bare name is found on PATH again.
    if (!g_args.empty() && g_args[0].find_first_of("/\\") != std::string::npos)
        g_args[0] = std::filesystem::absolute(g_args[0]).string();
}

void restart_program() {
#if defined(_WIN32)
    // The command line exactly as this process got it: arguments with spaces
    // (game folders often have them) stay quoted as the user quoted them.
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring line = GetCommandLineW();
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        qilin::rt::fatal(nullptr, "reset: cannot start the program again (error %lu)", GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::_Exit(0);
#else
    if (g_args.empty()) qilin::rt::fatal(nullptr, "reset: the command line wasn't recorded");
    std::vector<char*> argv;
    for (std::string& a : g_args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());  // returns only on failure
    qilin::rt::fatal(nullptr, "reset: cannot start %s again", argv[0]);
#endif
}

}  // namespace punchout
