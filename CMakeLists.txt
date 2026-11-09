cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-audio
    VERSION 0.1.0
    DESCRIPTION "PolyMech portable audio"
    LANGUAGES CXX)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# Standalone builds compile the clients. A parent must opt in before add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_PM_AUDIO_CLIENTS_DEFAULT ON)
else()
  set(_PM_AUDIO_CLIENTS_DEFAULT OFF)
endif()
option(PM_AUDIO_SPEECH_CLIENTS "ElevenLabs HTTP clients and the voice-over timeline" ${_PM_AUDIO_CLIENTS_DEFAULT})
option(PM_AUDIO_SOUND_TRIGGER "PCM trigger and double-whistle detector" ${_PM_AUDIO_CLIENTS_DEFAULT})
option(PM_AUDIO_FILTER "File filters: decode, DeepFilterNet, GTCRN flag" ${_PM_AUDIO_CLIENTS_DEFAULT})

# Whisper.cpp and ONNX stay Tanit's. This package does not FetchContent them.
# A parent that already created `whisper` or `onnxruntime_imported` wins.
# The product root sets PM_AUDIO_TANIT_ROOT to its source tree. Empty skips the search.
set(PM_AUDIO_TANIT_ROOT "" CACHE PATH
  "Optional product tree with third_party/onnxruntime-* and model folders. Empty skips that search.")
set(PM_AUDIO_ONNX_ROOT "" CACHE PATH
  "onnxruntime tree with include/onnxruntime_cxx_api.h. Empty searches Tanit third_party.")
set(PM_AUDIO_WHISPER_ROOT "" CACHE PATH
  "whisper.cpp source tree. Empty uses the whisper target when Tanit already built it.")
if(NOT PM_AUDIO_ONNX_ROOT AND NOT TARGET onnxruntime_imported)
  file(GLOB _pm_audio_onnx_headers
    "${PM_AUDIO_TANIT_ROOT}/third_party/onnxruntime-*/include/onnxruntime_cxx_api.h")
  if(_pm_audio_onnx_headers)
    list(SORT _pm_audio_onnx_headers)
    list(GET _pm_audio_onnx_headers -1 _pm_audio_onnx_header)
    cmake_path(GET _pm_audio_onnx_header PARENT_PATH _pm_audio_onnx_inc)
    cmake_path(GET _pm_audio_onnx_inc PARENT_PATH PM_AUDIO_ONNX_ROOT)
  endif()
endif()
if(NOT DEFINED PM_AUDIO_HAS_ONNX)
  if(TARGET onnxruntime_imported OR
     (PM_AUDIO_ONNX_ROOT AND EXISTS "${PM_AUDIO_ONNX_ROOT}/include/onnxruntime_cxx_api.h"))
    set(PM_AUDIO_HAS_ONNX ON)
  else()
    set(PM_AUDIO_HAS_ONNX OFF)
  endif()
endif()
set(PM_AUDIO_HAS_WHISPER OFF)
if(TARGET whisper)
  set(PM_AUDIO_HAS_WHISPER ON)
elseif(PM_AUDIO_WHISPER_ROOT AND EXISTS "${PM_AUDIO_WHISPER_ROOT}/include/whisper.h")
  set(PM_AUDIO_HAS_WHISPER ON)
endif()

set(PM_AUDIO_SOURCES
  src/log.cpp
  src/ring.cpp
  src/session/audio_lease.cpp
  src/session/audio_route.cpp
  src/session/audio_events.cpp
  src/session/audio_graph.cpp
  src/session/audio_session_selftest.cpp
  src/subs/subtitle_writer.cpp
  src/voice/voice_types.cpp
  src/voice/voice_match.cpp
  src/voice/voice_envelope.cpp
  src/session/aaudio_capture.cpp
  src/path.cpp
  src/capture.cpp
  src/dynlib.cpp
  src/voice/voice_pipeline.cpp
  src/stt/whisper_lang.cpp
  src/stt/whisper_wav.cpp
  src/stt/whisper_probe.cpp
  src/filter/models.cpp
  src/tts/tts_text.cpp
  src/tts/md_speech.cpp
  src/tts/moss_tts.cpp
  src/stt/whisper_infer.cpp
  src/stt/windows_ai_stt.cpp
  src/tts/windows_tts.cpp
  src/echo.cpp
  src/devices.cpp
  src/pcm_record_buffer.cpp
  src/record_session.cpp
  src/mf_aac_record_writer.cpp)

function(pm_audio_ensure_miniaudio)
  if(TARGET miniaudio_hdr)
    return()
  endif()
  include(FetchContent)
  FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(miniaudio)
  if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
  endif()
  add_library(miniaudio_hdr INTERFACE)
  target_include_directories(miniaudio_hdr INTERFACE "${miniaudio_SOURCE_DIR}")
