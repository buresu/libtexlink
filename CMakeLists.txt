cmake_minimum_required(VERSION 3.21)

project(dmabuflink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DMABL_BUILD_SHARED_LIBS "Build dmabuflink as a shared library" OFF)
option(DMABL_BUILD_EXAMPLES "Build example programs" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM  REQUIRED gbm)
pkg_check_modules(DRM  REQUIRED libdrm)

add_library(dmabuflink
    src/buffer.c
    src/socket.c
    src/sync.c
    src/session.c
    src/registry.c
)

if(BUILD_SHARED_LIBS)
    set_target_properties(dmabuflink PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

target_include_directories(dmabuflink
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE src
            ${GBM_INCLUDE_DIRS}
            ${DRM_INCLUDE_DIRS}
)

target_link_libraries(dmabuflink
    PUBLIC ${GBM_LIBRARIES}
           ${DRM_LIBRARIES}
           rt
)

target_compile_options(dmabuflink PRIVATE
    -Wall -Wextra -Wpedantic
    ${GBM_CFLAGS_OTHER}
    ${DRM_CFLAGS_OTHER}
)

if(DMABL_BUILD_EXAMPLES)
    foreach(example producer consumer)
        add_executable(${example} examples/${example}.c)
        target_link_libraries(${example} PRIVATE dmabuflink)
        target_include_directories(${example} PRIVATE include)
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endforeach()
endif()

include(GNUInstallDirs)

install(TARGETS dmabuflink
    EXPORT  dmabuflink-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/dmabuflink.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT dmabuflink-targets
    FILE      dmabuflink-config.cmake
    NAMESPACE dmabuflink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dmabuflink
)
