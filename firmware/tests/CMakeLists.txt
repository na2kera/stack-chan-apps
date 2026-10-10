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

# app_roulette の hw/light_pattern (頭部 LED の色の計算)
add_executable(light_pattern_test
    light_pattern_test.cpp
    ../main/apps/app_roulette/hw/light_pattern.cpp
)

target_include_directories(light_pattern_test PRIVATE
    ../main
)

target_compile_features(light_pattern_test PRIVATE cxx_std_17)

add_test(NAME light_pattern_test COMMAND $<TARGET_FILE:light_pattern_test>)

# app_photobooth の flow/time_format (写真の期限の表示)
add_executable(time_format_test
    time_format_test.cpp
    ../main/apps/app_photobooth/flow/time_format.cpp
)

target_include_directories(time_format_test PRIVATE
    ../main
)

target_compile_features(time_format_test PRIVATE cxx_std_17)

add_test(NAME time_format_test COMMAND $<TARGET_FILE:time_format_test>)

# app_photobooth の net/edge_url (EDGE_BASE_URL の分解)
add_executable(edge_url_test
    edge_url_test.cpp
    ../main/apps/app_photobooth/net/edge_url.cpp
)

target_include_directories(edge_url_test PRIVATE
    ../main
)

target_compile_features(edge_url_test PRIVATE cxx_std_17)

add_test(NAME edge_url_test COMMAND $<TARGET_FILE:edge_url_test>)

# app_photobooth の net/link_logic (接続の失敗の分類、タイムアウト表、接続状態)
add_executable(link_logic_test
    link_logic_test.cpp
    ../main/apps/app_photobooth/net/link_logic.cpp
)

target_include_directories(link_logic_test PRIVATE
    ../main
)

target_compile_features(link_logic_test PRIVATE cxx_std_17)

add_test(NAME link_logic_test COMMAND $<TARGET_FILE:link_logic_test>)

# app_photobooth の net/edge_config.h (config_local.h の書き方の受け付けと旧形式の検出)。
# フィクスチャごとに config.h / net/edge_config.h / config_local.h を並べたディレクトリを作ってコンパイルする。
set(PB_APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../main/apps/app_photobooth)
function(edge_config_fixture name)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/edge_config_${name})
    configure_file(${PB_APP_DIR}/config.h ${dir}/config.h COPYONLY)
    configure_file(${PB_APP_DIR}/net/edge_config.h ${dir}/net/edge_config.h COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/edge_config_fixtures/${name}.h ${dir}/config_local.h COPYONLY)
    set(EDGE_CONFIG_FIXTURE_DIR ${dir} PARENT_SCOPE)
endfunction()

# 通るはずの書き方: 実行して値を比べる
foreach(pair "namespace|http://192.168.0.167:8765" "global|https://edge.example.com"
             "define|https://edge.example.com:8443" "both|http://192.168.0.167:8765")
    string(REPLACE "|" ";" parts ${pair})
    list(GET parts 0 name)
    list(GET parts 1 url)
    edge_config_fixture(${name})
    add_executable(edge_config_test_${name} edge_config_test.cpp)
    target_include_directories(edge_config_test_${name} PRIVATE ${EDGE_CONFIG_FIXTURE_DIR})
    target_compile_definitions(edge_config_test_${name} PRIVATE EXPECTED_BASE_URL="${url}" FIXTURE_NAME="${name}")
    target_compile_features(edge_config_test_${name} PRIVATE cxx_std_17)
    add_test(NAME edge_config_test_${name} COMMAND $<TARGET_FILE:edge_config_test_${name}>)
endforeach()

# 止まるはずの書き方: コンパイラのエラー文言を確かめる (終了コードではなく文言で合否を決める)
set(EDGE_CONFIG_SYSROOT_ARGS)
if(CMAKE_OSX_SYSROOT)
    set(EDGE_CONFIG_SYSROOT_ARGS -isysroot ${CMAKE_OSX_SYSROOT})
endif()
foreach(pair "old_namespace|EDGE_HOST/EDGE_PORT" "old_global|EDGE_HOST/EDGE_PORT"
             "old_define|EDGE_HOST/EDGE_PORT" "missing|config_local.example.h")
    string(REPLACE "|" ";" parts ${pair})
    list(GET parts 0 name)
    list(GET parts 1 expected)
    edge_config_fixture(${name})
    add_test(NAME edge_config_rejects_${name}
             COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -fsyntax-only ${EDGE_CONFIG_SYSROOT_ARGS}
                     -I${EDGE_CONFIG_FIXTURE_DIR} -DFIXTURE_NAME="${name}"
                     ${CMAKE_CURRENT_SOURCE_DIR}/edge_config_test.cpp)
    set_tests_properties(edge_config_rejects_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
endforeach()
