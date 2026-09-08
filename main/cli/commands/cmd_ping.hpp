#pragma once

// "ping" command: sends ICMP echo requests to a host (name or IP). Only registered when a
// network interface is enabled (CONFIG_APP_ENABLE_WIFI or CONFIG_APP_ENABLE_ETHERNET). Runs
// asynchronously (like the ESP-IDF ping example) -- replies print as they arrive, so this
// command returns immediately rather than blocking the console for the whole session.

#include "esp_console.h"
#include "esp_err.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

constexpr uint32_t PING_COUNT = 4;

void on_ping_success(esp_ping_handle_t hdl, void*) {
    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed_time_ms = 0, recv_len = 0;
    ip_addr_t target_addr{};
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time_ms, sizeof(elapsed_time_ms));
    printf("%u bytes from %s: icmp_seq=%u ttl=%u time=%u ms\n", static_cast<unsigned>(recv_len),
           ipaddr_ntoa(&target_addr), seqno, ttl, static_cast<unsigned>(elapsed_time_ms));
}

void on_ping_timeout(esp_ping_handle_t hdl, void*) {
    uint16_t seqno = 0;
    ip_addr_t target_addr{};
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    printf("From %s icmp_seq=%u timeout\n", ipaddr_ntoa(&target_addr), seqno);
}

void on_ping_end(esp_ping_handle_t hdl, void*) {
    ip_addr_t target_addr{};
    uint32_t transmitted = 0, received = 0, total_time_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    const uint32_t loss = transmitted > 0
                               ? static_cast<uint32_t>((1.0f - static_cast<float>(received) / transmitted) * 100)
                               : 0;
    printf("\n--- %s ping statistics ---\n", ipaddr_ntoa(&target_addr));
    printf("%u packets transmitted, %u received, %u%% packet loss, time %ums\n",
           static_cast<unsigned>(transmitted), static_cast<unsigned>(received), static_cast<unsigned>(loss),
           static_cast<unsigned>(total_time_ms));
    esp_ping_delete_session(hdl);
}

inline int cli_cmd_ping(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: ping <host>\n");
        return 1;
    }

    ip_addr_t target_addr{};
    struct addrinfo hint {};
    struct addrinfo* res = nullptr;
    if (getaddrinfo(argv[1], nullptr, &hint, &res) != 0 || res == nullptr) {
        printf("ping: unknown host %s\n", argv[1]);
        return 1;
    }
    if (res->ai_family == AF_INET) {
        struct in_addr addr4 = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    }
    freeaddrinfo(res);

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = PING_COUNT;
    config.target_addr = target_addr;

    const esp_ping_callbacks_t callbacks = {
        .cb_args = nullptr,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
    };

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &ping) != ESP_OK) {
        printf("ping: failed to create session\n");
        return 1;
    }
    if (esp_ping_start(ping) != ESP_OK) {
        printf("ping: failed to start\n");
        esp_ping_delete_session(ping);
        return 1;
    }
    return 0;
}

} // namespace

inline void register_ping() {
    const esp_console_cmd_t cmd = {
        .command = "ping",
        .help = "Send 4 ICMP echo requests to a host (name or IP)",
        .hint = "<host>",
        .func = &cli_cmd_ping,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
