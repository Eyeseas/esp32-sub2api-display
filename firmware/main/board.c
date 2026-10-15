#include "display.h"
#include "config_flags.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "board";
static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t panel;
static int backlight_percent = 100;
static bool backlight_lit;

static esp_err_t backlight_apply(void)
{
    uint32_t duty = backlight_lit ? (uint32_t)backlight_percent * 1023 / 100 : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t pca9557_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), 1000);
}

// Touch calibration. This panel's FT6336 is not linear: measured on 2026-10-06, a tap at (30, 30) read about
// (50, 70) while the centre was nearly exact, and readings left of x = 40 were hard to produce. Each screen
// coordinate is a quadratic polynomial of the controller's point, fitted from on-screen targets and kept in NVS.
static esp_lcd_touch_handle_t touch_handle;
static float touch_cal[2][6];
static bool touch_cal_valid, touch_raw;
static lv_point_t touch_last_raw, touch_last_second;
static int touch_last_count;

static void touch_terms(float x, float y, double terms[6])
{
    double u = x / 320.0, v = y / 240.0;
    terms[0] = 1; terms[1] = u; terms[2] = v; terms[3] = u * v; terms[4] = u * u; terms[5] = v * v;
}

static void touch_apply(const float cal[2][6], float x, float y, float *out_x, float *out_y)
{
    double terms[6], sx = 0, sy = 0;
    touch_terms(x, y, terms);
    for (int i = 0; i < 6; i++) { sx += cal[0][i] * terms[i]; sy += cal[1][i] * terms[i]; }
    *out_x = (float)sx; *out_y = (float)sy;
}

// Replaces esp_lvgl_port's reader so the point count and a second point stay visible to diagnostics. LVGL keeps
// the last pressed point for the release, so only pressed samples are corrected.
static void touch_read_calibrated(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t points[2] = {0};
    uint8_t count = 0;
    if (esp_lcd_touch_read_data(touch_handle) != ESP_OK ||
        esp_lcd_touch_get_data(touch_handle, points, &count, 2) != ESP_OK || !count) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = points[0].x; data->point.y = points[0].y;
    touch_last_raw = data->point;
    touch_last_count = count;
    touch_last_second = (lv_point_t){points[1].x, points[1].y};
    if (!touch_cal_valid || touch_raw) return;
    float x, y;
    touch_apply(touch_cal, data->point.x, data->point.y, &x, &y);
    data->point.x = x < 0 ? 0 : x > 319 ? 319 : lroundf(x);
    data->point.y = y < 0 ? 0 : y > 239 ? 239 : lroundf(y);
}

// Call from the LVGL task. Raw points are what calibration targets collect.
void board_touch_raw(bool raw) { touch_raw = raw; }

// The controller's point behind the latest pressed sample, before calibration, with how many points it reported
// and the second one.
lv_point_t board_touch_last_raw(int *count, lv_point_t *second)
{
    if (count) *count = touch_last_count;
    if (second) *second = touch_last_second;
    return touch_last_raw;
}

// Least squares over the samples, solved by Gaussian elimination on the normal equations.
static bool fit_axis(const int raw[][2], const int screen[][2], int count, int axis, float out[6])
{
    double m[6][7] = {{0}}, terms[6];
    for (int k = 0; k < count; k++) {
        touch_terms(raw[k][0], raw[k][1], terms);
        for (int i = 0; i < 6; i++) {
            for (int j = 0; j < 6; j++) m[i][j] += terms[i] * terms[j];
            m[i][6] += terms[i] * screen[k][axis];
        }
    }
    for (int col = 0; col < 6; col++) {
        int pivot = col;
        for (int row = col + 1; row < 6; row++) if (fabs(m[row][col]) > fabs(m[pivot][col])) pivot = row;
        if (fabs(m[pivot][col]) < 1e-9) return false;
        for (int j = 0; j < 7; j++) { double t = m[col][j]; m[col][j] = m[pivot][j]; m[pivot][j] = t; }
        for (int row = 0; row < 6; row++) {
            if (row == col) continue;
            double factor = m[row][col] / m[col][col];
            for (int j = col; j < 7; j++) m[row][j] -= factor * m[col][j];
        }
    }
    for (int i = 0; i < 6; i++) out[i] = (float)(m[i][6] / m[i][i]);
    return true;
}

