find_package(Threads REQUIRED)

add_executable(lc_host hal_host.c main.c)
target_include_directories(lc_host PRIVATE .)
target_compile_definitions(lc_host PRIVATE _GNU_SOURCE)
target_compile_options(lc_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lc_host lc_core Threads::Threads)
