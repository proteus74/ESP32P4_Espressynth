idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "esp32p4")
    set(tusb_mcu "OPT_MCU_ESP32P4")
elseif(target STREQUAL "esp32s3")
    set(tusb_mcu "OPT_MCU_ESP32S3")
else()
    set(tusb_mcu "OPT_MCU_NONE")
endif()

set(srcs
    "src/tusb.c"
    "src/common/tusb_fifo.c"
    "src/host/usbh.c"
    "src/host/hub.c"
    "src/class/midi/midi_host.c"
    "src/class/hid/hid_host.c"
    "src/portable/synopsys/dwc2/hcd_dwc2.c"
    "src/portable/synopsys/dwc2/dwc2_common.c"
)

if(CONFIG_APP_ENABLE_USB_DEVICE)
    list(APPEND srcs
        "src/device/usbd.c"
        "src/device/usbd_control.c"
        "src/portable/synopsys/dwc2/dcd_dwc2.c"
    )
endif()

if(CONFIG_USB_DEV_ENABLE_CDC)
    list(APPEND srcs "src/class/cdc/cdc_device.c")
endif()

if(CONFIG_USB_DEV_ENABLE_MIDI)
    list(APPEND srcs "src/class/midi/midi_device.c")
endif()

if(CONFIG_USB_DEV_ENABLE_UAC2)
    list(APPEND srcs "src/class/audio/audio_device.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_mm
    REQUIRES freertos
)

target_compile_options(${COMPONENT_LIB} PUBLIC "-DCFG_TUSB_MCU=${tusb_mcu}")