// Fits, checks and saves a calibration from raw controller points and the targets they were aimed at.
// Rejects fits whose worst target is off by more than 15 px.
esp_err_t board_calibrate_touch(const int raw[][2], const int screen[][2], int count, float *worst)
{
    float cal[2][6];
    if (count < 6 || !fit_axis(raw, screen, count, 0, cal[0]) || !fit_axis(raw, screen, count, 1, cal[1])) return ESP_ERR_INVALID_ARG;
    *worst = 0;
    for (int k = 0; k < count; k++) {
        float x, y;
        touch_apply(cal, raw[k][0], raw[k][1], &x, &y);
        float error = hypotf(x - screen[k][0], y - screen[k][1]);
        if (error > *worst) *worst = error;
    }
    if (*worst > 15) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("display", NVS_READWRITE, &nvs), TAG, "touch nvs");
    esp_err_t result = nvs_set_blob(nvs, "touch_cal", cal, sizeof(cal));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    if (result != ESP_OK) return result;
    memcpy(touch_cal, cal, sizeof(cal));
    touch_cal_valid = true;
    for (int axis = 0; axis < 2; axis++)
        ESP_LOGI(TAG, "touch calibration %c: %.1f %.1f %.1f %.1f %.1f %.1f", axis ? 'y' : 'x', cal[axis][0], cal[axis][1],
                 cal[axis][2], cal[axis][3], cal[axis][4], cal[axis][5]);
    return ESP_OK;
}

