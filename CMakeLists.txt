cmake_minimum_required(VERSION 3.25)
project(glance CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Qml)
qt_standard_project_setup()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/mupdf/build/release/libmupdf.a")
    message(FATAL_ERROR "MuPDF not built — run: make -C third_party/mupdf build=release libs")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/mupdf/build/release/libmupdf-third.a")
    set(MUPDF_THIRD "${CMAKE_SOURCE_DIR}/third_party/mupdf/build/release/libmupdf-third.a")
else()
    set(MUPDF_THIRD "")
endif()

qt_add_executable(glance
    src/main.cpp
    src/document.cpp
    src/pageprovider.cpp
    src/ocrmanager.cpp
    src/inputstate.cpp
    src/theme.cpp
)
target_compile_definitions(glance PRIVATE
    QML_MAIN_SOURCE_S="${CMAKE_SOURCE_DIR}/qml/Main.qml")
target_include_directories(glance PRIVATE third_party/mupdf/include)
target_link_libraries(glance PRIVATE
    Qt6::Quick Qt6::Qml
    "${CMAKE_SOURCE_DIR}/third_party/mupdf/build/release/libmupdf.a"
    ${MUPDF_THIRD}
    pthread m)
