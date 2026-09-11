cmake_minimum_required(VERSION 3.22)
project(werfeed_native_engine VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.8
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

option(WERFEED_ENABLE_ASIO "Enable ASIO support with a locally supplied Steinberg ASIO SDK" OFF)
set(WERFEED_ASIO_SDK_PATH "$ENV{ASIO_SDK_PATH}" CACHE PATH "Path to the ASIO SDK root (contains common/asio.h)")

add_executable(werfeed-native src/main.cpp)
set_target_properties(werfeed-native PROPERTIES OUTPUT_NAME "werfeed-engine")
target_compile_features(werfeed-native PRIVATE cxx_std_20)
target_link_libraries(werfeed-native PRIVATE
  juce::juce_audio_devices
  juce::juce_audio_basics
  juce::juce_audio_formats
  juce::juce_events)
target_compile_definitions(werfeed-native PRIVATE
  JUCE_APPLICATION_NAME_STRING="Werfeed Native Engine"
  JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_DISPLAY_SPLASH_SCREEN=0)

if (WIN32)
  target_compile_definitions(werfeed-native PRIVATE
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1)
  if (WERFEED_ENABLE_ASIO)
    if (NOT EXISTS "${WERFEED_ASIO_SDK_PATH}/common/asio.h")
      message(FATAL_ERROR "WERFEED_ENABLE_ASIO requires WERFEED_ASIO_SDK_PATH with common/asio.h.")
    endif()
    target_include_directories(werfeed-native PRIVATE
      "${WERFEED_ASIO_SDK_PATH}/common"
      "${WERFEED_ASIO_SDK_PATH}/host"
      "${WERFEED_ASIO_SDK_PATH}/host/pc")
    target_compile_definitions(werfeed-native PRIVATE JUCE_ASIO=1)
  else()
    target_compile_definitions(werfeed-native PRIVATE JUCE_ASIO=0)
  endif()
elseif (WERFEED_ENABLE_ASIO)
  message(FATAL_ERROR "ASIO support is only available in the Windows native build.")
endif()

include(CTest)
if (BUILD_TESTING)
  add_executable(routing-tests tests/routing_tests.cpp)
  target_compile_features(routing-tests PRIVATE cxx_std_20)
  target_include_directories(routing-tests PRIVATE src)
  add_test(NAME routing-tests COMMAND routing-tests)
  add_executable(dsp-tests tests/dsp_tests.cpp)
  target_compile_features(dsp-tests PRIVATE cxx_std_20)
  target_include_directories(dsp-tests PRIVATE src)
  add_test(NAME dsp-tests COMMAND dsp-tests)
  add_executable(device-policy-tests tests/device_policy_tests.cpp)
  target_compile_features(device-policy-tests PRIVATE cxx_std_20)
  target_include_directories(device-policy-tests PRIVATE src)
  add_test(NAME device-policy-tests COMMAND device-policy-tests)
endif()