# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUSB_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/main/tusb_config.h\"" APPEND)
project(ESP32P4_Espressynth)

# jonlidgard/wm8960 v0.0.3 has a real bug: wm8960_t.register_local_copy is sized with
# sizeof(WM8960_REGISTER_DEFAULTS) (a byte count, 112) used directly as an array length
# instead of an element count, making it twice as large as intended. wm8960_reset()'s
# memcpy(..., sizeof(register_local_copy)) then reads 224 bytes from the 112-byte source
# array. Harmless in practice (only register indices 0-55 are ever read back), but this
# project builds with -Werror, so silence just this one warning for just this one
# third-party component instead of disabling it project-wide.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("jonlidgard__wm8960" IN_LIST build_components)
    idf_component_get_property(wm8960_lib jonlidgard__wm8960 COMPONENT_LIB)
    target_compile_options(${wm8960_lib} PRIVATE -Wno-error=stringop-overread)
endif()

# Web server static assets ("www" partition, see partitions.csv and
# main/features/web/web_server.hpp): built from data/ into build/www.bin by the
# "littlefs_www_bin" target (`idf.py littlefs_www_bin` builds just the image).
# Deliberately NOT FLASH_IN_PROJECT -- these assets are meant to be rebuilt/reflashed
# independently of the app via `idf.py www-flash`, not on every `idf.py flash`.
littlefs_create_partition_image(www "${CMAKE_CURRENT_LIST_DIR}/data")
