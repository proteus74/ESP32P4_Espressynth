# EEZ Studio UI (generated, never edited by hand): one export per display layout, picked by
# Kconfig "Display Output -> EEZ UI layout". Only the chosen folder is compiled and on the
# include path - every layout exports the same file and symbol names.
if(CONFIG_APP_UI_LAYOUT_800_480)
    set(EEZ_UI_DIR "../eezstudio/src/ui/800_480")
else()
    set(EEZ_UI_DIR "../eezstudio/src/ui/1024_800")
endif()

set(COMPONENT_SRCS
    "main.cpp"
    "app_core.cpp"
    "esp_hosted_dma.cpp"
    "wifi_remote_compat.c"
    "boards/board_init.cpp"
    "features/display/jd9365_init.c"
    "features/display/jd9165_panel_init.c"
    "features/display/touch/esp_lcd_touch.c"
    "features/display/touch/esp_lcd_gsl3680.c"
    "features/display/touch/gsl_point_id.c"
    "features/display/touch/esp_lcd_touch_gt911.c"
    "features/display/touch/esp_lcd_jd9365.c"
    "features/display/touch/esp_lcd_jd9165.c"
    "features/synth/PresetPaths.cpp"
    "features/synth/SamplePaths.cpp"
    "features/synth/MemoryDiagnostics.cpp"
    "features/io/i2c_device_registry.cpp"
    "features/display/eez_ui_bridge.cpp"
    "${EEZ_UI_DIR}/images.c"
    "${EEZ_UI_DIR}/screens.c"
    "${EEZ_UI_DIR}/styles.c"
    "${EEZ_UI_DIR}/ui.c"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "boards"
    "features"
    "features/display/touch"
    "cli"
    "audio/engine"
    "features/synth"
    "${EEZ_UI_DIR}"
)

# NOTE: REQUIRES is resolved before Kconfig, so it cannot depend on CONFIG_ options
# (unlike SRCS above) -- esp_driver_i2s, espressif__esp_codec_dev and jonlidgard__wm8960 are
# listed unconditionally for that reason. Managed components (idf_component.yml) are NOT
# auto-linked just by being listed there - without an explicit REQUIRES entry here, their
# library never ends up in the final link and every one of their functions comes back as an
# "undefined reference" at link time even though the component itself compiles fine.
idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_ADD_INCLUDEDIRS}
    REQUIRES espressif__esp_lcd_ili9341 esp_driver_spi tinyusb driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ppa esp_driver_jpeg esp_lcd esp_mm crow_rgb_lcd esp_timer freertos log esp_netif esp_eth fatfs sdmmc esp_wifi nvs_flash bt nimble_central_utils esp_partition esp_app_format bootloader_support console spi_flash esp_hw_support lwip esp_http_server dns_server joltwallet__littlefs espressif__esp_codec_dev jonlidgard__wm8960 waveshare__esp_lcd_jd9365_10_1 lvgl esp_lvgl_port espressif__cjson espressif__esp_audio_effects tiny_soundfont mda_piano msfa espressif__led_strip
    WHOLE_ARCHIVE
)

# The whole audio engine (main/audio/engine/*.h, header-only) is compiled through main.cpp.
# -fno-math-errno lets sqrtf/floorf/fabsf become single FPU instructions instead of libm
# calls (errno is never read here); -fno-trapping-math drops FP exception bookkeeping.
# Deliberately NOT -ffast-math: NaN-aware clamps (noderender::clampToRange) rely on IEEE
# comparison semantics.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-fno-math-errno;-fno-trapping-math")

# Keep dependency task and SDIO buffer policies in project-owned wrappers.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=xTaskCreatePinnedToCore"
    "-Wl,--wrap=xTaskCreateStaticPinnedToCore"
    "-Wl,--wrap=eh_host_port_dma_alloc_aligned"
)
