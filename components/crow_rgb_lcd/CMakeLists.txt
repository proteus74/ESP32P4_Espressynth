# Patched copy of the ESP-IDF RGB LCD driver (see crow_rgb_lcd.c header comment).
idf_component_register(SRCS "crow_rgb_lcd.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "${IDF_PATH}/components/esp_lcd/priv_include"
                       REQUIRES esp_lcd
                       PRIV_REQUIRES esp_mm esp_psram esp_pm esp_driver_i2s esp_driver_dma esp_driver_gpio esp_hal_lcd esp_hal_i2s)