esp_err_t board_init(lv_display_t **display)
{
    // LCKFB's GPIO42 backlight is active-low, so the 5 kHz PWM output is inverted (as in the LCKFB example).
    // Duty 0 keeps it off until the first frame has been sent.
    ledc_timer_config_t backlight_timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "backlight timer");
    ledc_channel_config_t backlight = {.gpio_num = 42, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1, .duty = 0, .flags.output_invert = true};
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight), TAG, "backlight channel");
    // The user key (SW2, silkscreen BOOT) pulls GPIO0 low. Holding it through reset still enters download mode.
    gpio_config_t key_cfg = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&key_cfg), TAG, "user key");
    nvs_handle_t nvs;
    uint8_t saved;
    if (nvs_open("display", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, "backlight", &saved) == ESP_OK && saved >= 1 && saved <= 100) backlight_percent = saved;
        size_t size = sizeof(touch_cal);
        touch_cal_valid = nvs_get_blob(nvs, "touch_cal", touch_cal, &size) == ESP_OK && size == sizeof(touch_cal);
        nvs_close(nvs);
    }
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1, .sda_io_num = 1, .scl_io_num = 2,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus");
    i2c_master_dev_handle_t expander;
    i2c_device_config_t exp_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x19, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &exp_cfg, &expander), TAG, "PCA9557");
    // Keep LCD CS high while configuring SPI; PA_EN remains high.
    ESP_RETURN_ON_ERROR(pca9557_write(expander, 1, 0x03), TAG, "PCA9557 output");
    ESP_RETURN_ON_ERROR(pca9557_write(expander, 3, 0xF8), TAG, "PCA9557 direction");
    ESP_LOGI(TAG, "PCA9557 0x19 ready; LCD CS is controlled by IO0");
    spi_bus_config_t spi_cfg = {.sclk_io_num = 41, .mosi_io_num = 40, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 320 * 40 * 2 + 8};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &spi_cfg, SPI_DMA_CH_AUTO), TAG, "SPI");
    esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = 39, .cs_gpio_num = -1,
        .pclk_hz = CONFIG_DISPLAY_LCD_PCLK_MHZ * 1000000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 2, .trans_queue_depth = 10};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &lcd_io), TAG, "LCD IO");
    esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_cfg, &panel), TAG, "ST7789");
    // Preserve the initialization used by the board project and the tested build.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(pca9557_write(expander, 1, 0x02), TAG, "LCD CS low");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, CONFIG_DISPLAY_LCD_INVERT), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, CONFIG_DISPLAY_LCD_X_GAP, CONFIG_DISPLAY_LCD_Y_GAP), TAG, "panel offset");
    // The ST7789 reset defaults wash out black, crush the gray ramp, couple rows and leave retained text on
    // this IPS panel. These voltage and gamma values were verified on the board with the `ramp` test pattern.
    static const struct { uint8_t command, length, data[14]; } tuning[] = {
        {0xc2, 1, {0x01}},             // VDV/VRH from registers
        {0xc3, 1, {0x12}},             // VRH: gamma reference voltage
        {0xc4, 1, {0x20}},             // VDV
        {0xbb, 1, {0x19}},             // VCOM 0.725 V
        {0xe0, 14, {0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b, 0x3f, 0x54, 0x4c, 0x18, 0x0d, 0x0b, 0x1f, 0x23}},
        {0xe1, 14, {0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c, 0x3f, 0x44, 0x51, 0x2f, 0x1f, 0x1f, 0x20, 0x23}},
    };
    for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++)
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, tuning[i].command, tuning[i].data, tuning[i].length), TAG, "panel tuning");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
    // Touch EINT is not wired, so LVGL's input timer polls it; that timer, not task_max_sleep_ms, paces the task.
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port");
    lvgl_port_display_cfg_t display_cfg = {.io_handle = lcd_io, .panel_handle = panel,
        .buffer_size = 320 * 40, .double_buffer = true, .hres = 320, .vres = 240,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = CONFIG_DISPLAY_LCD_SWAP_XY,
                     .mirror_x = CONFIG_DISPLAY_LCD_MIRROR_X,
                     .mirror_y = CONFIG_DISPLAY_LCD_MIRROR_Y},
        .flags = {.buff_dma = true, .swap_bytes = true}};
    *display = lvgl_port_add_disp(&display_cfg);
    ESP_RETURN_ON_FALSE(*display, ESP_ERR_NO_MEM, TAG, "display allocation");
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &touch_io_cfg, &touch_io), TAG, "touch IO");
    esp_lcd_touch_config_t touch_cfg = {.x_max = 240, .y_max = 320, .rst_gpio_num = -1, .int_gpio_num = -1,
        .flags = {.swap_xy = CONFIG_DISPLAY_TOUCH_SWAP_XY,
                  .mirror_x = CONFIG_DISPLAY_TOUCH_MIRROR_X,
                  .mirror_y = CONFIG_DISPLAY_TOUCH_MIRROR_Y}};
    // The FT5x06 driver overwrites 0x80-0x89 (thresholds, report periods) with values for larger FT5x06 panels.
    // Keep the FT6336's own values instead: read them first and write them back after the driver's init.
    uint8_t factory[10] = {0}, chip[2] = {0};
    esp_err_t factory_read = esp_lcd_panel_io_rx_param(touch_io, 0x80, factory, sizeof(factory));
    esp_lcd_panel_io_rx_param(touch_io, 0xa3, &chip[0], 1);
    esp_lcd_panel_io_rx_param(touch_io, 0xa6, &chip[1], 1);
    ESP_LOGI(TAG, "touch chip 0x%02x firmware 0x%02x; 0x80-0x89: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (%s)",
             chip[0], chip[1], factory[0], factory[1], factory[2], factory[3], factory[4], factory[5], factory[6], factory[7],
             factory[8], factory[9], esp_err_to_name(factory_read));
    esp_lcd_touch_handle_t touch;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch_cfg, &touch), TAG, "FT6336/FT5x06");
    if (factory_read == ESP_OK)
        for (int i = 0; i < (int)sizeof(factory); i++) esp_lcd_panel_io_tx_param(touch_io, 0x80 + i, &factory[i], 1);
    lvgl_port_touch_cfg_t touch_port = {.disp = *display, .handle = touch};
    lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_port);
    ESP_RETURN_ON_FALSE(touch_indev, ESP_ERR_NO_MEM, TAG, "touch registration");
    touch_handle = touch;
    lv_indev_set_read_cb(touch_indev, touch_read_calibrated);
    backlight_lit = true;
    ESP_RETURN_ON_ERROR(backlight_apply(), TAG, "backlight on");
    ESP_LOGI(TAG, "ST7789 320x240 SPI3 mode=2 %d MHz / FT5x06 0x38 initialized; backlight PWM %d%%; touch %s",
        CONFIG_DISPLAY_LCD_PCLK_MHZ, backlight_percent, touch_cal_valid ? "calibrated" : "uncalibrated");
    return ESP_OK;
}

