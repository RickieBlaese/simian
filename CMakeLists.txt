cmake_minimum_required(VERSION 3.25)

project(simian VERSION 0.3 DESCRIPTION "monkeytype in terminal")

find_package(rapidfuzz REQUIRED)


add_subdirectory(libs/rapidjson)
 
# Create an executable
add_executable(${PROJECT_NAME}
    src/simian.cc
)
 
# Linking libraries
target_link_libraries(${PROJECT_NAME}
    menu
    ncursesw
    tinfo
    ssl
    crypto
    rapidfuzz::rapidfuzz
)

# These are output from ncursesw5-config --cflags --libs
add_compile_definitions(DEFAULT_SOURCE XOPEN_SOURCE=600)
 
# Specify includes
target_include_directories(${PROJECT_NAME} PRIVATE
    libs/rapidjson/include
    libs
)



 
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
