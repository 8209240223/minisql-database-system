set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# 强制使用 7-zip（而非 cmake -E tar）解压源码；vcpkg 内置 cmake tar 在 Windows 上
# 多次 vcpkg_from_github 同端口重复解压时会栈溢出崩溃（boost-cmake 已在 boost/boost 之后
# 第二次解压 34KB cmake tar 时复现，0xC0000409）。7za 已由 vcpkg 预置在下载缓存目录。
set(VCPKG_USE_7ZIP 1)

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
