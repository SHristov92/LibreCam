find_package(Threads REQUIRED)

add_executable(a9_host hal_host.c main.c)
target_include_directories(a9_host PRIVATE .)
target_compile_definitions(a9_host PRIVATE _GNU_SOURCE)
target_compile_options(a9_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(a9_host a9_core Threads::Threads)
