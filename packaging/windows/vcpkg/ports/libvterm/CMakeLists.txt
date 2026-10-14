cmake_minimum_required(VERSION 3.21)
project(libvterm C)

include(GNUInstallDirs)

file(GLOB VTERM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(vterm STATIC ${VTERM_SOURCES})
set_target_properties(vterm PROPERTIES
    C_STANDARD 99
    EXPORT_NAME libvterm
)
target_include_directories(vterm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(MSVC)
    # 源码按 UTF-8 编写；C4996 是 MSVC 对标准 C 字符串函数的“不安全”提示，与上游无关。
    target_compile_options(vterm PRIVATE /utf-8 /wd4996)
endif()

install(TARGETS vterm EXPORT unofficial-libvterm-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES include/vterm.h include/vterm_keycodes.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(EXPORT unofficial-libvterm-targets
    NAMESPACE unofficial::
    DESTINATION "share/unofficial-libvterm"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-libvterm-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-libvterm-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-libvterm-config.cmake"
    DESTINATION "share/unofficial-libvterm"
)
