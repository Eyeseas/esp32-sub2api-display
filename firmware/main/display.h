#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_MAX_ACCOUNTS 3
#define DISPLAY_MAX_WINDOWS 4
#define DISPLAY_MAX_MODELS 4
#define DISPLAY_RESPONSE_LIMIT 32768

typedef struct {
    char label[40];
    char quality[24];
    char kind[24];
    float used_percent;
    time_t reset_at;
    time_t sampled_at;
    time_t stale_at;
    bool has_stats;       // Usage totals since this window started, from local sub2api logs.
    char stats_requests[24];
    char stats_tokens[24];
    char stats_cost[24];
} quota_window_t;

typedef struct {
    char name[48];        // Empty on the row that folds the remaining models.
    int other_models;
    char requests[24];
    char tokens[24];
    char cost[24];
} model_usage_t;

enum { SEVERITY_OK, SEVERITY_UNKNOWN, SEVERITY_WARN, SEVERITY_CRIT };

typedef struct {
    char id[24];
    char alias[64];
    char platform[24];
    char type[24];
    char flag[24];        // Most severe flag label; empty when the account has none.
    int flag_severity;    // Severity of `flag` itself, including quota reminders.
    int block_severity;   // Most severe flag other than quota reminders.
    int concurrency;
    int concurrency_limit;
    int window_count;
    int windows_total;
    quota_window_t windows[DISPLAY_MAX_WINDOWS];
    char balance[48];
    char today_requests[24];
    char today_tokens[24];
    char today_cost[24];
} display_account_t;

typedef struct {
    bool valid;
    bool partial;
    bool demo;
    int accounts_total;
    int observed_normal;
    int alert_accounts;
    int page;
    bool has_more;
    char day[16];
    char timezone[40];
    bool has_utc_offset;  // Offset of the sub2api day's timezone, derived from day.start_at; drives the clock.
    int utc_offset;
    char requests[32];
    char tokens[32];
    char account_cost[32];
    char user_cost[32];
    char standard_cost[32];
    bool models_complete; // Today's models across the whole device scope, most tokens first.
    int model_count;
    model_usage_t models[DISPLAY_MAX_MODELS];
    time_t generated_at;
    int account_count;
    display_account_t accounts[DISPLAY_MAX_ACCOUNTS];
} display_model_t;

typedef struct {
    char text[192];       // One hitokoto sentence, at most 40 characters as requested.
    char source[128];     // "author「work」" or "「work」".
    char type[16];        // Category name, e.g. 诗词.
} quote_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char bridge_url[256];
    char device_token[192];
    bool allow_http;
} device_config_t;

esp_err_t board_init(lv_display_t **display);
esp_err_t board_set_backlight(bool on);
esp_err_t board_set_brightness(int percent);
bool board_key_pressed(void);
void board_touch_raw(bool raw);
lv_point_t board_touch_last_raw(int *count, lv_point_t *second);
esp_err_t board_calibrate_touch(const int raw[][2], const int screen[][2], int count, float *worst);
esp_err_t board_fill_color(uint32_t color);
esp_err_t board_draw_ramp(void);
esp_err_t board_lcd_command(uint8_t command, const uint8_t *data, size_t length);
bool device_config_load(device_config_t *config);
void device_config_start_console(void);
esp_err_t model_parse(const char *body, display_model_t *model);
void model_demo(display_model_t *model);
esp_err_t quote_parse(const char *body, quote_t *quote);
void ui_init(lv_display_t *display);
void ui_set_model(const display_model_t *model);
void ui_set_status(const char *status);
void ui_set_quote(const quote_t *quote);
bool ui_text_supported(const char *text);
void network_start(const device_config_t *config);
void network_request_page(int page);
bool network_request_quote(void);
void ui_select_view(int view);
esp_err_t ui_display_test(const char *pattern);
esp_err_t ui_set_brightness(int percent);
esp_err_t ui_lcd_command(uint8_t command, const uint8_t *data, size_t length);
