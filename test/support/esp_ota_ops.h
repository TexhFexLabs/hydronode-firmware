#pragma once
#include <cstdint>
using esp_err_t = int;
using esp_ota_handle_t = unsigned;
constexpr int ESP_OK = 0;
struct esp_partition_t { uint32_t size = 0x1C0000; };
enum esp_ota_img_states_t { ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID };
inline esp_partition_t fakePartition;
inline esp_ota_img_states_t fakeImageState = ESP_OTA_IMG_VALID;
inline bool rolledBack = false;
inline unsigned downloadedImages = 0;
inline const esp_partition_t* esp_ota_get_next_update_partition(void*) { return &fakePartition; }
inline const esp_partition_t* esp_ota_get_running_partition() { return &fakePartition; }
inline int esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* out) { *out = fakeImageState; return 0; }
inline int esp_ota_begin(const esp_partition_t*, unsigned, esp_ota_handle_t* h) { *h = 1; downloadedImages++; return 0; }
inline int esp_ota_write(esp_ota_handle_t, const void*, size_t) { return 0; }
inline int esp_ota_end(esp_ota_handle_t) { return 0; }
inline int esp_ota_abort(esp_ota_handle_t) { return 0; }
inline int esp_ota_set_boot_partition(const esp_partition_t*) { return 0; }
inline int esp_ota_mark_app_valid_cancel_rollback() { fakeImageState = ESP_OTA_IMG_VALID; return 0; }
inline int esp_ota_mark_app_invalid_rollback_and_reboot() { rolledBack = true; throw Restarted{}; }
