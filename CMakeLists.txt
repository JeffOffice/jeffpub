cmake_minimum_required(VERSION 3.21)
project(JeffPub79 VERSION 0.1.33 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Network Test)

add_subdirectory(third_party)

file(GLOB_RECURSE JP_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp src/core/*.h
  src/text/*.cpp src/text/*.h
  src/render/*.cpp src/render/*.h
  src/io/*.cpp src/io/*.h
  src/templates/*.cpp src/templates/*.h)
file(GLOB_RECURSE JP_UI_SOURCES CONFIGURE_DEPENDS
  src/canvas/*.cpp src/canvas/*.h
  src/app/*.cpp src/app/*.h)

# Everything except main() lives in a library so the tests can link it.
add_library(jpcore STATIC ${JP_CORE_SOURCES} ${JP_UI_SOURCES} resources/resources.qrc)
target_include_directories(jpcore PUBLIC src)
target_link_libraries(jpcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport Qt6::Network PRIVATE jp_libmspub jp_hunspell jp_zlib)
target_compile_definitions(jpcore PUBLIC JP_VERSION="${PROJECT_VERSION}" QT_NO_KEYWORDS PRIVATE JP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(jpcore PUBLIC -Wall -Wextra -Wno-unused-parameter)
if(WIN32)
  # The system Save dialog (filedialogs.cpp).
  target_link_libraries(jpcore PRIVATE ole32 uuid shell32)
endif()
if(MINGW)
  # Large translation units exceed the default COFF section limit.
  target_compile_options(jpcore PUBLIC -Wa,-mbig-obj -D_USE_MATH_DEFINES)
endif()

qt_add_executable(jeffpub79 WIN32 MACOSX_BUNDLE src/main.cpp)
if(APPLE)
  # A macOS app: an icon made from the 256-pixel picture, document types for
  # .jpub and .pub, and the fonts and dictionaries in Contents/Resources.
  set(JP_ICONSET ${CMAKE_BINARY_DIR}/JeffPub79.iconset)
  set(JP_ICNS ${CMAKE_BINARY_DIR}/JeffPub79.icns)
  add_custom_command(OUTPUT ${JP_ICNS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JP_ICONSET}
    COMMAND sips -z 16 16 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_16x16.png
    COMMAND sips -z 32 32 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_16x16@2x.png
    COMMAND sips -z 32 32 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_32x32.png
    COMMAND sips -z 64 64 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_32x32@2x.png
    COMMAND sips -z 128 128 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_128x128.png
    COMMAND sips -z 256 256 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_128x128@2x.png
    COMMAND sips -z 256 256 ${CMAKE_SOURCE_DIR}/resources/app.png --out ${JP_ICONSET}/icon_256x256.png
    COMMAND iconutil -c icns ${JP_ICONSET} -o ${JP_ICNS}
    DEPENDS ${CMAKE_SOURCE_DIR}/resources/app.png
    VERBATIM)
  set_source_files_properties(${JP_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(jeffpub79 PRIVATE ${JP_ICNS})
  set_target_properties(jeffpub79 PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.jeffsteinport.jeffpub79
    MACOSX_BUNDLE_ICON_FILE JeffPub79.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  add_custom_command(TARGET jeffpub79 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/fonts $<TARGET_BUNDLE_CONTENT_DIR:jeffpub79>/Resources/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/dict $<TARGET_BUNDLE_CONTENT_DIR:jeffpub79>/Resources/dict
    VERBATIM)
endif()
if(WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/resources/app.ico)
  target_sources(jeffpub79 PRIVATE resources/app.rc)
endif()
target_link_libraries(jeffpub79 PRIVATE jpcore)
set_target_properties(jeffpub79 PROPERTIES OUTPUT_NAME "JeffPub79")

enable_testing()
qt_add_executable(jptests tests/tests.cpp)
target_link_libraries(jptests PRIVATE jpcore Qt6::Test)
target_compile_definitions(jptests PRIVATE JP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data")
# qt_add_executable makes Windows GUI programs by default; tests must keep their console output.
set_target_properties(jptests PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME jptests COMMAND jptests)
set_tests_properties(jptests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

install(TARGETS jeffpub79 RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# Developer tool for rendering and converting publications from the command line.
qt_add_executable(jpubtool tools/jpubtool.cpp)
target_link_libraries(jpubtool PRIVATE jpcore)
set_target_properties(jpubtool PROPERTIES WIN32_EXECUTABLE OFF)
