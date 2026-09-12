cmake_minimum_required(VERSION 3.20)
project(cortex_native VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(CORTEX_SOURCES
    src/core.cpp
    src/tokenizer.cpp
    src/neural_model.cpp
    src/memory.cpp
    src/quality.cpp
    src/knowledge.cpp
    src/native_cognition.cpp
    src/agent.cpp
    src/connectivity.cpp
    src/local_model.cpp
    src/reasoner.cpp
    src/self_editor.cpp
    src/web_server.cpp
    src/web_ui.cpp
)

add_library(cortex-core STATIC ${CORTEX_SOURCES})
target_include_directories(cortex-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cortex-core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(cortex-core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(cortex-core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(WIN32)
    target_link_libraries(cortex-core PUBLIC ws2_32 shell32)
endif()

add_executable(cortex src/main.cpp)
target_link_libraries(cortex PRIVATE cortex-core)

add_executable(cortex-tests tests/test_main.cpp)
target_link_libraries(cortex-tests PRIVATE cortex-core)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cortex-native-suite COMMAND cortex-tests)
endif()

install(TARGETS cortex RUNTIME DESTINATION bin)
