# turns assets/clipper.png into a byte array header so the binary carries its own window icon.
# (gcc 13 has no #embed yet)
set(CLIPPER_ICON_PNG ${CMAKE_SOURCE_DIR}/assets/clipper.png)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CLIPPER_ICON_PNG})

file(READ ${CLIPPER_ICON_PNG} CLIPPER_ICON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CLIPPER_ICON_BYTES "${CLIPPER_ICON_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/AppIcon.h
    "#pragma once\n// generated from assets/clipper.png, don't edit\n"
    "inline constexpr unsigned char kAppIconPng[] = {${CLIPPER_ICON_BYTES}};\n")
