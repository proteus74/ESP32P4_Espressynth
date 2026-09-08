#pragma once

// Ethernet status command. Only registered when Ethernet is enabled
// (CONFIG_APP_ENABLE_ETHERNET); backed by main/features/ethernet/ethernet_manager.hpp.

#include "esp_console.h"
#include "esp_netif.h"
#include "features/ethernet/ethernet_manager.hpp"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_eth_status(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!Feature::Ethernet::is_connected()) {
        printf("Link down.\n");
        return 0;
    }
    printf("Link up.\n");

    esp_netif_ip_info_t ip_info{};
    if (Feature::Ethernet::get_ip_info(ip_info) == ESP_OK) {
        printf("IP:      " IPSTR "\n", IP2STR(&ip_info.ip));
        printf("Netmask: " IPSTR "\n", IP2STR(&ip_info.netmask));
        printf("Gateway: " IPSTR "\n", IP2STR(&ip_info.gw));
    }
    return 0;
}

} // namespace

inline void register_eth() {
    const esp_console_cmd_t cmd = {
        .command = "eth-status",
        .help = "Show Ethernet link status and IP configuration",
        .hint = nullptr,
        .func = &cli_cmd_eth_status,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
