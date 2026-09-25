#pragma once

#include "app_core.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <cstdio>
#include <cstring>

#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
    #include "features/wifi/wifi_manager.hpp"
#endif

// Static-file HTTP server, served from its own dedicated "www" LittleFS partition (see
// ../../../partitions.csv), separate from the general-purpose "littlefs" data partition
// (features/filesystem/littlefs_manager.hpp). That keeps the web assets image small and
// independently buildable/flashable: populate the project's data/ directory, then run
// `idf.py www-flash` (see the root CMakeLists.txt) to rebuild and flash just that
// partition -- no app rebuild or full reflash required to update served files.
//
// esp_http_server listens on INADDR_ANY by default, so once started it is reachable over
// whichever network interface(s) are actually up -- Wi-Fi, Ethernet, or both at once --
// with no per-interface wiring needed here.
namespace Feature::Web {

constexpr const char* MOUNT_POINT = "/www";
constexpr const char* PARTITION_LABEL = "www";

namespace {

constexpr const char* TAG = "FeatureWeb";
constexpr size_t FILE_READ_CHUNK_SIZE = 8192;

httpd_handle_t server = nullptr;

// Disables Nagle's algorithm on every accepted connection. Without this, a chunked-transfer
// response (this handler's own static_file_handler, or a synth API route's larger JSON
// replies) can sit waiting for the peer's delayed ACK of the previous chunk instead of going
// out immediately - worth tens of ms per chunk, the usual cause of a large asset (main.js-sized
// bundles) taking seconds to load over LAN.
esp_err_t socket_open_callback(httpd_handle_t /*hd*/, int sockfd) {
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

const char* content_type_for(const char* path) {
    const char* dot = std::strrchr(path, '.');
    if (dot == nullptr) return "application/octet-stream";
    if (std::strcmp(dot, ".html") == 0 || std::strcmp(dot, ".htm") == 0) return "text/html";
    if (std::strcmp(dot, ".css") == 0) return "text/css";
    if (std::strcmp(dot, ".js") == 0) return "application/javascript";
    if (std::strcmp(dot, ".json") == 0) return "application/json";
    if (std::strcmp(dot, ".png") == 0) return "image/png";
    if (std::strcmp(dot, ".jpg") == 0 || std::strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (std::strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (std::strcmp(dot, ".ico") == 0) return "image/x-icon";
    return "text/plain";
}

// Catch-all GET handler: maps the request URI onto a file under the "www" LittleFS
// mount and streams it back in chunks. "/" serves "/index.html".
esp_err_t static_file_handler(httpd_req_t* req) {
    char uri_path[CONFIG_HTTPD_MAX_URI_LEN + 1] = {};
    // snprintf, not strncpy: always null-terminates within sizeof(uri_path), avoiding
    // -Wstringop-truncation (GCC can't prove strncpy's result stays terminated here purely
    // from this call site, even though the array's zero-init already guarantees it).
    std::snprintf(uri_path, sizeof(uri_path), "%s", req->uri);
    char* query = std::strchr(uri_path, '?');
    if (query != nullptr) *query = '\0';
    if (std::strstr(uri_path, "..") != nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    char file_path[sizeof(uri_path) + 32];
    if (std::strcmp(uri_path, "/") == 0) {
        std::snprintf(file_path, sizeof(file_path), "%s/index.html", MOUNT_POINT);
    } else {
        std::snprintf(file_path, sizeof(file_path), "%s%s", MOUNT_POINT, uri_path);
    }

    FILE* file = std::fopen(file_path, "rb");
    if (file == nullptr) {
#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
        // While acting as a captive portal, send every unknown path to "/" instead of a
        // plain 404 -- that's what makes a phone's OS captive-portal detection (Apple's
        // captive.apple.com/hotspot-detect.html, Android's generate_204, Windows' ncsi.txt,
        // ...) land the user straight on this device's page after joining the fallback AP.
        // A body is required alongside the redirect: iOS won't recognize a captive portal
        // from a bare redirect with no content.
        if (Feature::WiFi::is_captive_portal_active()) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, "Redirecting to the captive portal", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
#endif
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // NOT PSRAM: littlefs's lfs_bd_read() bypasses its own cache for large, block-aligned
    // reads (lfs.c, "bypass cache?") and has the underlying esp_partition_read() write
    // straight into *this* buffer -- while the flash cache is disabled, same hazard as
    // the task stack above. A read of this size against a partition using 4096-byte
    // blocks (see the littlefs-python --block-size in the root CMakeLists.txt) reliably
    // hits that bypass, so this can't be PSRAM either.
    auto* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(FILE_READ_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        std::fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for(file_path));

    esp_err_t result = ESP_OK;
    size_t bytes_read;
    while ((bytes_read = std::fread(buffer, 1, FILE_READ_CHUNK_SIZE, file)) > 0) {
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buffer), bytes_read) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, nullptr, 0); // terminates the chunked response
    }

    heap_caps_free(buffer);
    std::fclose(file);
    return result;
}

} // namespace

/**
 * @brief The running httpd server handle, valid once begin() has returned ESP_OK. Lets other
 *        feature modules (e.g. features/synth/synth_web_api.hpp) register additional routes
 *        on the same server instead of standing up their own - see register_catch_all()'s
 *        comment for why that must happen between the two calls.
 */
inline httpd_handle_t handle() {
    return server;
}

/**
 * @brief Mount the "www" LittleFS partition and start the HTTP server on port 80. Does NOT
 *        register the catch-all static-file route yet -- call register_catch_all() only after
 *        every other feature module that wants to add routes (e.g. Feature::Synth's WS/REST
 *        API) has registered them on handle(). esp_http_server tries registered handlers in
 *        registration order and the catch-all's wildcard URI pattern matches everything, so
 *        any route added after it would never be reached. Safe to call even if the web assets
 *        image hasn't been flashed yet -- logs and returns an error instead of formatting the
 *        partition out from under a pending flash.
 */
inline esp_err_t begin() {
    esp_vfs_littlefs_conf_t fs_conf = {};
    fs_conf.base_path = MOUNT_POINT;
    fs_conf.partition_label = PARTITION_LABEL;
    fs_conf.format_if_mount_failed = false;
    fs_conf.dont_mount = false;

    const esp_err_t mount_err = esp_vfs_littlefs_register(&fs_conf);
    if (mount_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount '%s' partition: %s (has the web assets image been "
                      "built and flashed? see main/features/web/web_server.hpp)",
                 PARTITION_LABEL, esp_err_to_name(mount_err));
        return mount_err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = App::MAIN_CORE;
    // NOT MALLOC_CAP_SPIRAM: URI handlers here call into LittleFS (fopen/fread), which
    // does raw esp_partition/esp_flash reads. Those briefly disable the flash cache, and
    // ESP-IDF asserts that the calling task's *stack* is entirely in internal RAM at that
    // point (esp_task_stack_is_sane_cache_disabled()) -- a PSRAM stack panics the moment
    // a request actually touches the filesystem. See static_file_handler()'s read buffer
    // for the same hazard hitting a plain heap allocation, not just a task stack.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    // Headroom for the synth API's WS connection (stays open for the whole session) plus
    // overlapping preset/sample-list/file-explorer requests and static asset fetches -
    // default (7, 3 reserved internally) exhausts under that load and stalls httpd's TCP
    // send long enough to trip the task watchdog. Needs CONFIG_LWIP_MAX_SOCKETS raised to
    // match (see sdkconfig.defaults) - httpd's own socket count is capped by that ceiling.
    // Harmless overhead when the synth feature is disabled - just a few more socket slots.
    config.max_open_sockets = 12;
    config.max_uri_handlers = 28; // ws, preset get/post/list, wavetable, samples/soundfonts, files
                                  // list/upload/delete/rename, midi devices, version, memory
                                  // diagnostics, static "/*"
    config.stack_size = 10240; // default (4096) is too tight for the file-explorer handlers'
                                // path-building locals plus the upload handler's streaming buffer
    config.open_fn = &socket_open_callback;

    const esp_err_t start_err = httpd_start(&server, &config);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(start_err));
        return start_err;
    }

    ESP_LOGI(TAG, "Web server listening on port %u, serving %s (partition '%s')",
             config.server_port, MOUNT_POINT, PARTITION_LABEL);
    return ESP_OK;
}

/**
 * @brief Registers the catch-all wildcard static-file route. Call this LAST, after begin() and
 *        after every other feature module has registered its own routes on handle() - see
 *        begin()'s comment for why the order matters.
 */
inline void register_catch_all() {
    const httpd_uri_t catch_all = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &catch_all);
}

} // namespace Feature::Web
