# Bundle a Longshot mod: the mod IS its folder (mod.toml + sources), there is
# nothing to compile - the game compiles the TypeScript as it loads. This
# target only stages the folder where the Lumen manager expects a built
# bundle: <build>/mods/<bundle>, the convention run_cmake_build() reads.
#
# Note: the build dir lives inside this source dir (the manager configures
# <source_dir>/build), so copying is strictly file-by-file - a wholesale
# copy_directory would feed the build tree back into the bundle.
cmake_minimum_required(VERSION 3.20)
project(lumen_photo_longshot NONE)

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/mods/lumen-photo")

file(GLOB_RECURSE LUMEN_MOD_SOURCES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/mod.toml"
     "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/test/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# A generated script does the staging at build time, with absolute paths
# baked in at configure: `cmake --build` after an edit re-syncs the listed
# sources without reconfiguring, and nothing outside them ever moves.
set(sync_script "${CMAKE_BINARY_DIR}/lumen_bundle_sync.cmake")
set(sync_body "")
foreach(entry IN LISTS LUMEN_MOD_SOURCES)
    string(APPEND sync_body
        "get_filename_component(d \"${BUNDLE_DIR}/${entry}\" DIRECTORY)\n"
        "file(MAKE_DIRECTORY \"\${d}\")\n"
        "file(COPY \"${CMAKE_CURRENT_SOURCE_DIR}/${entry}\" DESTINATION \"\${d}\")\n")
endforeach()
file(WRITE "${sync_script}" "${sync_body}")

add_custom_target(lumen_photo_bundle ALL
    COMMAND "${CMAKE_COMMAND}" -P "${sync_script}")
