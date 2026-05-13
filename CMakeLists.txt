cmake_minimum_required(VERSION 3.21)

project(texlink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(TEXLINK_BUILD_SHARED_LIBS "Build texlink as a shared library" OFF)
option(TEXLINK_BUILD_BACKENDS "Build texlink backend utility libraries" ON)
option(TEXLINK_BUILD_EXAMPLES "Build example programs" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM  REQUIRED gbm)

add_library(texlink
    src/buffer.c
    src/socket.c
    src/sync.c
    src/session.c
    src/registry.c
)

if(BUILD_SHARED_LIBS)
    set_target_properties(texlink PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

target_include_directories(texlink
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE src
            ${GBM_INCLUDE_DIRS}
)

target_link_libraries(texlink
    PUBLIC ${GBM_LIBRARIES}
           rt
)

target_compile_options(texlink PRIVATE
    -Wall -Wextra -Wpedantic
    ${GBM_CFLAGS_OTHER}
)

set(TEXLINK_INSTALL_TARGETS texlink)

pkg_check_modules(GLFW3 glfw3)
pkg_check_modules(EGL   egl)
find_package(OpenGL COMPONENTS OpenGL)
find_package(GLEW)
find_package(Vulkan)

if(TEXLINK_BUILD_BACKENDS)
    if(EGL_FOUND AND OpenGL_OpenGL_FOUND AND GLEW_FOUND)
        add_library(texlink_egl src/egl.c)
        target_link_libraries(texlink_egl
            PUBLIC texlink
                   ${EGL_LIBRARIES}
                   OpenGL::OpenGL
            PRIVATE GLEW::GLEW
        )
        target_include_directories(texlink_egl
            PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:include>
            PRIVATE ${EGL_INCLUDE_DIRS}
        )
        target_compile_options(texlink_egl PRIVATE
            -Wall -Wextra -Wpedantic
            ${EGL_CFLAGS_OTHER}
        )
        list(APPEND TEXLINK_INSTALL_TARGETS texlink_egl)
    endif()

    if(Vulkan_FOUND)
        add_library(texlink_vulkan src/vulkan.c)
        target_link_libraries(texlink_vulkan
            PUBLIC texlink
                   Vulkan::Vulkan
        )
        target_include_directories(texlink_vulkan
            PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:include>
                    ${Vulkan_INCLUDE_DIRS}
        )
        target_compile_options(texlink_vulkan PRIVATE
            -Wall -Wextra -Wpedantic
        )
        list(APPEND TEXLINK_INSTALL_TARGETS texlink_vulkan)
    endif()
endif()

if(TEXLINK_BUILD_EXAMPLES)
    foreach(example simple_producer simple_consumer)
        add_executable(${example} examples/${example}.c)
        target_link_libraries(${example} PRIVATE texlink)
        target_include_directories(${example} PRIVATE include)
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endforeach()

    if(TARGET texlink_egl AND GLFW3_FOUND)
        foreach(example egl_tex_producer egl_tex_consumer)
            add_executable(${example} examples/${example}.c)
            target_link_libraries(${example} PRIVATE
                texlink_egl
                ${GLFW3_LIBRARIES}
                GLEW::GLEW
                m
            )
            target_include_directories(${example} PRIVATE
                include
                ${GLFW3_INCLUDE_DIRS}
                ${EGL_INCLUDE_DIRS}
            )
            target_compile_options(${example} PRIVATE
                -Wall -Wextra
                ${GLFW3_CFLAGS_OTHER}
                ${EGL_CFLAGS_OTHER}
            )
        endforeach()
    endif()

    if(TARGET texlink_vulkan AND GLFW3_FOUND)
        foreach(example vulkan_tex_producer vulkan_tex_consumer)
            add_executable(${example} examples/${example}.c)
            target_link_libraries(${example} PRIVATE
                    texlink_vulkan
                    ${GLFW3_LIBRARIES}
                    m
            )
            target_include_directories(${example} PRIVATE
                    include
                    ${GLFW3_INCLUDE_DIRS}
                    ${Vulkan_INCLUDE_DIRS}
            )
            target_compile_definitions(${example} PRIVATE
                    GLFW_INCLUDE_VULKAN
            )
            target_compile_options(${example} PRIVATE
                    -Wall -Wextra -Wpedantic
                    ${GLFW3_CFLAGS_OTHER}
            )
        endforeach()
    endif()
endif()

include(GNUInstallDirs)

install(TARGETS ${TEXLINK_INSTALL_TARGETS}
    EXPORT  texlink-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    include/texlink.h
    include/texlink_egl.h
    include/texlink_vulkan.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT texlink-targets
    FILE      texlink-config.cmake
    NAMESPACE texlink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/texlink
)
