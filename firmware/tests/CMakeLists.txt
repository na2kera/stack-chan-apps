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

# ArduinoJson (ヘッダのみ)。実機と同じ components/ArduinoJson (fetch_repos.py が取る) があればそれを使い、
# 無ければ (CI は fetch_repos.py を実行しない) FetchContent で取る。バージョンは repos.json と同じ v7.4.2 にすること。
set(ARDUINOJSON_LOCAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/ArduinoJson)
if(EXISTS ${ARDUINOJSON_LOCAL_DIR}/src/ArduinoJson.h)
    set(ARDUINOJSON_INCLUDE_DIR ${ARDUINOJSON_LOCAL_DIR}/src)
else()
    include(FetchContent)
    FetchContent_Declare(arduinojson
        GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
        GIT_TAG v7.4.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(arduinojson)
    set(ARDUINOJSON_INCLUDE_DIR ${arduinojson_SOURCE_DIR}/src)
endif()

# app_photobooth の net/edge_parse (edge の応答の解釈)
add_executable(edge_parse_test
    edge_parse_test.cpp
    ../main/apps/app_photobooth/net/edge_parse.cpp
)

target_include_directories(edge_parse_test PRIVATE
    ../main
    ${ARDUINOJSON_INCLUDE_DIR}
)

target_compile_features(edge_parse_test PRIVATE cxx_std_17)

add_test(NAME edge_parse_test COMMAND $<TARGET_FILE:edge_parse_test>)

# app_photobooth の hw/head_logic (首の応答監視。サーボは偽物)
add_executable(head_logic_test
    head_logic_test.cpp
    ../main/apps/app_photobooth/hw/head_logic.cpp
)

target_include_directories(head_logic_test PRIVATE
    ../main
)

target_compile_features(head_logic_test PRIVATE cxx_std_17)

add_test(NAME head_logic_test COMMAND $<TARGET_FILE:head_logic_test>)
