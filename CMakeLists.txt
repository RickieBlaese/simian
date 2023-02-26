cmake_minimum_required(VERSION 3.25)

project(simian VERSION 0.3 DESCRIPTION "monkeytype in terminal")



# include(FetchContent)
# FetchContent_Declare(
#   rapidfuzz
#   SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/RapidFuzz
#   PREFIX ${CMAKE_CURRENT_BINARY_DIR}/rapidfuzz
#   CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR> "${CMAKE_OPT_ARGS}"
# )
# FetchContent_MakeAvailable(rapidfuzz)

add_subdirectory(3rdparty/RapidFuzz)

# Create an executable
add_executable(${PROJECT_NAME}
    src/simian.cc
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
