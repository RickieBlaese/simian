cmake_minimum_required(VERSION 3.25)


project(simian VERSION 0.3 DESCRIPTION "monkeytype in terminal")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

include(FetchContent)
include(ExternalProject)

FetchContent_Declare(rapidfuzz
    GIT_REPOSITORY https://github.com/maxbachmann/rapidfuzz-cpp.git
    GIT_TAG main
)
FetchContent_MakeAvailable(rapidfuzz)

ExternalProject_Add(rapidfuzz-cpp
    PREFIX libs
    GIT_REPOSITORY https://github.com/maxbachmann/rapidfuzz-cpp.git
    GIT_TAG main
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    TEST_COMMAND ""
)

ExternalProject_Add(cpp-httplib
    PREFIX libs
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG master
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    TEST_COMMAND ""
)

ExternalProject_Add(rapidjson
    PREFIX libs
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    GIT_TAG master
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    TEST_COMMAND ""
)

# Create an executable
add_executable(${PROJECT_NAME}
    src/simian.cc
)
 
# Specify includes
target_include_directories(${PROJECT_NAME} PRIVATE
    .
)
 
# Linking libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ncursesw
    tinfo
    ssl
    crypto
    rapidfuzz::rapidfuzz
)

# These are output from ncursesw5-config --cflags --libs
add_compile_definitions(DEFAULT_SOURCE XOPEN_SOURCE=600)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
