# ESP-IDF queues WS close callbacks with a reusable session pointer. Coalesce
# closes and require a pending close flag, so a stale callback cannot close a
# newly opened connection. Compile a patched build copy; leave the SDK intact.
idf_component_get_property(httpd_dir esp_http_server COMPONENT_DIR)
idf_component_get_property(httpd_lib esp_http_server COMPONENT_LIB)
set(httpd_session_source "${httpd_dir}/src/httpd_sess.c")
file(READ "${httpd_session_source}" httpd_session_code)

set(httpd_close_old [=[    return httpd_queue_work(handle, httpd_sess_close, session);]=])
set(httpd_close_new [=[    if (session->lru_socket) {
        return ESP_OK; // A close is already queued for this session.
    }
    session->lru_socket = true;
    esp_err_t ret = httpd_queue_work(handle, httpd_sess_close, session);
    if (ret != ESP_OK) {
        session->lru_socket = false;
    }
    return ret;]=])
foreach(expected IN ITEMS
        "if (!sock_db->lru_counter && !sock_db->lru_socket)"
        "    context.session->lru_socket = true;"
        "${httpd_close_old}")
    string(FIND "${httpd_session_code}" "${expected}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "ESP-IDF httpd_sess.c changed; review the session close patch")
    endif()
endforeach()
string(REPLACE "if (!sock_db->lru_counter && !sock_db->lru_socket)"
               "if (!sock_db->lru_socket)" httpd_session_code "${httpd_session_code}")
string(REPLACE "    context.session->lru_socket = true;\n" ""
               httpd_session_code "${httpd_session_code}")
string(REPLACE "${httpd_close_old}" "${httpd_close_new}"
               httpd_session_code "${httpd_session_code}")
set(httpd_session_patched "${CMAKE_BINARY_DIR}/httpd_sess.c")
file(CONFIGURE OUTPUT "${httpd_session_patched}" CONTENT "${httpd_session_code}" @ONLY)
get_target_property(httpd_sources ${httpd_lib} SOURCES)
list(REMOVE_ITEM httpd_sources "${httpd_session_source}")
list(APPEND httpd_sources "${httpd_session_patched}")
set_property(TARGET ${httpd_lib} PROPERTY SOURCES "${httpd_sources}")
target_include_directories(${httpd_lib} PRIVATE "${httpd_dir}/src")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${httpd_session_source}")
