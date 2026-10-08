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

# shared/wav (埋め込み WAV のヘッダ解析)
add_executable(wav_test
    wav_test.cpp
    ../main/apps/shared/wav.cpp
)

target_include_directories(wav_test PRIVATE
    ../main
)

target_compile_features(wav_test PRIVATE cxx_std_17)

add_test(NAME wav_test COMMAND $<TARGET_FILE:wav_test>)

# app_photobooth の view/jpeg_info (JPEG のヘッダ解析)
add_executable(jpeg_info_test
    jpeg_info_test.cpp
    ../main/apps/app_photobooth/view/jpeg_info.cpp
)

target_include_directories(jpeg_info_test PRIVATE
    ../main
)

target_compile_features(jpeg_info_test PRIVATE cxx_std_17)

add_test(NAME jpeg_info_test COMMAND $<TARGET_FILE:jpeg_info_test>)
