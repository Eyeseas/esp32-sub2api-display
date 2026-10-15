#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static device_config_t config;
static EventGroupHandle_t wifi_events;
static TaskHandle_t network_task_handle;
static portMUX_TYPE page_lock = portMUX_INITIALIZER_UNLOCKED;
static int requested_page = 1;
static volatile bool quote_requested;

// Categories d/h/i/k (literature, film, poetry, philosophy) avoid the kana common in anime quotes. 40 characters
// fill at most three lines of the desk. See https://developer.hitokoto.cn/sentence/
#define QUOTE_URL "https://v1.hitokoto.cn/?c=d&c=h&c=i&c=k&max_length=40&encode=json&charset=utf-8"
#define QUOTE_INTERVAL 600

typedef struct { char *body; size_t length; bool overflow; char etag[96]; } response_t;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(wifi_events, BIT0);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifi_events, BIT0);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "ETag"))
        snprintf(response->etag, sizeof(response->etag), "%s", event->header_value);
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (event->data_len < 0 || response->length + event->data_len > DISPLAY_RESPONSE_LIMIT) {
            response->overflow = true; return ESP_ERR_INVALID_SIZE;
        }
        memcpy(response->body + response->length, event->data, event->data_len);
        response->length += event->data_len;
        response->body[response->length] = 0;
    }
    return ESP_OK;
}

// Sentences with a character outside the GB2312 desk font are skipped. v1.hitokoto.cn allows 2 requests a second.
static bool fetch_quote(char *body, quote_t *quote)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(600));
        response_t response = {.body = body}; body[0] = 0;
        esp_http_client_config_t http_cfg = {.url = QUOTE_URL, .timeout_ms = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = http_event, .user_data = &response,
            .buffer_size = 2048, .buffer_size_tx = 1024};
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) return false;
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_err_t result = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (result != ESP_OK || response.overflow || status != 200) {
            ESP_LOGW("network", "quote failed: HTTP=%d transport=%s", status, esp_err_to_name(result));
            return false;
        }
        if (quote_parse(body, quote) == ESP_OK && ui_text_supported(quote->text) && ui_text_supported(quote->source)) {
            ESP_LOGI("network", "quote accepted: attempt=%d bytes=%u", attempt + 1, (unsigned)strlen(quote->text));
            return true;
        }
        ESP_LOGI("network", "quote skipped: unparsable or outside the font");
    }
    return false;
}

static void network_task(void *unused)
{
    char *body = heap_caps_malloc(DISPLAY_RESPONSE_LIMIT + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = malloc(DISPLAY_RESPONSE_LIMIT + 1);
    display_model_t *incoming = calloc(1, sizeof(*incoming));
    quote_t *quote = calloc(1, sizeof(*quote));
    if (!body || !incoming || !quote) { ui_set_status("内存不足"); free(body); free(incoming); free(quote); vTaskDelete(NULL); return; }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    char etag[96] = "";
    int last_page = 0, delay = 15;
    time_t quote_due = 0;
    for (;;) {
        if (!(xEventGroupWaitBits(wifi_events, BIT0, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000)) & BIT0)) {
            ui_set_status("Wi-Fi 断线"); esp_wifi_connect(); continue;
        }
        if (time(NULL) < 1700000000) {
            ui_set_status("校时中");
            esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000));
            if (time(NULL) < 1700000000) continue;
        }
        portENTER_CRITICAL(&page_lock); int page = requested_page; portEXIT_CRITICAL(&page_lock);
        if (page != last_page) { etag[0] = 0; last_page = page; }
        char url[384], authorization[208];
        snprintf(url, sizeof(url), "%s/display/v1/snapshot?page=%d&page_size=3", config.bridge_url, page);
        snprintf(authorization, sizeof(authorization), "Bearer %s", config.device_token);
        response_t response = {.body = body}; body[0] = 0;
        esp_http_client_config_t http_cfg = {.url = url, .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = http_event, .user_data = &response,
            .disable_auto_redirect = true, .buffer_size = 2048, .buffer_size_tx = 1024};
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) { ui_set_status("连接失败"); vTaskDelay(pdMS_TO_TICKS(15000)); continue; }
        esp_http_client_set_header(client, "Authorization", authorization);
        esp_http_client_set_header(client, "Accept", "application/json");
        if (etag[0]) esp_http_client_set_header(client, "If-None-Match", etag);
        esp_err_t result = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        memset(authorization, 0, sizeof(authorization));
        if (result == ESP_OK && !response.overflow && status == 200 && model_parse(body, incoming) == ESP_OK && incoming->page == page) {
            ui_set_model(incoming); ui_set_status(incoming->partial ? "部分数据过期" : "在线");
            snprintf(etag, sizeof(etag), "%s", response.etag); delay = 15;
            ESP_LOGI("network", "snapshot accepted: page=%d accounts=%d partial=%d", page, incoming->account_count, incoming->partial);
        } else if (result == ESP_OK && status == 304) { delay = 15; }
        else {
            ui_set_status(status == 401 || status == 403 ? "授权失效" : response.overflow ? "响应过大" : "读取失败");
            etag[0] = 0; delay = status == 401 || status == 403 ? 300 : (delay < 150 ? delay * 2 : 300);
            ESP_LOGW("network", "snapshot failed: HTTP=%d transport=%s", status, esp_err_to_name(result));
        }
        memset(body, 0, response.length);
        if (quote_requested || time(NULL) >= quote_due) {
            quote_requested = false;
            int64_t started = esp_timer_get_time();
            bool fetched = fetch_quote(body, quote);
            ESP_LOGI("network", "quote fetch took %lld ms", (long long)((esp_timer_get_time() - started) / 1000));
            ui_set_quote(fetched ? quote : NULL);
            quote_due = time(NULL) + (fetched ? QUOTE_INTERVAL : 60);
        }
        long wait = (long)(quote_due - time(NULL));
        if (wait > delay) wait = delay;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait > 1 ? wait : 1) * 1000));
    }
}

void network_request_page(int page)
{
    if (page < 1 || page > 10000) return;
    portENTER_CRITICAL(&page_lock); requested_page = page; portEXIT_CRITICAL(&page_lock);
    if (network_task_handle) xTaskNotifyGive(network_task_handle);
}

// A tap on the desk sentence; the task fetches a new one when it next wakes. False when not networking.
bool network_request_quote(void)
{
    if (!network_task_handle) return false;
    quote_requested = true;
    xTaskNotifyGive(network_task_handle);
    return true;
}

void network_start(const device_config_t *incoming)
{
    config = *incoming;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, config.wifi_ssid, strlen(config.wifi_ssid));
    memcpy(wifi.sta.password, config.wifi_password, strlen(config.wifi_password));
    wifi.sta.threshold.authmode = config.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    memset(&wifi, 0, sizeof(wifi));
    ESP_ERROR_CHECK(esp_wifi_start()); ui_set_status("连接 Wi-Fi");
    xTaskCreate(network_task, "display_network", 12288, NULL, 4, &network_task_handle);
}
