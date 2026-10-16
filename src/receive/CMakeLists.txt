cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(moq2ts-receiver LANGUAGES C)
    enable_testing()
endif()

find_package(libmoq CONFIG REQUIRED COMPONENTS service)
if(NOT TARGET moq::mpeg2ts)
    message(FATAL_ERROR "moq5 was built without MPEG-2 TS support")
endif()

add_executable(moq2ts-receiver main.c TsStats.c)
target_link_libraries(moq2ts-receiver PRIVATE moq::service moq::mpeg2ts)
target_compile_features(moq2ts-receiver PRIVATE c_std_11)
install(TARGETS moq2ts-receiver RUNTIME DESTINATION bin)

add_executable(moq2ts-receiver-stats-tests ../../tests/receiver_stats_test.c TsStats.c)
add_test(NAME moq2ts-receiver-stats-tests COMMAND moq2ts-receiver-stats-tests)
add_test(NAME moq2ts-receiver-cli-smoke
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/receiver-cli-smoke.sh
                 $<TARGET_FILE:moq2ts-receiver>)
