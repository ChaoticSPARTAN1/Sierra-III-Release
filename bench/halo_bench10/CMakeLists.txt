cmake_minimum_required(VERSION 3.10)
set(CMAKE_TOOLCHAIN_FILE "$ENV{PSPDEV}/psp/share/pspdev.cmake" CACHE PATH "toolchain file")
project(halo_bench10 C CXX ASM)
add_executable(halo_bench10 main.cpp sysctrl_import_0000.S sysctrl_import_0001.S)
target_compile_options(halo_bench10 PRIVATE -O2 -G0 -Wall -fno-pic)
target_compile_options(halo_bench10 PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)
target_link_libraries(halo_bench10 me-core psppower pspctrl pspdebug psprtc pspdisplay pspge m)
create_pbp_file(TARGET halo_bench10 TITLE "Halo PSP Bench 10 - Memory Stick and ME" BUILD_PRX)
