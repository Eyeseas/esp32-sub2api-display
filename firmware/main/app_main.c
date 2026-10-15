#include "display.h"
#include "config_flags.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

void app_main(void)
{
    setenv("TZ", "UTC0", 1); tzset();
    esp_err_t result = nvs_flash_init();
    // Do not erase NVS automatically; preserve configuration and any existing namespaces.
    ESP_ERROR_CHECK(result);
    lv_display_t *display;
    ESP_ERROR_CHECK(board_init(&display)); ui_init(display);
    device_config_t config;
    if (device_config_load(&config)) {
        network_start(&config);
    } else if (CONFIG_DISPLAY_DEMO) {
        display_model_t *model = calloc(1, sizeof(*model));
        if (model) { model_demo(model); ui_set_model(model); free(model); }
        ui_set_status("演示 / 待配置");
    }
    device_config_start_console();
    ESP_LOGI("display", "Ready; free internal=%u PSRAM=%u; USB accepts JSON commands",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
