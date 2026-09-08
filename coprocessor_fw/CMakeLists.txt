# SPDX-License-Identifier: Apache-2.0
#
# ESP32-C6 companion firmware: turns the C6 into the ESP-Hosted co-processor
# that main/features/wifi/wifi_manager.hpp and main/features/ble/ble_manager.hpp talk to over SDIO.
# Separate ESP-IDF project (own target: esp32c6) -- build and flash it
# independently, or push it from the P4 host via Feature::Hosted::OTA
# (see main/features/hosted/hosted_ota.hpp). See coprocessor_fw/README.md.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32p4_template_coprocessor)
