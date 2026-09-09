set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Older bundled vcpkg can corrupt non-ASCII TEMP/TMP when capturing vcvars.
# Set paths inside the CMake child after that capture, preserving UTF-8 paths.
get_filename_component(minisql_build_temp "${CMAKE_CURRENT_LIST_DIR}/../../.tools/tmp" ABSOLUTE)
file(MAKE_DIRECTORY "${minisql_build_temp}")
set(ENV{TEMP} "${minisql_build_temp}")
set(ENV{TMP} "${minisql_build_temp}")

# Use the project-local official pkgconf if present; old bundled vcpkg refers
# to an MSYS2 package that is no longer available on its mirrors.
get_filename_component(minisql_pkgconf "${CMAKE_CURRENT_LIST_DIR}/../../.tools/pkgconf/PFiles64/pkgconf-3.0.7/pkgconf.exe" ABSOLUTE)
if(EXISTS "${minisql_pkgconf}" AND NOT DEFINED ENV{PKG_CONFIG})
    set(ENV{PKG_CONFIG} "${minisql_pkgconf}")
endif()
