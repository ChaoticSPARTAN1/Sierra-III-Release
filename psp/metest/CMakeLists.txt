cmake_minimum_required(VERSION 3.10)
set(CMAKE_TOOLCHAIN_FILE "$ENV{PSPDEV}/psp/share/pspdev.cmake" CACHE PATH "toolchain file")
project(halo_me_test C CXX ASM)
add_executable(halo_me_test main.cpp)
target_compile_options(halo_me_test PRIVATE -O2 -G0 -Wall -fno-pic)
target_compile_options(halo_me_test PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)
target_link_libraries(halo_me_test me-core psppower pspge pspdisplay pspctrl pspdebug m)
create_pbp_file(TARGET halo_me_test TITLE "Halo PSP: Media Engine test" BUILD_PRX)
