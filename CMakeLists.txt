cmake_minimum_required(VERSION 3.16)

project(vuds VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(VUDS_ENABLE_SANITIZERS "Enable ASan and UBSan in Debug builds" ON)

add_executable(vuds_ecu
    src/main.c
    src/can_driver.c
    src/isotp.c
    src/uds_engine.c
    src/uds_services.c
    src/uds_data.c
)

target_include_directories(vuds_ecu PRIVATE include)
target_compile_options(vuds_ecu PRIVATE -Wall -Wextra -Werror)

if(VUDS_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(vuds_ecu PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(vuds_ecu PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(isotp_rx_harness
        tests/isotp_rx_harness.c
        src/can_driver.c
        src/isotp.c
        src/uds_engine.c
        src/uds_services.c
        src/uds_data.c
    )
    target_include_directories(isotp_rx_harness PRIVATE include)
    target_compile_options(isotp_rx_harness PRIVATE -Wall -Wextra -Werror)
    if(VUDS_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(isotp_rx_harness PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(isotp_rx_harness PRIVATE
            -fsanitize=address,undefined
        )
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME python_tests
            COMMAND ${Python3_EXECUTABLE} -m pytest tests/ -v
                    --ecu-binary=$<TARGET_FILE:vuds_ecu>
                    --isotp-harness=$<TARGET_FILE:isotp_rx_harness>
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    endif()
endif()
