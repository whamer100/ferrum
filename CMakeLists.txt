cmake_minimum_required(VERSION 3.28)
project(ferrum)

if (${CMAKE_BUILD_TYPE} MATCHES Debug)
    add_compile_definitions(__DEBUG__)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -O3 -static")

include(get_cpm.cmake)

CPMAddPackage("gh:nlohmann/json@3.11.3")
CPMAddPackage(
        NAME cpr
        GITHUB_REPOSITORY libcpr/cpr
        GIT_TAG 1.11.1
        OPTIONS
        "BUILD_SHARED_LIBS OFF"
        "CPR_FORCE_USE_SYSTEM_CURL OFF"
        "CPR_BUILD_TESTS OFF"
        "CPR_ENABLE_SSL ON"
)
#CPMAddPackage(
#        NAME ZipLib
#        GITHUB_REPOSITORY DreamyCecil/ZipLib
#        GIT_TAG master
#)

set(target_sources
        main.cpp
        utils.h
)
set(vendor_sources
        # https://github.com/tfussell/miniz-cpp
        # vendor/zip_file.hpp
        # https://github.com/nyq/miniz-cpp
        # vendor/miniz-cpp/miniz_cpp.hpp

        # https://github.com/richgel999/miniz  # 3.0.2
        vendor/miniz.c
        vendor/miniz.h
)
add_executable(ferrum ${target_sources} ${vendor_sources})
target_link_libraries(ferrum PRIVATE nlohmann_json::nlohmann_json cpr::cpr)
