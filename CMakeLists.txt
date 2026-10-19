cmake_minimum_required(VERSION 3.24)
project(TtpClienc LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_clienc requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(cmake/version.cmake)
FetchContent_Declare(tinyxml2 URL https://codeload.github.com/leethomason/tinyxml2/zip/refs/tags/11.0.0
  URL_HASH SHA256=1c35b8d8a586125d5e43380fd2137abbfde645183ed1641f86c815359320412f DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "No upstream tests in distribution" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static XML parser" FORCE)
FetchContent_MakeAvailable(tinyxml2)
add_library(ttp_clienc SHARED src/plugin.cpp src/encoder.cpp src/settings.cpp src/settings.rc
  src/ttp_clienc.def "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_clienc ttp_clienc_version)
target_include_directories(ttp_clienc PUBLIC include PRIVATE src)
target_compile_definitions(ttp_clienc PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_clienc PRIVATE /W4 /permissive- /EHsc /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_clienc PRIVATE tinyxml2 ole32 uuid shlwapi comdlg32)
set_target_properties(ttp_clienc PROPERTIES PREFIX "" OUTPUT_NAME "ttp_clienc")
add_custom_command(TARGET ttp_clienc POST_BUILD COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py" "$<TARGET_FILE:ttp_clienc>"
  --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
  --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
  --report "$<TARGET_FILE_DIR:ttp_clienc>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_clienc RUNTIME DESTINATION AddIn COMPONENT Runtime)
