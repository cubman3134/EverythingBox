cmake_minimum_required(VERSION 3.21)
project(crash28repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Network QmlModels)

add_executable(crash28repro main.cpp)
target_link_libraries(crash28repro PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::QmlModels
    Qt6::QuickPrivate Qt6::QmlPrivate Qt6::CorePrivate Qt6::GuiPrivate
    Qt6::QmlModelsPrivate)

if (MSVC)
    # Release: keep frame pointers + full symbols so a fault is easy to place.
    target_compile_options(crash28repro PRIVATE /Zi)
    target_link_options(crash28repro PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
