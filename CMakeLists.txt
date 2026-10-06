cmake_minimum_required(VERSION 3.21)
project(JeffPub79 VERSION 0.1.10 LANGUAGES CXX)

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
target_link_libraries(jpcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport Qt6::Network PRIVATE jp_libmspub jp_hunspell)
target_compile_definitions(jpcore PUBLIC JP_VERSION="${PROJECT_VERSION}" QT_NO_KEYWORDS PRIVATE JP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(jpcore PUBLIC /W3 /utf-8 /permissive-)
else()
  target_compile_options(jpcore PUBLIC -Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
  # The system Save dialog (filedialogs.cpp).
  target_link_libraries(jpcore PRIVATE ole32 uuid shell32)
endif()
if(MINGW)
  # Large translation units exceed the default COFF section limit.
  target_compile_options(jpcore PUBLIC -Wa,-mbig-obj -D_USE_MATH_DEFINES)
endif()

qt_add_executable(jeffpub79 WIN32 src/main.cpp)
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

install(TARGETS jeffpub79 RUNTIME DESTINATION bin)

# Developer tool for rendering and converting publications from the command line.
qt_add_executable(jpubtool tools/jpubtool.cpp)
target_link_libraries(jpubtool PRIVATE jpcore)
set_target_properties(jpubtool PROPERTIES WIN32_EXECUTABLE OFF)
