# PDFium, for placing PDF pages as vector graphics (src/render/pdfpage.cpp).
# The prebuilt library from bblanchon/pdfium-binaries, downloaded once into
# the build tree and checked against its published SHA-256. Its license
# texts (PDFium's and those of the libraries built into it) are in
# _deps/pdfium/licenses and ship with every package.
set(JP_PDFIUM_RELEASE "chromium/8086")
if(WIN32)
  set(_pdfium_asset pdfium-win-x64.tgz)
  set(_pdfium_sha256 1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d)
elseif(APPLE)
  set(_pdfium_asset pdfium-mac-univ.tgz)
  set(_pdfium_sha256 ceb8dff448db2a2b15ac52e1360427edc3461f2b51cb4579b1462f1a931087d5)
else()
  set(_pdfium_asset pdfium-linux-x64.tgz)
  set(_pdfium_sha256 588577cf52dabc1a444988bac841920df54cc2f141801424de97ab04f4fbb935)
endif()

set(JP_PDFIUM_DIR ${CMAKE_BINARY_DIR}/_deps/pdfium)
set(_pdfium_archive ${CMAKE_BINARY_DIR}/_deps/${_pdfium_asset})
if(NOT EXISTS ${JP_PDFIUM_DIR}/include/fpdfview.h)
  file(DOWNLOAD https://github.com/bblanchon/pdfium-binaries/releases/download/${JP_PDFIUM_RELEASE}/${_pdfium_asset}
       ${_pdfium_archive}
       EXPECTED_HASH SHA256=${_pdfium_sha256}
       TLS_VERIFY ON
       STATUS _pdfium_status)
  list(GET _pdfium_status 0 _pdfium_code)
  if(NOT _pdfium_code EQUAL 0)
    message(FATAL_ERROR "Downloading PDFium (${_pdfium_asset}) failed: ${_pdfium_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT ${_pdfium_archive} DESTINATION ${JP_PDFIUM_DIR})
  if(APPLE)
    # It names itself "./libpdfium.dylib"; programs are to find it by rpath
    # (and macdeployqt then moves it into the app). Changing the name voids
    # its signature, so it's signed again (ad hoc, as the app is).
    execute_process(COMMAND install_name_tool -id @rpath/libpdfium.dylib ${JP_PDFIUM_DIR}/lib/libpdfium.dylib COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND codesign --force --sign - ${JP_PDFIUM_DIR}/lib/libpdfium.dylib COMMAND_ERROR_IS_FATAL ANY)
  endif()
endif()

add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${JP_PDFIUM_DIR}/include)
if(WIN32)
  set(JP_PDFIUM_LIBRARY ${JP_PDFIUM_DIR}/bin/pdfium.dll)
  # MinGW links the DLL itself; its C interface needs no import library.
  set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION ${JP_PDFIUM_LIBRARY} IMPORTED_IMPLIB ${JP_PDFIUM_LIBRARY})
elseif(APPLE)
  set(JP_PDFIUM_LIBRARY ${JP_PDFIUM_DIR}/lib/libpdfium.dylib)
  set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION ${JP_PDFIUM_LIBRARY} IMPORTED_SONAME @rpath/libpdfium.dylib)
else()
  set(JP_PDFIUM_LIBRARY ${JP_PDFIUM_DIR}/lib/libpdfium.so)
  set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION ${JP_PDFIUM_LIBRARY} IMPORTED_SONAME libpdfium.so)
endif()
