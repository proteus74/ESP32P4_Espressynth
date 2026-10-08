set(COMPONENT_SRCS
    "main.cpp"
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
    "../eezstudio/src/ui/images.c"
    "../eezstudio/src/ui/screens.c"
    "../eezstudio/src/ui/styles.c"
    "../eezstudio/src/ui/ui.c"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "boards"
    "features"
    "features/display/touch"
    "cli"
    "audio/engine"
    "features/synth"
    "../eezstudio/src/ui"
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
    REQUIRES espressif__esp_lcd_ili9341 esp_driver_spi tinyusb driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ppa esp_lcd esp_timer freertos log esp_netif esp_eth fatfs sdmmc esp_wifi nvs_flash bt nimble_central_utils esp_partition esp_app_format bootloader_support console spi_flash esp_hw_support lwip esp_http_server dns_server joltwallet__littlefs espressif__esp_codec_dev jonlidgard__wm8960 waveshare__esp_lcd_jd9365_10_1 lvgl esp_lvgl_port espressif__cjson espressif__esp_audio_effects tiny_soundfont mda_piano espressif__led_strip
    WHOLE_ARCHIVE
)
