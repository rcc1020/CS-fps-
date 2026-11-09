cmake_minimum_required(VERSION 3.15)
project(CS2_Optimizer_Panel C)

set(CMAKE_C_STANDARD 11)

add_executable(CS2_Optimizer_GUI WIN32
    src/main.c
    src/core.c
    src/tweaks.c
    src/proc.c
    src/checkup.c
    src/gui.c
    src/app.rc          # 嵌入清单 (Common-Controls v6 / asInvoker / PerMonitorV2 DPI) + 版本信息
)

target_include_directories(CS2_Optimizer_GUI PRIVATE src)
target_compile_definitions(CS2_Optimizer_GUI PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(CS2_Optimizer_GUI PRIVATE /W3 /O2 /utf-8)
    target_link_options(CS2_Optimizer_GUI PRIVATE "/MANIFEST:NO")
else()
    target_compile_options(CS2_Optimizer_GUI PRIVATE -Wall -O2)
    target_link_options(CS2_Optimizer_GUI PRIVATE -municode)
endif()

target_link_libraries(CS2_Optimizer_GUI PRIVATE
    user32 gdi32 winmm advapi32 shell32 comctl32 psapi powrprof shlwapi)
