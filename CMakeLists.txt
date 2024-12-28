cmake_minimum_required(VERSION 3.28)
project(ferrum)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -static")

include(get_cpm.cmake)

CPMAddPackage("gh:nlohmann/json@3.11.3")
CPMAddPackage(
        NAME cpr
        GITHUB_REPOSITORY libcpr/cpr
        GIT_TAG bb01c8db702fb41e5497aee9c0559ddf4bf13749
        OPTIONS
        "BUILD_SHARED_LIBS OFF"
        "CPR_FORCE_USE_SYSTEM_CURL OFF"
        "CPR_BUILD_TESTS OFF"
        "CPR_ENABLE_SSL ON"
)

set(target_sources
        main.cpp
)
add_executable(ferrum ${target_sources})
target_link_libraries(ferrum nlohmann_json::nlohmann_json cpr::cpr)
