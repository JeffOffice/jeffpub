cmake_minimum_required(VERSION 3.21)
project(JeffPub VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Network Test)

add_subdirectory(third_party)
include(cmake/pdfium.cmake)

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
target_link_libraries(jpcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport Qt6::Network PRIVATE jp_libmspub jp_hunspell jp_zlib pdfium)
target_compile_definitions(jpcore PUBLIC JP_VERSION="${PROJECT_VERSION}" QT_NO_KEYWORDS PRIVATE JP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(jpcore PUBLIC -Wall -Wextra -Wno-unused-parameter)
if(WIN32)
  # The system Save dialog (filedialogs.cpp).
  target_link_libraries(jpcore PRIVATE ole32 uuid shell32)
endif()
if(WIN32)
  # Programs find pdfium.dll beside them.
  file(COPY ${JP_PDFIUM_LIBRARY} DESTINATION ${CMAKE_BINARY_DIR})
endif()
if(MINGW)
  # Large translation units exceed the default COFF section limit.
  target_compile_options(jpcore PUBLIC -Wa,-mbig-obj -D_USE_MATH_DEFINES)
endif()

qt_add_executable(jeffpub WIN32 MACOSX_BUNDLE src/main.cpp)
if(APPLE)
  # A macOS app: an icon made from the 256-pixel picture, document types for
  # .jpub and .pub, and the fonts and dictionaries in Contents/Resources.
  set(JP_ICONSET ${CMAKE_BINARY_DIR}/JeffPub.iconset)
  set(JP_ICNS ${CMAKE_BINARY_DIR}/JeffPub.icns)
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
  target_sources(jeffpub PRIVATE ${JP_ICNS})
  set_target_properties(jeffpub PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.jeffoffice.jeffpub
    MACOSX_BUNDLE_ICON_FILE JeffPub.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  add_custom_command(TARGET jeffpub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/fonts $<TARGET_BUNDLE_CONTENT_DIR:jeffpub>/Resources/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/dict $<TARGET_BUNDLE_CONTENT_DIR:jeffpub>/Resources/dict
    VERBATIM)
endif()
if(WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/resources/app.ico)
  target_sources(jeffpub PRIVATE resources/app.rc)
endif()
target_link_libraries(jeffpub PRIVATE jpcore)
set_target_properties(jeffpub PROPERTIES OUTPUT_NAME "JeffPub")

# Translations of JeffPub's own text. `cmake --build build --target
# jeffpub_lupdate` gathers every string marked for translation (tr(),
# QCoreApplication::translate()) into translations/jeffpub.ts, the list a
# translator starts from; each translations/jeffpub_<language>.ts is built
# into the program under :/i18n/. Without Qt's Linguist tools the program
# builds the same, in English.
# English needs one too, for plurals only: jeffpub_en.ts gives "%n page(s)"
# its singular and plural forms (lupdate adds new ones; fill in their forms).
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
  file(GLOB JP_TRANSLATIONS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/translations/jeffpub_*.ts)
  list(REMOVE_ITEM JP_TRANSLATIONS ${CMAKE_SOURCE_DIR}/translations/jeffpub_en.ts)
  qt_add_lupdate(TS_FILES ${CMAKE_SOURCE_DIR}/translations/jeffpub.ts ${JP_TRANSLATIONS}
                 PLURALS_TS_FILE ${CMAKE_SOURCE_DIR}/translations/jeffpub_en.ts
                 SOURCE_TARGETS jpcore jeffpub
                 LUPDATE_TARGET jeffpub_lupdate
                 OPTIONS -no-obsolete -locations relative)
  qt_add_lrelease(TS_FILES ${JP_TRANSLATIONS} ${CMAKE_SOURCE_DIR}/translations/jeffpub_en.ts QM_FILES_OUTPUT_VARIABLE JP_QM_FILES)
  qt_add_resources(jpcore "translations" PREFIX "/i18n" BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${JP_QM_FILES})
endif()

enable_testing()
qt_add_executable(jptests tests/tests.cpp)
target_link_libraries(jptests PRIVATE jpcore jp_libmspub Qt6::Test)
# libmspub's shape table, for checking its defaults against Publisher's.
target_include_directories(jptests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libmspub/src ${CMAKE_SOURCE_DIR}/third_party/compat)
target_compile_definitions(jptests PRIVATE JP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data")
# qt_add_executable makes Windows GUI programs by default; tests must keep their console output.
set_target_properties(jptests PROPERTIES WIN32_EXECUTABLE OFF)
add_test(NAME jptests COMMAND jptests)
set_tests_properties(jptests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

install(TARGETS jeffpub RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# Developer tool for rendering and converting publications from the command line.
qt_add_executable(jpubtool tools/jpubtool.cpp)
target_link_libraries(jpubtool PRIVATE jpcore)
set_target_properties(jpubtool PROPERTIES WIN32_EXECUTABLE OFF)
