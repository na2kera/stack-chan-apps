cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

# app_roulette のゲームの状態 (LVGL / HAL に依存しない部分)
add_executable(roulette_slot_test
    roulette_slot_test.cpp
    ../main/apps/app_roulette/game/slot.cpp
)

target_include_directories(roulette_slot_test PRIVATE
    ../main
)

target_compile_features(roulette_slot_test PRIVATE cxx_std_17)

add_test(NAME roulette_slot_test COMMAND $<TARGET_FILE:roulette_slot_test>)
