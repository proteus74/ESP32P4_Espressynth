#pragma once
#include "sdkconfig.h"

#if defined(CONFIG_BOARD_JC8012P4A1)
    #include "jc8012p4a1.hpp"
    namespace CurrentBoard = Board::JC8012P4A1;
#elif defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    #include "jc1060p470c_7inch.hpp"
    namespace CurrentBoard = Board::JC1060P470C7Inch;
#elif defined(CONFIG_WAVESHARE_P4_WIFI_DEV_KIT_BOARD)
    #include "waveshare_p4_wifi_dev_kit.hpp"
    namespace CurrentBoard = Board::WaveshareP4WiFiDevKit;
#elif defined(CONFIG_BOARD_CUSTOM_P4)
    #include "custom_p4.hpp"
    namespace CurrentBoard = Board::Custom;
#else
    #error "No valid board target selected in Kconfig! Run idf.py menuconfig to choose a board."
#endif
