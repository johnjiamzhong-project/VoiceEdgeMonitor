cmake_minimum_required(VERSION 3.16)

project(VoiceEdgeMonitor VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOICEEDGE_BUILD_ALSA "Build ALSA capture targets" ON)
option(VOICEEDGE_BUILD_WS "Build WebSocket targets" OFF)
option(VOICEEDGE_BUILD_ASR "Build sherpa-onnx offline ASR target" OFF)
set(SHERPA_ONNX_ROOT "" CACHE PATH "Prefix of the target-board sherpa-onnx runtime")

if(VOICEEDGE_BUILD_ALSA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_executable(capture_probe
        board/capture_probe.cpp
    )

    target_compile_options(capture_probe PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_link_libraries(capture_probe PRIVATE PkgConfig::ALSA)

    add_executable(pipeline_probe
        board/pipeline_probe.cpp
    )

    target_compile_options(pipeline_probe PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_link_libraries(pipeline_probe PRIVATE PkgConfig::ALSA)

    install(TARGETS capture_probe pipeline_probe RUNTIME DESTINATION bin)
endif()

if(VOICEEDGE_BUILD_WS)
    find_path(VOICEEDGE_BOOST_INCLUDE_DIR NAMES beast.hpp
              PATH_SUFFIXES boost
              HINTS /usr/include /usr/local/include
              REQUIRED)
    find_package(Threads REQUIRED)

    add_executable(ws_cli
        tools/ws_cli.cpp
    )

    target_compile_options(ws_cli PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_definitions(ws_cli PRIVATE
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_SYSTEM_NO_DEPRECATED
    )

    target_include_directories(ws_cli PRIVATE ${VOICEEDGE_BOOST_INCLUDE_DIR})

    target_link_libraries(ws_cli PRIVATE
        Threads::Threads
    )

    if(VOICEEDGE_BUILD_ALSA)
        add_executable(ws_server
            board/ws_server.cpp
        )

        target_compile_options(ws_server PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )

        target_compile_definitions(ws_server PRIVATE
            BOOST_ERROR_CODE_HEADER_ONLY
            BOOST_SYSTEM_NO_DEPRECATED
        )

        target_include_directories(ws_server PRIVATE ${VOICEEDGE_BOOST_INCLUDE_DIR})

        target_link_libraries(ws_server PRIVATE
            PkgConfig::ALSA
            Threads::Threads
        )

        install(TARGETS ws_server RUNTIME DESTINATION bin)
    endif()

    install(TARGETS ws_cli RUNTIME DESTINATION bin)
endif()

if(VOICEEDGE_BUILD_ASR)
    if(NOT SHERPA_ONNX_ROOT)
        message(FATAL_ERROR "VOICEEDGE_BUILD_ASR=ON requires -DSHERPA_ONNX_ROOT=...")
    endif()
    find_package(Threads REQUIRED)

    find_path(SHERPA_ONNX_INCLUDE_DIR NAMES sherpa-onnx/c-api/c-api.h
              HINTS "${SHERPA_ONNX_ROOT}/include"
              REQUIRED)
    find_library(SHERPA_ONNX_C_API_LIBRARY NAMES sherpa-onnx-c-api
                 HINTS "${SHERPA_ONNX_ROOT}/lib"
                 REQUIRED)

    if(TARGET pipeline_probe)
        target_compile_definitions(pipeline_probe PRIVATE VOICEEDGE_HAS_ASR=1)
        target_include_directories(pipeline_probe PRIVATE "${SHERPA_ONNX_INCLUDE_DIR}")
        target_link_libraries(pipeline_probe PRIVATE "${SHERPA_ONNX_C_API_LIBRARY}" Threads::Threads)
        set_target_properties(pipeline_probe PROPERTIES
            BUILD_RPATH "${SHERPA_ONNX_ROOT}/lib"
            INSTALL_RPATH "${SHERPA_ONNX_ROOT}/lib"
        )
    endif()

    if(TARGET ws_server)
        target_compile_definitions(ws_server PRIVATE VOICEEDGE_HAS_ASR=1)
        target_include_directories(ws_server PRIVATE "${SHERPA_ONNX_INCLUDE_DIR}")
        target_link_libraries(ws_server PRIVATE "${SHERPA_ONNX_C_API_LIBRARY}" Threads::Threads)
        set_target_properties(ws_server PROPERTIES
            BUILD_RPATH "${SHERPA_ONNX_ROOT}/lib"
            INSTALL_RPATH "${SHERPA_ONNX_ROOT}/lib"
        )
    endif()

    add_executable(asr_probe
        board/asr_probe.cpp
    )

    target_compile_options(asr_probe PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_include_directories(asr_probe PRIVATE "${SHERPA_ONNX_INCLUDE_DIR}")
    target_link_libraries(asr_probe PRIVATE "${SHERPA_ONNX_C_API_LIBRARY}" Threads::Threads)
    set_target_properties(asr_probe PROPERTIES
        BUILD_RPATH "${SHERPA_ONNX_ROOT}/lib"
        INSTALL_RPATH "${SHERPA_ONNX_ROOT}/lib"
    )
    install(TARGETS asr_probe RUNTIME DESTINATION bin)
endif()

add_executable(bounded_queue_test
    tests/bounded_queue_test.cpp
)

add_executable(protocol_test
    tests/protocol_test.cpp
)

add_executable(energy_vad_test
    tests/energy_vad_test.cpp
)

target_compile_options(bounded_queue_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(protocol_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(energy_vad_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME bounded_queue_test COMMAND bounded_queue_test)
    add_test(NAME protocol_test COMMAND protocol_test)
    add_test(NAME energy_vad_test COMMAND energy_vad_test)
    if(VOICEEDGE_BUILD_ALSA)
        add_test(NAME capture_probe_help COMMAND capture_probe --help)
        add_test(NAME pipeline_probe_help COMMAND pipeline_probe --help)
        if(VOICEEDGE_BUILD_WS)
            add_test(NAME ws_server_help COMMAND ws_server --help)
        endif()
    endif()
    if(VOICEEDGE_BUILD_WS)
        add_test(NAME ws_cli_help COMMAND ws_cli --help)
    endif()
    if(VOICEEDGE_BUILD_ASR)
        add_test(NAME asr_probe_help COMMAND asr_probe --help)
    endif()
endif()
