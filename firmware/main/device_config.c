#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static bool copy_field(cJSON *root, const char *name, char *out, size_t size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= size) return false;
    strcpy(out, value->valuestring);
    return true;
}

static bool decode_config(cJSON *root, device_config_t *config)
{
    memset(config, 0, sizeof(*config));
    if (!copy_field(root, "wifi_ssid", config->wifi_ssid, sizeof(config->wifi_ssid)) || !config->wifi_ssid[0] ||
        !copy_field(root, "wifi_password", config->wifi_password, sizeof(config->wifi_password)) ||
        !copy_field(root, "bridge_url", config->bridge_url, sizeof(config->bridge_url)) ||
        !copy_field(root, "device_token", config->device_token, sizeof(config->device_token)) || !config->device_token[0]) return false;
    config->allow_http = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "allow_http"));
    if (strncmp(config->bridge_url, "https://", 8) && !(config->allow_http && !strncmp(config->bridge_url, "http://", 7))) return false;
    if (strchr(config->device_token, '\n') || strchr(config->device_token, '\r') ||
        strchr(config->bridge_url, '\n') || strchr(config->bridge_url, '\r') || strchr(config->bridge_url, '?')) return false;
    size_t length = strlen(config->bridge_url);
    while (length && config->bridge_url[length - 1] == '/') config->bridge_url[--length] = 0;
    return true;
}

bool device_config_load(device_config_t *config)
{
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READONLY, &nvs) != ESP_OK) return false;
    // The default main-task stack is smaller than this JSON buffer.
    char *buffer = calloc(1, 4096);
    if (!buffer) { nvs_close(nvs); return false; }
    size_t length = 4096;
    esp_err_t result = nvs_get_str(nvs, "config", buffer, &length);
    nvs_close(nvs);
    if (result != ESP_OK) { free(buffer); return false; }
    cJSON *root = cJSON_Parse(buffer);
    bool valid = root && decode_config(root, config);
    cJSON_Delete(root);
    memset(buffer, 0, 4096);
    free(buffer);
    return valid;
}

static void console_task(void *unused)
{
    char *buffer = calloc(1, 4096);
    if (!buffer) { vTaskDelete(NULL); return; }
    size_t used = 0;
    bool overflow = false;
    setvbuf(stdin, NULL, _IONBF, 0);
    for (;;) {
        int value = getchar();
        if (value == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (value != '\n' && value != '\r') {
            if (used < 4095 && !overflow) buffer[used++] = (char)value;
            else overflow = true;
            continue;
        }
        if (!used && !overflow) continue;
        buffer[used] = 0;
        cJSON *root = overflow ? NULL : cJSON_Parse(buffer);
        cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
        device_config_t config;
        if (cJSON_IsString(command) && !strcmp(command->valuestring, "configure") && decode_config(root, &config)) {
            nvs_handle_t nvs;
            esp_err_t result = nvs_open("display", NVS_READWRITE, &nvs);
            if (result == ESP_OK) {
                result = nvs_set_str(nvs, "config", buffer);
                if (result == ESP_OK) result = nvs_commit(nvs);
                nvs_close(nvs);
            }
            printf(result == ESP_OK ? "CONFIG_OK\n" : "CONFIG_SAVE_FAILED\n");
            cJSON_Delete(root);
            memset(buffer, 0, 4096);
            memset(&config, 0, sizeof(config));
            if (result == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(250)); esp_restart(); }
        } else if (cJSON_IsString(command) && !strcmp(command->valuestring, "display_test")) {
            cJSON *pattern = cJSON_GetObjectItemCaseSensitive(root, "pattern");
            if (cJSON_IsString(pattern)) {
                cJSON *step = cJSON_GetObjectItemCaseSensitive(root, "step"), *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
                cJSON *level = cJSON_GetObjectItemCaseSensitive(root, "level");
                esp_err_t result = ui_display_test(pattern->valuestring, cJSON_IsNumber(step) ? step->valueint : -1,
                                                   cJSON_IsNumber(steps) ? steps->valueint : 0, cJSON_IsNumber(level) ? level->valueint : 0x80);
                if (result == ESP_OK) printf("DISPLAY_TEST_OK %s\n", pattern->valuestring);
                else printf("DISPLAY_TEST_FAILED %s\n", esp_err_to_name(result));
            } else printf("DISPLAY_TEST_INVALID\n");
            cJSON_Delete(root);
        } else if (cJSON_IsString(command) && !strcmp(command->valuestring, "lcd")) {
            cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "cmd"), *data = cJSON_GetObjectItemCaseSensitive(root, "data"), *item;
            uint8_t bytes[14];
            size_t length = 0;
            bool valid = cJSON_IsNumber(code) && code->valueint >= 0 && code->valueint <= 255 && (!data || cJSON_IsArray(data));
            cJSON_ArrayForEach(item, data) {
                if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255 || length >= sizeof(bytes)) { valid = false; break; }
                bytes[length++] = (uint8_t)item->valueint;
            }
            esp_err_t result = valid ? ui_lcd_command((uint8_t)code->valueint, bytes, length) : ESP_ERR_INVALID_ARG;
            if (result == ESP_OK) printf("LCD_OK 0x%02x\n", code->valueint);
            else printf("LCD_FAILED %s\n", esp_err_to_name(result));
            cJSON_Delete(root);
        } else if (cJSON_IsString(command) && !strcmp(command->valuestring, "backlight")) {
            cJSON *level = cJSON_GetObjectItemCaseSensitive(root, "level");
            esp_err_t result = cJSON_IsNumber(level) ? ui_set_brightness(level->valueint) : ESP_ERR_INVALID_ARG;
            if (result == ESP_OK) printf("BACKLIGHT_OK %d\n", level->valueint);
            else printf("BACKLIGHT_FAILED %s\n", esp_err_to_name(result));
            cJSON_Delete(root);
        } else if (cJSON_IsString(command) && !strcmp(command->valuestring, "view")) {
            cJSON *view = cJSON_GetObjectItemCaseSensitive(root, "view");
            if (cJSON_IsNumber(view) && view->valueint >= 0 && view->valueint <= 5) {
                ui_select_view(view->valueint);
                printf("VIEW_OK\n");
            }
            cJSON_Delete(root);
        } else if (cJSON_IsString(command) && !strcmp(command->valuestring, "status")) {
            printf(device_config_load(&config) ? "CONFIGURED\n" : "NOT_CONFIGURED\n");
            memset(&config, 0, sizeof(config));
            cJSON_Delete(root);
        } else { printf("CONFIG_INVALID\n"); cJSON_Delete(root); }
        used = 0; overflow = false; memset(buffer, 0, 4096);
    }
}

void device_config_start_console(void)
{
    xTaskCreate(console_task, "config_console", 8192, NULL, 3, NULL);
}
