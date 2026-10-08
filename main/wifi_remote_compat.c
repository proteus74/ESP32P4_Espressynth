#include "sdkconfig.h"

#if CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_SOC_WIFI_SUPPORTED
// IDF 6.1's remote adapter has an empty wifi_osi_funcs_t and a weak, zero-sized
// g_wifi_osi_funcs. Binutils drops that section with non-contiguous RAM linking.
// Remote WiFi only passes its address; replace the unused placeholder with one
// byte so references remain resolvable. Native WiFi retains its real OSI table.
unsigned char g_wifi_osi_funcs;
#endif
