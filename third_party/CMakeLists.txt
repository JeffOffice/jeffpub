# Third-party code used to import .pub files.
#   librevenge 0.0.5 (MPL-2.0 / LGPL-2.1+) — document interfaces and OLE2 streams
#   libmspub (MPL-2.0) — the Publisher file parser
# Both are patched to build without Boost and ICU; see third_party/README.md.

include(FetchContent)

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(zlib)
  add_library(jp_zlib INTERFACE)
  target_link_libraries(jp_zlib INTERFACE zlibstatic)
  target_include_directories(jp_zlib INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
else()
  add_library(jp_zlib INTERFACE)
  target_link_libraries(jp_zlib INTERFACE ZLIB::ZLIB)
endif()

set(RVNG ${CMAKE_CURRENT_SOURCE_DIR}/librevenge)
add_library(jp_librevenge STATIC
  ${RVNG}/src/lib/RVNGBinaryData.cpp
  ${RVNG}/src/lib/RVNGMemoryStream.cpp
  ${RVNG}/src/lib/RVNGOLEStream.cpp
  ${RVNG}/src/lib/RVNGProperty.cpp
  ${RVNG}/src/lib/RVNGPropertyList.cpp
  ${RVNG}/src/lib/RVNGPropertyListVector.cpp
  ${RVNG}/src/lib/RVNGString.cpp
  ${RVNG}/src/lib/RVNGStringVector.cpp
  ${RVNG}/src/lib/RVNGStreamImplementation.cpp
  ${RVNG}/src/lib/RVNGZipStream.cpp)
target_include_directories(jp_librevenge PUBLIC ${RVNG}/inc PRIVATE ${RVNG}/src/lib ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(jp_librevenge PRIVATE jp_zlib)
set_target_properties(jp_librevenge PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)

file(GLOB MSPUB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libmspub/src/*.cpp)
add_library(jp_libmspub STATIC ${MSPUB_SOURCES})
target_include_directories(jp_libmspub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libmspub/inc
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libmspub/src ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(jp_libmspub PUBLIC jp_librevenge PRIVATE jp_zlib)
set_target_properties(jp_libmspub PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(jp_librevenge PRIVATE /W0 /utf-8 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS)
  target_compile_options(jp_libmspub PRIVATE /W0 /utf-8 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(jp_librevenge PRIVATE -w)
  target_compile_options(jp_libmspub PRIVATE -w)
  if(MINGW)
    target_compile_options(jp_librevenge PRIVATE -Wa,-mbig-obj -D_USE_MATH_DEFINES)
    target_compile_options(jp_libmspub PRIVATE -Wa,-mbig-obj -D_USE_MATH_DEFINES)
  endif()
endif()

# Hunspell (MPL 1.1 / GPL 2 / LGPL 2.1 tri-license) for spell checking.
FetchContent_Declare(hunspell
  URL https://github.com/hunspell/hunspell/archive/refs/tags/v1.7.2.tar.gz
  URL_HASH SHA256=69fa312d3586c988789266eaf7ffc9861d9f6396c31fc930a014d551b59bbd6e)
FetchContent_GetProperties(hunspell)
if(NOT hunspell_POPULATED)
  FetchContent_Populate(hunspell)
endif()
file(GLOB HUNSPELL_SOURCES ${hunspell_SOURCE_DIR}/src/hunspell/*.cxx)
add_library(jp_hunspell STATIC ${HUNSPELL_SOURCES})
target_include_directories(jp_hunspell PUBLIC ${hunspell_SOURCE_DIR}/src)
target_compile_definitions(jp_hunspell PUBLIC HUNSPELL_STATIC)
set_target_properties(jp_hunspell PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(jp_hunspell PRIVATE /W0 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(jp_hunspell PRIVATE -w)
endif()