endfunction()

if(PM_AUDIO_SPEECH_CLIENTS)
  if(NOT TARGET CURL::libcurl)
    include(FetchContent)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    if(WIN32)
      set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
      set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    else()
      set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
      set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    endif()
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(ENABLE_WEBSOCKETS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(CURL
      URL https://github.com/curl/curl/releases/download/curl-8_12_1/curl-8.12.1.tar.xz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(CURL)
  endif()
  pm_audio_ensure_miniaudio()
  list(APPEND PM_AUDIO_SOURCES
    src/stt/elevenlabs.cpp
    src/tts/elevenlabs_tts.cpp
    src/tts/voiceover.cpp)
endif()

if(PM_AUDIO_SOUND_TRIGGER)
  if(NOT TARGET PFFFT::PFFFT)
    include(FetchContent)
    set(PFFFT_USE_TYPE_FLOAT ON CACHE BOOL "" FORCE)
    set(PFFFT_USE_TYPE_DOUBLE OFF CACHE BOOL "" FORCE)
    set(INSTALL_PFFFT OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Full clone: GIT_SHALLOW cannot check out this commit (it is not a branch tip).
    FetchContent_Declare(pm_pffft
      GIT_REPOSITORY https://github.com/marton78/pffft.git
      GIT_TAG b3344191adc040a0f4c8553a168ab38b3ef04367)
    FetchContent_MakeAvailable(pm_pffft)
  endif()
  list(APPEND PM_AUDIO_SOURCES
    src/trigger/sound_trigger.cpp
    src/trigger/sound_trigger_whistle.cpp)
endif()

if(PM_AUDIO_FILTER)
  pm_audio_ensure_miniaudio()
  list(APPEND PM_AUDIO_SOURCES
    src/filter/audio_filter.cpp
    src/filter/deepfilter.cpp
    src/filter/gtcrn.cpp
    src/filter/gtcrn_live.cpp
    src/filter/onnx_run.cpp
    src/voice/voice_enhancers.cpp)
endif()

pm_audio_ensure_miniaudio()

# mixed_capture.cpp is the process capture table. The host compiles it once.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  list(APPEND PM_AUDIO_SOURCES src/mixed_capture.cpp)
endif()

add_library(pm-audio-lib STATIC ${PM_AUDIO_SOURCES})
add_library(pm::audio ALIAS pm-audio-lib)
target_include_directories(pm-audio-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pm-audio-lib PUBLIC cxx_std_20)
target_link_libraries(pm-audio-lib PUBLIC nlohmann_json::nlohmann_json miniaudio_hdr)
if(WIN32)
  target_link_libraries(pm-audio-lib PUBLIC mfplat mfreadwrite mfuuid)
endif()
file(TO_CMAKE_PATH "${PM_AUDIO_TANIT_ROOT}" _pm_audio_tanit_fwd)
target_compile_definitions(pm-audio-lib PRIVATE "PM_AUDIO_TANIT_ROOT=\"${_pm_audio_tanit_fwd}\"")
if(NOT TARGET md4c AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(FetchContent)
  FetchContent_Declare(md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    GIT_TAG release-0.5.2
    GIT_SHALLOW TRUE)
  set(MD4C_USE_UTF8 ON CACHE BOOL "" FORCE)
  set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(md4c)
endif()
if(TARGET md4c)
  target_link_libraries(pm-audio-lib PUBLIC md4c)
  if(md4c_SOURCE_DIR)
    target_include_directories(pm-audio-lib PRIVATE "${md4c_SOURCE_DIR}/src")
  endif()
  target_compile_definitions(pm-audio-lib PRIVATE PM_AUDIO_HAS_MD4C=1)
endif()
if(TARGET vibevoice OR PM_AUDIO_VIBEVOICE_PLUGIN)
  target_sources(pm-audio-lib PRIVATE src/tts/vibevoice_tts.cpp)
endif()
if(TARGET vibevoice AND NOT PM_AUDIO_VIBEVOICE_PLUGIN)
  target_compile_definitions(pm-audio-lib PRIVATE PM_AUDIO_LINK_VIBEVOICE=1)
  target_link_libraries(pm-audio-lib PUBLIC vibevoice)
endif()
if(TARGET whisper AND NOT PM_AUDIO_WHISPER_PLUGIN)
  target_compile_definitions(pm-audio-lib PRIVATE PM_AUDIO_LINK_WHISPER=1)
  target_link_libraries(pm-audio-lib PUBLIC whisper)
  if(whisper_cpp_SOURCE_DIR)
    target_include_directories(pm-audio-lib PRIVATE "${whisper_cpp_SOURCE_DIR}/include")
  elseif(PM_AUDIO_WHISPER_ROOT)
    target_include_directories(pm-audio-lib PRIVATE "${PM_AUDIO_WHISPER_ROOT}/include")
  endif()
endif()
if(PM_AUDIO_SPEECH_CLIENTS)
  target_link_libraries(pm-audio-lib PUBLIC CURL::libcurl miniaudio_hdr)
endif()
if(PM_AUDIO_SOUND_TRIGGER)
  target_link_libraries(pm-audio-lib PUBLIC PFFFT::PFFFT)
endif()
if(PM_AUDIO_FILTER)
  target_link_libraries(pm-audio-lib PUBLIC miniaudio_hdr)
  if(PM_AUDIO_HAS_ONNX)
    target_compile_definitions(pm-audio-lib PRIVATE PM_AUDIO_HAS_ONNX=1)
    if(WIN32)
      target_compile_definitions(pm-audio-lib PRIVATE ORT_API_MANUAL_INIT)
    elseif(TARGET onnxruntime_imported)
      target_link_libraries(pm-audio-lib PUBLIC onnxruntime_imported)
    endif()
    if(TARGET onnxruntime_imported)
      target_include_directories(pm-audio-lib PRIVATE
        $<TARGET_PROPERTY:onnxruntime_imported,INTERFACE_INCLUDE_DIRECTORIES>)
    elseif(PM_AUDIO_ONNX_ROOT)
      target_include_directories(pm-audio-lib PRIVATE "${PM_AUDIO_ONNX_ROOT}/include")
    endif()
    if(TARGET onnxruntime_imported)
      target_compile_definitions(pm-audio-lib PRIVATE "PM_AUDIO_ONNX_ROOT=\"onnxruntime_imported\"")
    elseif(PM_AUDIO_ONNX_ROOT)
      file(TO_CMAKE_PATH "${PM_AUDIO_ONNX_ROOT}" _pm_audio_onnx_fwd)
      target_compile_definitions(pm-audio-lib PRIVATE "PM_AUDIO_ONNX_ROOT=\"${_pm_audio_onnx_fwd}\"")
    endif()
  endif()
  if(PM_AUDIO_HAS_WHISPER)
    target_compile_definitions(pm-audio-lib PRIVATE PM_AUDIO_HAS_WHISPER=1)
    if(TARGET whisper AND NOT PM_AUDIO_WHISPER_ROOT)
      target_compile_definitions(pm-audio-lib PRIVATE "PM_AUDIO_WHISPER_ROOT=\"whisper\"")
    elseif(PM_AUDIO_WHISPER_ROOT)
      file(TO_CMAKE_PATH "${PM_AUDIO_WHISPER_ROOT}" _pm_audio_whisper_fwd)
      target_compile_definitions(pm-audio-lib PRIVATE "PM_AUDIO_WHISPER_ROOT=\"${_pm_audio_whisper_fwd}\"")
    endif()
  endif()
endif()
if(MSVC)
  target_compile_definitions(pm-audio-lib PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_compile_options(pm-audio-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-audio-lib PRIVATE -Wall -Wextra)
endif()
option(PM_AUDIO_ASAN "AddressSanitizer. Off in the host build." OFF)
if(PM_AUDIO_ASAN)
  if(MSVC)
    target_compile_options(pm-audio-lib PRIVATE /fsanitize=address)
    target_link_options(pm-audio-lib PUBLIC /fsanitize=address)
  else()
    target_compile_options(pm-audio-lib PRIVATE -fsanitize=address,undefined)
    target_link_options(pm-audio-lib PUBLIC -fsanitize=address,undefined)
  endif()
endif()

if(NOT TARGET pm::commons)
  if(TARGET pm-commons-lib)
    add_library(pm::commons ALIAS pm-commons-lib)
  else()
    find_package(pm-commons CONFIG QUIET)
  endif()
endif()
if(TARGET pm::commons)
  target_link_libraries(pm-audio-lib PRIVATE pm::commons)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT TARGET CLI11::CLI11)
    include(FetchContent)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cli11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.4.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cli11)
  endif()

  add_executable(pm-audio cli/main.cpp src/miniaudio_impl.cpp)
  target_link_libraries(pm-audio PRIVATE pm-audio-lib CLI11::CLI11)
  if(PM_AUDIO_SOUND_TRIGGER)
    target_sources(pm-audio PRIVATE cli/sound_trigger.cpp)
    target_compile_definitions(pm-audio PRIVATE PM_AUDIO_SOUND_TRIGGER=1)
  endif()
  target_compile_features(pm-audio PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_definitions(pm-audio PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pm-audio PRIVATE /W3 /utf-8 /EHsc)
  else()
    target_compile_options(pm-audio PRIVATE -Wall -Wextra)
  endif()
  include(CTest)
  if(BUILD_TESTING)
    add_test(NAME pm-audio-cli
      COMMAND node tests/run.mjs
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
