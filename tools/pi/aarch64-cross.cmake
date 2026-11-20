# Cross-compiling for 64-bit ARM Linux (the Raspberry Pi 4's Raspberry Pi OS)
# from x86-64 Linux with Debian/Ubuntu's g++-14-aarch64-linux-gnu, as a check
# that everything, the code lifted from the disc included, builds for it
# (tools/check/arm64_build.sh). Steps that run built programs (lifting the
# disc, the tests) go through qemu-aarch64 (qemu-user). On a Pi itself, build
# natively instead (tools/build-punchout.sh).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-14)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L /usr/aarch64-linux-gnu)
# The Pi 4's Cortex-A72.
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a72")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a72")
