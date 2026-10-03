# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/bluetooth/nimble/common/nimble_central_utils")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUSB_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/main/tusb_config.h\"" APPEND)

# LVGL's builtin allocator needs a 4 MB pool for lodepng's compressed-file and inflate scratch
# allocations. Put that pool in PSRAM through LVGL's documented allocator hook. Keep the macro
# object-like: CMake silently drops function-style definitions such as
# `LV_MEM_POOL_ALLOC(size)=...`, leaving the pool as a large internal-RAM array.
idf_build_set_property(
    COMPILE_DEFINITIONS
    "LV_MEM_POOL_INCLUDE=\"${CMAKE_CURRENT_LIST_DIR}/main/lvgl_pool_alloc/lv_mem_pool_alloc.h\""
    APPEND)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "LV_MEM_POOL_ALLOC=espressynth_lv_mem_pool_alloc"
    APPEND)

project(ESP32P4_Espressynth)

# jonlidgard/wm8960 v0.0.3 has a real bug: wm8960_t.register_local_copy is sized with
# sizeof(WM8960_REGISTER_DEFAULTS) (a byte count, 112) used directly as an array length
# instead of an element count, making it twice as large as intended. wm8960_reset()'s
# memcpy(..., sizeof(register_local_copy)) then reads 224 bytes from the 112-byte source
# array. Harmless in practice (only register indices 0-55 are ever read back), but this
# project builds with -Werror, so silence just this one warning for just this one
# third-party component instead of disabling it project-wide. At -Og GCC only flags this
# via -Wstringop-overread; at -O2 (CONFIG_COMPILER_OPTIMIZATION_PERF) its more aggressive
# constant-propagation additionally surfaces the identical out-of-bounds access as
# -Warray-bounds, so both need silencing here.
idf_build_get_property(build_components BUILD_COMPONENTS)
# Put RGB565 blending and arc/rounded-edge masking hot paths in IRAM. Keep
# other formats in flash to preserve internal RAM for audio. Use a stable
# section name because LVGL annotates declarations as well as definitions.
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    idf_component_get_property(lvgl_dir lvgl__lvgl COMPONENT_DIR)
    set_source_files_properties(
        "${lvgl_dir}/src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c"
        "${lvgl_dir}/src/draw/sw/lv_draw_sw_mask.c"
        TARGET_DIRECTORY ${lvgl_lib}
        PROPERTIES COMPILE_DEFINITIONS "LV_ATTRIBUTE_FAST_MEM=__attribute__((section(\".iram1\")))")
endif()
if("jonlidgard__wm8960" IN_LIST build_components)
    idf_component_get_property(wm8960_lib jonlidgard__wm8960 COMPONENT_LIB)
    target_compile_options(${wm8960_lib} PRIVATE -Wno-error=stringop-overread -Wno-error=array-bounds)
endif()

# Web server static assets ("www" partition, see partitions.csv and
# main/features/web/web_server.hpp): built from data/ into build/www.bin by the
# "littlefs_www_bin" target (`idf.py littlefs_www_bin` builds just the image).
# Deliberately NOT FLASH_IN_PROJECT -- these assets are meant to be rebuilt/reflashed
# independently of the app via `idf.py www-flash`, not on every `idf.py flash`.
littlefs_create_partition_image(www "${CMAKE_CURRENT_LIST_DIR}/data")

# Default/seed content for the "littlefs" partition (see partitions.csv and
# main/features/filesystem/littlefs_manager.hpp) - e.g. the boot splash image
# (main/features/display/display_lvgl.hpp reads /littlefs/system/splash.png). Same mechanism
# as "www" above: built from littlefs_data/ into build/littlefs.bin by the
# "littlefs_littlefs_bin" target, reflashed independently via `idf.py littlefs-flash`, not on
# every `idf.py flash` (deliberately not FLASH_IN_PROJECT).
#
# WARNING, unlike "www": this is the SAME partition the running firmware writes user data to
# at runtime (saved presets/samples/soundfonts - see synth_manager.hpp's
# ensure_storage_directories(), synth_web_api.hpp's upload/save routes). `idf.py
# littlefs-flash` REPLACES THE ENTIRE PARTITION with only what's currently in littlefs_data/ -
# anything saved on the device that isn't also checked into littlefs_data/ is PERMANENTLY
# LOST the moment this runs. Safe for provisioning a fresh/blank device; back up anything
# worth keeping (File Explorer) before running it again on a device that already has real
# saved content.
littlefs_create_partition_image(littlefs "${CMAKE_CURRENT_LIST_DIR}/littlefs_data")