// Call while holding the LVGL port lock so drawing cannot race these transfers.
esp_err_t board_set_backlight(bool on)
{
    if (on) {
        // A commandless parameter transfer waits for queued SPI color DMA to finish.
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, -1, NULL, 0), TAG, "wait for frame");
    }
    backlight_lit = on;
    return backlight_apply();
}

bool board_key_pressed(void) { return gpio_get_level(GPIO_NUM_0) == 0; }

// Call while holding the LVGL port lock. The level is saved so it survives the reset caused by opening USB serial.
esp_err_t board_set_brightness(int percent)
{
    if (percent < 1 || percent > 100) return ESP_ERR_INVALID_ARG;
    backlight_percent = percent;
    ESP_RETURN_ON_ERROR(backlight_apply(), TAG, "backlight level");
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("display", NVS_READWRITE, &nvs), TAG, "backlight nvs");
    esp_err_t result = nvs_set_u8(nvs, "backlight", (uint8_t)percent);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

esp_err_t board_fill_color(uint32_t color)
{
    const size_t pixels = 320 * 40;
    uint8_t *buffer = heap_caps_malloc(pixels * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "test buffer");
    uint16_t rgb565 = ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f);
    for (size_t i = 0; i < pixels; i++) {
        buffer[2 * i] = rgb565 >> 8;
        buffer[2 * i + 1] = rgb565 & 0xff;
    }
    esp_err_t result = ESP_OK;
    for (int y = 0; y < 240; y += 40) {
        result = esp_lcd_panel_draw_bitmap(panel, 0, y, 320, y + 40, buffer);
        if (result != ESP_OK) break;
    }
    // Keep the DMA buffer alive until the final stripe has left the SPI queue.
    esp_err_t drained = esp_lcd_panel_io_tx_param(lcd_io, -1, NULL, 0);
    if (drained == ESP_OK) free(buffer);
    if (result != ESP_OK) return result;
    return drained;
}

static uint16_t rgb565_be(uint32_t color)
{
    uint16_t value = ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f);
    return (uint16_t)((value >> 8) | (value << 8));
}

// Diagnostic stripes, top to bottom: gray 0-255, dark gray 0-60, red, green, blue ramps (16 steps each),
// then the UI palette beside 1 px white lines on black. Written straight to the panel, bypassing LVGL.
esp_err_t board_draw_ramp(void)
{
    static const uint32_t palette[] = {0x0b0e0d, 0x242c28, 0x4b5550, 0x85918b, 0xa9b6b0, 0xe9efec};
    uint16_t *buffer = heap_caps_malloc(320 * 40 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "test buffer");
    esp_err_t result = ESP_OK;
    for (int stripe = 0; stripe < 6 && result == ESP_OK; stripe++) {
        for (int x = 0; x < 320; x++) {
            uint32_t step = x / 20, color;
            if (stripe == 0) color = (step * 17) * 0x010101;
            else if (stripe == 1) color = (step * 4) * 0x010101;
            else if (stripe < 5) color = (step * 17) << (8 * (4 - stripe));
            else if (x < 192) color = palette[x / 32];
            else color = x % 4 == 0 ? 0xffffff : 0x000000;
            for (int y = 0; y < 40; y++) buffer[y * 320 + x] = rgb565_be(color);
        }
        result = esp_lcd_panel_draw_bitmap(panel, 0, stripe * 40, 320, stripe * 40 + 40, buffer);
        // The buffer is reused for the next stripe, so wait until this one has been sent.
        esp_err_t drained = esp_lcd_panel_io_tx_param(lcd_io, -1, NULL, 0);
        if (result == ESP_OK) result = drained;
    }
    free(buffer);
    return result;
}

// Writes one panel tuning register; callers hold the LVGL port lock so no frame is in flight.
esp_err_t board_lcd_command(uint8_t command, const uint8_t *data, size_t length)
{
    return esp_lcd_panel_io_tx_param(lcd_io, command, length ? data : NULL, length);
}
