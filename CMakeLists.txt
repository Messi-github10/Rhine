cmake_minimum_required(VERSION 3.22)
project(Rhine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
set(CMAKE_PREFIX_PATH "$ENV{HOME}/Tools/Qt/qt/6.8.0/gcc_64")
find_package(Qt6 REQUIRED COMPONENTS Quick Qml)

# FFmpeg via vcpkg (static libs)
set(VCPKG_INSTALLED "$ENV{HOME}/Code/Github/vcpkg/installed/x64-linux")
list(APPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED}/share/ffmpeg")
find_package(FFMPEG REQUIRED)

# Source files
set(SOURCES
    App/main.cpp
    Player/decoder.cpp
    Player/PlayerController.cpp
    Render/renderer.cpp
    UI/videodisplay.cpp
)

set(HEADERS
    Player/framequeue.h
    Player/decoder.h
    Player/PlayerController.h
    Render/renderer.h
    UI/videodisplay.h
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
)

target_include_directories(RhineApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/App
    ${CMAKE_CURRENT_SOURCE_DIR}/Player
    ${CMAKE_CURRENT_SOURCE_DIR}/Render
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
    ${FFMPEG_INCLUDE_DIRS}
)