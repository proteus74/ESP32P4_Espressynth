#pragma once

#include "boards/board_config.hpp"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

namespace Feature::Ethernet {

namespace {
inline esp_netif_t* s_eth_netif = nullptr;
inline bool s_link_up = false;

inline void eth_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    constexpr const char* TAG = "FeatureEthernet";
    auto* eth_handle = *static_cast<esp_eth_handle_t*>(event_data);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        s_link_up = true;
        uint8_t mac_addr[6] = {};
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet connected, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        ESP_LOGI(TAG, "Ethernet disconnected");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

inline void got_ip_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
    constexpr const char* TAG = "FeatureEthernet";
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const esp_netif_ip_info_t& ip_info = event->ip_info;

    ESP_LOGI(TAG, "Ethernet DHCP address assigned: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Ethernet netmask: " IPSTR ", gateway: " IPSTR,
             IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
}
} // namespace

/**
 * @brief True once the link is up (cable connected, autonegotiation complete).
 */
inline bool is_connected() {
    return s_link_up;
}

/**
 * @brief Get the interface's current IP configuration. Returns ESP_ERR_INVALID_STATE if
 *        Ethernet hasn't been initialized yet.
 */
inline esp_err_t get_ip_info(esp_netif_ip_info_t& ip_info) {
    if (s_eth_netif == nullptr) return ESP_ERR_INVALID_STATE;
    return esp_netif_get_ip_info(s_eth_netif, &ip_info);
}

/**
 * @brief Initialize 100M Ethernet (EMAC + PHY) subsystem
 */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureEthernet";

    if constexpr (!CurrentBoard::HAS_ETHERNET) {
        ESP_LOGE(TAG, "Selected board %s does not support on-board Ethernet!", CurrentBoard::BOARD_NAME);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CurrentBoard::PIN_ETH_MDC;
    emac_config.smi_gpio.mdio_num = CurrentBoard::PIN_ETH_MDIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    // Spelled out rather than left to ETH_ESP32_EMAC_DEFAULT_CONFIG(): these are the RMII
    // pins both P4 boards use, and having them here makes a wiring mismatch visible in the
    // boot log instead of hidden in an IDF header. The 50 MHz RMII reference clock is
    // driven by the PHY into GPIO50 (EMAC_CLK_EXT_IN) on both boards.
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 50;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = 49;
    emac_config.emac_dataif_gpio.rmii.txd0_num  = 34;
    emac_config.emac_dataif_gpio.rmii.txd1_num  = 35;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = 28;
    emac_config.emac_dataif_gpio.rmii.rxd0_num  = 29;
    emac_config.emac_dataif_gpio.rmii.rxd1_num  = 30;

    ESP_LOGI(TAG, "EMAC RMII: CLK_IN=GPIO50 TX_EN=GPIO49 TXD0=GPIO34 TXD1=GPIO35 "
                  "CRS_DV=GPIO28 RXD0=GPIO29 RXD1=GPIO30");
    ESP_LOGI(TAG, "PHY: MDC=GPIO%d MDIO=GPIO%d RST=GPIO%d, expected address %d",
             CurrentBoard::PIN_ETH_MDC, CurrentBoard::PIN_ETH_MDIO,
             CurrentBoard::PIN_ETH_PHY_RST, CurrentBoard::ETH_PHY_ADDR);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    // Scan the MDIO bus instead of trusting the board's nominal address: on the
    // JC1060P470C the PHY's AD3 strap hangs off GPIO0, so the latched address can differ
    // from the schematic value depending on that pin's state at PHY reset.
    phy_config.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_config.reset_gpio_num = CurrentBoard::PIN_ETH_PHY_RST;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == nullptr) {
        ESP_LOGE(TAG, "EMAC creation failed");
        return ESP_FAIL;
    }

    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == nullptr) {
        ESP_LOGE(TAG, "PHY creation failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        // Almost always means the PHY did not answer on MDIO: no PHY fitted, PHY held in
        // reset, missing 3V3/50 MHz clock, or wrong MDC/MDIO pins.
        ESP_LOGE(TAG, "Ethernet driver install failed: %s - no PHY found on MDIO?",
                 esp_err_to_name(err));
        phy->del(phy);
        mac->del(mac);
        return err;
    }

    uint32_t detected_phy_addr = 0;
    if (esp_eth_ioctl(eth_handle, ETH_CMD_G_PHY_ADDR, &detected_phy_addr) == ESP_OK) {
        ESP_LOGI(TAG, "PHY answered at MDIO address %u", static_cast<unsigned>(detected_phy_addr));
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* eth_netif = esp_netif_new(&netif_config);
    if (eth_netif == nullptr) {
        ESP_LOGE(TAG, "Ethernet netif creation failed");
        return ESP_FAIL;
    }
    s_eth_netif = eth_netif;

    esp_eth_netif_glue_handle_t netif_glue = esp_eth_new_netif_glue(eth_handle);
    err = esp_netif_attach(eth_netif, netif_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet netif attach failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &got_ip_event_handler, nullptr));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Ethernet started - waiting for link. No \"Ethernet connected\" line means "
                  "the link never came up (cable, magnetics/RJ45, or RMII clock).");
    return ESP_OK;
}

} // namespace Feature::Ethernet
