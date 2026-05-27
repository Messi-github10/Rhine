cmake_minimum_required(VERSION 3.22)
project(Rhine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
set(CMAKE_PREFIX_PATH "$ENV{HOME}/Tools/Qt/qt/6.8.0/gcc_64")
find_package(Qt6 REQUIRED COMPONENTS Quick Qml)
set(QT6_PREFIX "$ENV{HOME}/Tools/Qt/qt/6.8.0/gcc_64")

# FFmpeg via vcpkg (static libs)
set(VCPKG_INSTALLED "$ENV{HOME}/Code/Github/vcpkg/installed/x64-linux")
list(APPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED}/share/ffmpeg")
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED}")
find_package(FFMPEG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# libplacebo
set(LIBPLACEBO_ROOT "$ENV{HOME}/Code/Github/libplacebo")
set(LIBPLACEBO_INCLUDE "${LIBPLACEBO_ROOT}/src/include")
set(LIBPLACEBO_LIB "${LIBPLACEBO_ROOT}/build/src/libplacebo.so")
# Vulkan SDK (needed for vulkan.h by libplacebo)
set(VULKAN_SDK "$ENV{HOME}/Tools/VulkanSDK/1.4.341.1/x86_64")
# SPIRV lib needed by libplacebo
set(SPIRV_LIB "${VULKAN_SDK}/lib/libSPIRV.so")

# Source files
set(SOURCES
    App/main.cpp
    Demuxer/Demuxer.cpp
    Decoder/CpuDecoder.cpp
    Decoder/VulkanDecoder.cpp
    Player/PlayerController.cpp
    Render/renderer.cpp
    Render/Vulkan/VulkanContext.cpp
    Render/Vulkan/VulkanRenderer.cpp
    Render/Vulkan/libav_impl.c
    UI/videodisplay.cpp
    UI/VulkanDisplay.cpp
    Utils/Logger.cpp
)

set(HEADERS
    Demuxer/Demuxer.hpp
    Player/FrameQueue.hpp
    Decoder/IDecoder.hpp
    Decoder/CpuDecoder.hpp
    Decoder/VulkanDecoder.hpp
    Player/PlayerController.hpp
    Render/IRenderer.hpp
    Render/renderer.hpp
    Render/Vulkan/VulkanContext.hpp
    Render/Vulkan/VulkanRenderer.hpp
    UI/videodisplay.hpp
    UI/VulkanDisplay.hpp
    Utils/Logger.hpp
)

# Executable
qt_add_executable(RhineApp ${SOURCES} ${HEADERS})

# QML module
qt_add_qml_module(RhineApp
    URI Rhine
    VERSION 1.0
    QML_FILES UI/Main.qml
)

target_link_libraries(RhineApp PRIVATE
    Qt6::Quick
    Qt6::Qml
    ${FFMPEG_LIBRARIES}
    concurrentqueue::concurrentqueue
    spdlog::spdlog
    ${LIBPLACEBO_LIB}
    ${SPIRV_LIB}
    ${VULKAN_SDK}/lib/libvulkan.so
    glslang-default-resource-limits
)

target_include_directories(RhineApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/App
    ${CMAKE_CURRENT_SOURCE_DIR}/Decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/Demuxer
    ${CMAKE_CURRENT_SOURCE_DIR}/Player
    ${CMAKE_CURRENT_SOURCE_DIR}/Render
    ${CMAKE_CURRENT_SOURCE_DIR}/Render/Vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBPLACEBO_INCLUDE}
    ${VULKAN_SDK}/include
    # Qt6 private headers (for QPlatformNativeInterface)
    ${QT6_PREFIX}/include/QtGui/6.8.0
    ${QT6_PREFIX}/include/QtCore/6.8.0
)