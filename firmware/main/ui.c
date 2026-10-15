#include "display.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

// The desk follows prototypes/ui-v3-home design B; the Sub2API module follows prototypes/ui-v2 design A.
// Coordinates are text baselines on the 320x240 panel. The 16 px Chinese font covers GB2312, the 14 px one
// only the characters in this source.
LV_FONT_DECLARE(display_font);
LV_FONT_DECLARE(display_font_14);
#define CJK14 (&display_font_14)
#define CJK16 (&display_font)
#define LEFT LV_TEXT_ALIGN_LEFT
#define RIGHT LV_TEXT_ALIGN_RIGHT
#define CENTER LV_TEXT_ALIGN_CENTER

// VIEW_HOME, VIEW_TODAY and VIEW_DETAIL make up the Sub2API module.
enum { VIEW_HOME, VIEW_TODAY, VIEW_DETAIL, VIEW_DESK, VIEW_SYSTEM };
typedef struct { lv_obj_t *label; int account, window, severity; bool detailed; } live_window_t;

static display_model_t model;
static lv_obj_t *body, *status_label, *app_screen, *test_screen, *wake_layer;
static lv_display_t *ui_display;
static live_window_t live[2 * DISPLAY_MAX_ACCOUNTS];
static int live_count, view = VIEW_DESK, detail, desk_card;
static char detail_id[24];
static bool rendered_stale, screen_off, waking;
static char connection_status[48] = "待配置";
static const uint32_t BG = 0x0b0e0d, SURFACE = 0x141917, LINE = 0x242c28, FAINT = 0x4b5550, TEXT = 0xe9efec,
                      DIM = 0x85918b, NEUTRAL = 0xa9b6b0, GREEN = 0x5ed29c, AMBER = 0xf3b54a, RED = 0xff6e61, GREY = 0x5e6964;
static const uint32_t SEVERITY_COLOR[] = {GREEN, GREY, AMBER, RED};
// Desk carousel: card 0 is the clock and hitokoto, then one card per module. Only implemented modules get a card.
static const struct { const char *icon, *title; int view; } MODULES[] = {
    {LV_SYMBOL_CHARGE, "Sub2API 监控", VIEW_HOME}, {LV_SYMBOL_SETTINGS, "系统信息", VIEW_SYSTEM},
};
#define CARD_COUNT (1 + (int)(sizeof(MODULES) / sizeof(MODULES[0])))
#define CARD_W 272
#define CARD_H 168
#define CARD_X 32
#define CARD_STEP 176
static quote_t quote = {"人生如逆旅，我亦是行人。", "苏轼「临江仙·送钱穆父」", "诗词"};
static bool quote_loading;

static bool clock_valid(void) { return time(NULL) > 1700000000; }

static bool data_stale(void)
{
    return !model.demo && model.generated_at && clock_valid() && time(NULL) > model.generated_at + 120;
}

// Retained drawing: every render pass makes the same helper calls in the same order. An object already at
// that position in its parent, of the same kind, is reused and only properties that changed are written, so a
// data refresh redraws just what changed and never deletes the row under a finger. A view change alters the
// sequence, which drops the old objects from the first mismatch on. User data holds kind | claimed << 4.
enum { KIND_RECT = 1, KIND_TEXT, KIND_RICH_TEXT, KIND_ZONE, KIND_ROW, KIND_CHIP, KIND_PARAGRAPH, KIND_CAROUSEL, KIND_CARD };

static int kind_of(lv_obj_t *obj) { return (int)((uintptr_t)lv_obj_get_user_data(obj) & 15); }
static void reset_claims(lv_obj_t *obj, int kind) { lv_obj_set_user_data(obj, (void *)(uintptr_t)kind); }

// Returns the reusable object for this call, or NULL after dropping the parent's remaining old children.
static lv_obj_t *claim(lv_obj_t *parent, int kind)
{
    uintptr_t meta = (uintptr_t)lv_obj_get_user_data(parent);
    uint32_t index = meta >> 4;
    lv_obj_set_user_data(parent, (void *)(((uintptr_t)(index + 1) << 4) | (meta & 15)));
    lv_obj_t *out = lv_obj_get_child(parent, index);
    if (out && kind_of(out) == kind) { reset_claims(out, kind); return out; }
    while (lv_obj_get_child_count(parent) > index) lv_obj_delete(lv_obj_get_child(parent, index));
    return NULL;
}

// Deletes children the latest pass did not claim, e.g. a shorter account list.
static void trim(lv_obj_t *parent)
{
    uint32_t claimed = (uintptr_t)lv_obj_get_user_data(parent) >> 4;
    while (lv_obj_get_child_count(parent) > claimed) lv_obj_delete(lv_obj_get_child(parent, claimed));
    for (uint32_t i = 0; i < claimed; i++) {
        int kind = kind_of(lv_obj_get_child(parent, i));
        if (kind == KIND_ZONE || kind == KIND_ROW || kind == KIND_CAROUSEL || kind == KIND_CARD) trim(lv_obj_get_child(parent, i));
    }
}

static void place(lv_obj_t *obj, int x, int y, int w, int h)
{
    if (lv_obj_get_style_x(obj, 0) != x || lv_obj_get_style_y(obj, 0) != y) lv_obj_set_pos(obj, x, y);
    if (lv_obj_get_style_width(obj, 0) != w || lv_obj_get_style_height(obj, 0) != h) lv_obj_set_size(obj, w, h);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, hidden);
}

static void set_text_color(lv_obj_t *obj, uint32_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), lv_color_hex(color))) lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

static void set_label_text(lv_obj_t *label, const char *value)
{
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}

// Optional rects stay in the sequence while hidden, so toggling them never rebuilds what follows.
static lv_obj_t *rect_if(bool shown, lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *out = claim(parent, KIND_RECT);
    if (!out) {
        out = lv_obj_create(parent);
        lv_obj_remove_style_all(out);
        lv_obj_remove_flag(out, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(out, LV_OPA_COVER, 0);
        reset_claims(out, KIND_RECT);
    }
    place(out, x, y, w, h);
    set_hidden(out, !shown);
    if (!lv_color_eq(lv_obj_get_style_bg_color(out, 0), lv_color_hex(color))) lv_obj_set_style_bg_color(out, lv_color_hex(color), 0);
    if (lv_obj_get_style_radius(out, 0) != radius) lv_obj_set_style_radius(out, radius, 0);
    return out;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    return rect_if(true, parent, x, y, w, h, color, radius);
}

// x is the left edge, or the right edge for right-aligned text. Rich text interprets #rrggbb colour spans.
static lv_obj_t *text_kind(lv_obj_t *parent, int kind, const char *value, int x, int baseline, int width,
                           lv_text_align_t align, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *out = claim(parent, kind);
    if (!out) {
        out = lv_label_create(parent);
        lv_label_set_long_mode(out, LV_LABEL_LONG_DOT);
        if (kind == KIND_RICH_TEXT) lv_label_set_recolor(out, true);
        reset_claims(out, kind);
    }
    if (lv_obj_get_style_text_font(out, 0) != font) lv_obj_set_style_text_font(out, font, 0);
    set_text_color(out, color);
    if (lv_obj_get_style_text_align(out, 0) != align) lv_obj_set_style_text_align(out, align, 0);
    place(out, align == RIGHT ? x - width : x, baseline - (font->line_height - font->base_line), width, font->line_height);
    set_label_text(out, value);
    return out;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int baseline, int width, lv_text_align_t align,
                      const lv_font_t *font, uint32_t color)
{
    return text_kind(parent, KIND_TEXT, value, x, baseline, width, align, font, color);
}

static int text_width(const char *value, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, value, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// Wrapped text, centred vertically in the box, one line per `pitch` px; overflow ends with an ellipsis.
// LVGL breaks Chinese between any two characters.
static void paragraph(lv_obj_t *parent, const char *value, int x, int y, int w, int h, int pitch, const lv_font_t *font,
                      uint32_t color)
{
    lv_obj_t *out = claim(parent, KIND_PARAGRAPH);
    if (!out) {
        out = lv_label_create(parent);
        lv_label_set_long_mode(out, LV_LABEL_LONG_DOT);
        reset_claims(out, KIND_PARAGRAPH);
    }
    int space = pitch - font->line_height;
    if (lv_obj_get_style_text_font(out, 0) != font) lv_obj_set_style_text_font(out, font, 0);
    if (lv_obj_get_style_text_line_space(out, 0) != space) lv_obj_set_style_text_line_space(out, space, 0);
    set_text_color(out, color);
    lv_point_t size;
    lv_text_get_size(&size, value, font, 0, space, w, LV_TEXT_FLAG_NONE);
    int lines = (size.y + space) / pitch;
    if (lines > h / pitch) lines = h / pitch;
    if (lines < 1) lines = 1;
    int height = lines * pitch - space;
    place(out, x, y + (h - height) / 2, w, height);
    set_label_text(out, value);
}

static void zone_event(lv_obj_t *obj, lv_event_cb_t cb, intptr_t argument)
{
    lv_event_dsc_t *dsc = lv_obj_get_event_count(obj) ? lv_obj_get_event_dsc(obj, 0) : NULL;
    if (dsc && lv_event_dsc_get_cb(dsc) == cb && lv_event_dsc_get_user_data(dsc) == (void *)argument) return;
    if (dsc) lv_obj_remove_event(obj, 0);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, (void *)argument);
}

// A row is a zone that highlights while pressed; a card is a rounded surface that lightens while pressed.
// Neither scrolls, so a drag that starts on a card moves the carousel holding it.
static lv_obj_t *zone_kind(lv_obj_t *parent, int kind, int x, int y, int w, int h, lv_event_cb_t cb, intptr_t argument)
{
    lv_obj_t *out = claim(parent, kind);
    if (!out) {
        out = lv_obj_create(parent);
        lv_obj_remove_style_all(out);
        lv_obj_remove_flag(out, LV_OBJ_FLAG_SCROLLABLE);
        if (kind == KIND_ROW) {
            lv_obj_set_style_bg_color(out, lv_color_hex(SURFACE), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(out, LV_OPA_COVER, LV_STATE_PRESSED);
        } else if (kind == KIND_CARD) {
            lv_obj_set_style_bg_color(out, lv_color_hex(SURFACE), 0);
            lv_obj_set_style_bg_color(out, lv_color_hex(LINE), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(out, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(out, 10, 0);
        }
        reset_claims(out, kind);
    }
    place(out, x, y, w, h);
    zone_event(out, cb, argument);
    return out;
}

static lv_obj_t *zone(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, intptr_t argument)
{
    return zone_kind(parent, KIND_ZONE, x, y, w, h, cb, argument);
}

static void bar(lv_obj_t *parent, int x, int y, int w, int h, float percent, uint32_t color)
{
    rect(parent, x, y, w, h, LINE, h / 2);
    int fill = (int)lroundf(w * fminf(fmaxf(percent, 0), 100) / 100);
    rect_if(fill > 0, parent, x, y, fill > h ? fill : h, h, color, h / 2);
}

static void compact(const char *value, char *out, size_t size)
{
    if (!value || !strcmp(value, "--")) { snprintf(out, size, "--"); return; }
    double number = strtod(value, NULL);
    if (number >= 1000000) snprintf(out, size, "%.1fM", number / 1000000);
    else if (number >= 1000) snprintf(out, size, "%.1fK", number / 1000);
    else snprintf(out, size, "%s", value);
}

static void money(const char *value, char *out, size_t size)
{
    if (!value || !strcmp(value, "--")) snprintf(out, size, "--");
    else snprintf(out, size, "$%.2f", strtod(value, NULL));
}

static void duration(long seconds, char *out, size_t size)
{
    if (seconds < 0) seconds = 0;
    long minutes = seconds / 60, days = minutes / 1440, hours = minutes % 1440 / 60;
    if (days) snprintf(out, size, "%ldd%ldh", days, hours);
    else if (hours) snprintf(out, size, "%ldh%02ldm", hours, minutes % 60);
    else if (minutes < 10) snprintf(out, size, "%ld:%02ld", minutes, seconds % 60);
    else snprintf(out, size, "%ldm", minutes);
}

static void age(long seconds, char *out, size_t size)
{
    if (seconds < 0) seconds = 0;
    if (seconds < 60) snprintf(out, size, "%lds", seconds);
    else if (seconds < 3600) snprintf(out, size, "%ldm", seconds / 60);
    else if (seconds < 86400) snprintf(out, size, "%ldh", seconds / 3600);
    else snprintf(out, size, "%ldd", seconds / 86400);
}

static bool window_sample_old(const quota_window_t *w)
{
    return !strcmp(w->quality, "stale") || (clock_valid() && w->stale_at && w->stale_at <= time(NULL));
}

static bool window_reset_passed(const quota_window_t *w)
{
    return !strcmp(w->quality, "waiting_sample") || (clock_valid() && w->reset_at && w->reset_at <= time(NULL));
}

static int window_severity(const quota_window_t *w)
{
    if (data_stale() || window_reset_passed(w) || window_sample_old(w) || !strcmp(w->quality, "unknown")) return SEVERITY_UNKNOWN;
    return w->used_percent >= 95 ? SEVERITY_CRIT : w->used_percent >= 80 ? SEVERITY_WARN : SEVERITY_OK;
}

static void window_description(const quota_window_t *w, char *out, size_t size, bool detailed)
{
    time_t now = time(NULL);
    char span[24];
    if (data_stale()) snprintf(out, size, "过期");
    else if (window_reset_passed(w)) snprintf(out, size, "待新样本");
    else if (window_sample_old(w)) {
        if (clock_valid() && w->sampled_at && now > w->sampled_at) { age(now - w->sampled_at, span, sizeof(span)); snprintf(out, size, "过期 %s", span); }
        else snprintf(out, size, "过期");
    } else if (!strcmp(w->quality, "unknown")) snprintf(out, size, "未知");
    else if (clock_valid() && w->reset_at > now) {
        duration(w->reset_at - now, span, sizeof(span));
        if (!strcmp(w->quality, "estimated")) {
            if (detailed) snprintf(out, size, "估算 · %s 后重置", span); else snprintf(out, size, "估算");
        } else if (detailed) snprintf(out, size, "%s 后重置", span);
        else snprintf(out, size, "%s", span);
    } else if (!strcmp(w->quality, "estimated")) snprintf(out, size, "估算");
    else snprintf(out, size, "%s", model.demo ? "演示数据" : "无重置时间");
}

static const char *quality_label(const quota_window_t *w)
{
    if (data_stale() || window_sample_old(w)) return "样本过期";
    if (window_reset_passed(w)) return "待新样本";
    if (!strcmp(w->quality, "observed")) return "观测值";
    if (!strcmp(w->quality, "api_reported")) return "API 报告";
    if (!strcmp(w->quality, "estimated")) return "本地估算";
    return "未知";
}

static int account_severity(const display_account_t *account)
{
    int severity = account->block_severity;
    for (int i = 0; i < account->window_count; i++) {
        int s = window_severity(&account->windows[i]);
        if (s > severity) severity = s;
    }
    if (data_stale() && severity < SEVERITY_UNKNOWN) severity = SEVERITY_UNKNOWN;
    return severity;
}

static void track_window(lv_obj_t *label, int account, int window, int severity, bool detailed)
{
    if (live_count < (int)(sizeof(live) / sizeof(live[0])))
        live[live_count++] = (live_window_t){label, account, window, severity, detailed};
}

static void update_status(void)
{
    if (!status_label || screen_off) return;
    char buffer[64], span[16];
    uint32_t color = DIM;
    bool online = !strcmp(connection_status, "在线");
    if (model.demo) snprintf(buffer, sizeof(buffer), "演示数据");
    else if (online && model.generated_at && clock_valid()) {
        long seconds = (long)(time(NULL) - model.generated_at);
        age(seconds, span, sizeof(span));
        if (seconds > 120) { snprintf(buffer, sizeof(buffer), "数据过期 %s", span); color = AMBER; }
        else snprintf(buffer, sizeof(buffer), "在线 %s", span);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", connection_status);
        // Problems matter once there is data on screen; boot-time progress stays neutral.
        if (model.valid && !online) color = AMBER;
    }
    set_text_color(status_label, color);
    set_label_text(status_label, buffer);
}

static void render(void);
static void start_calibration(lv_event_t *event);
static void paint_open(void);
static bool paint_active(void);
static void paint_close(void);
static void render_async(void *unused) { (void)unused; render(); }
static void show(int next) { view = next; lv_async_call(render_async, NULL); }

static void select_account(lv_event_t *event)
{
    detail = (int)(intptr_t)lv_event_get_user_data(event);
    snprintf(detail_id, sizeof(detail_id), "%s", model.accounts[detail].id);
    show(VIEW_DETAIL);
}

static void open_today(lv_event_t *event) { (void)event; show(VIEW_TODAY); }
static void go_home(lv_event_t *event) { (void)event; show(VIEW_HOME); }
static void go_desk(lv_event_t *event) { (void)event; show(VIEW_DESK); }

static void open_paint(lv_event_t *event) { (void)event; paint_open(); }

static void next_quote(lv_event_t *event)
{
    (void)event;
    if (quote_loading || !network_request_quote()) return;
    quote_loading = true;
    lv_async_call(render_async, NULL);
}

static int page_count(void) { int pages = (model.accounts_total + 2) / 3; return pages < 1 ? 1 : pages; }

static void change_page(int delta)
{
    if (model.demo) return;
    int target = model.page + delta;
    if (target >= 1 && (target <= page_count() || (delta > 0 && model.has_more))) network_request_page(target);
}

static void page_event(lv_event_t *event) { change_page((int)(intptr_t)lv_event_get_user_data(event)); }

static void gesture_event(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev); // A swipe must not also click the row it started on.
    if (view == VIEW_TODAY || view == VIEW_DETAIL) { if (dir == LV_DIR_RIGHT) show(VIEW_HOME); }
    else if (view == VIEW_HOME) {
        // Right on the first page leaves the module, like the status bar.
        if (dir == LV_DIR_RIGHT && model.page <= 1) show(VIEW_DESK);
        else change_page(dir == LV_DIR_LEFT ? 1 : -1);
    } else if (view != VIEW_DESK && dir == LV_DIR_RIGHT) show(VIEW_DESK);
}

static void header(const char *title, lv_event_cb_t back)
{
    zone(body, 0, 0, 320, 26, back, 0);
    text(body, LV_SYMBOL_LEFT, 10, 17, 14, LEFT, &lv_font_montserrat_14, DIM);
    text(body, title, 26, 17, 150, LEFT, CJK16, TEXT);
    rect(body, 0, 24, 320, 1, LINE, 0);
}

static void render_slot(lv_obj_t *row, int x, int w, int account, int window)
{
    quota_window_t *win = &model.accounts[account].windows[window];
    int severity = window_severity(win);
    char buffer[48];
    text(row, win->label, x, 20, 44, LEFT, CJK14, DIM);
    snprintf(buffer, sizeof(buffer), "%.0f%%", win->used_percent);
    text(row, buffer, x + w, 20, 56, RIGHT, &lv_font_montserrat_20, SEVERITY_COLOR[severity]);
    bar(row, x, 25, w, 4, win->used_percent, SEVERITY_COLOR[severity]);
    window_description(win, buffer, sizeof(buffer), false);
    track_window(text(row, buffer, x, 43, w, LEFT, CJK14, severity == SEVERITY_UNKNOWN ? GREY : DIM), account, window, severity, false);
}

static void render_row(int index, int top, bool divider)
{
    display_account_t *account = &model.accounts[index];
    char buffer[48];
    lv_obj_t *row = zone_kind(body, KIND_ROW, 0, top, 320, 46, select_account, index);
    int severity = account_severity(account);
    rect_if(severity != SEVERITY_OK, row, 0, 6, 3, 34, SEVERITY_COLOR[severity], 0);
    text(row, account->alias, 10, 20, 96, LEFT, &lv_font_montserrat_16, TEXT);
    if (account->flag[0]) text(row, account->flag, 10, 43, 96, LEFT, CJK14, data_stale() ? GREY : SEVERITY_COLOR[account->flag_severity]);
    else {
        if (account->concurrency >= 0 && account->concurrency_limit > 0) snprintf(buffer, sizeof(buffer), "并发 %d/%d", account->concurrency, account->concurrency_limit);
        else snprintf(buffer, sizeof(buffer), "并发 --");
        text(row, buffer, 10, 43, 96, LEFT, CJK14, DIM);
    }
    if (account->window_count) {
        render_slot(row, 112, 94, index, 0);
        if (account->window_count > 1) render_slot(row, 216, 94, index, 1);
    } else {
        text(row, account->balance[0] ? "余额" : "未采样", 112, 20, 48, LEFT, CJK14, DIM);
        if (account->balance[0]) text(row, account->balance, 310, 20, 146, RIGHT, &lv_font_montserrat_20, TEXT);
        text(row, account->balance[0] ? "无额度窗口" : "暂无额度样本", 112, 43, 198, LEFT, CJK14, GREY);
    }
    if (divider) rect(row, 10, 45, 300, 1, LINE, 0);
}

static void render_home(void)
{
    char buffer[96];
    bool stale = data_stale();
    uint32_t value = stale ? DIM : TEXT;
    zone(body, 0, 0, 320, 25, go_desk, 0);
    text(body, LV_SYMBOL_LEFT, 8, 17, 12, LEFT, &lv_font_montserrat_12, DIM);
    rect(body, 21, 8, 8, 8, stale ? GREY : model.observed_normal < model.accounts_total ? RED : GREEN, 4);
    int used = snprintf(buffer, sizeof(buffer), "%d/%d #85918b 正常#", model.observed_normal, model.accounts_total);
    if (model.alert_accounts) snprintf(buffer + used, sizeof(buffer) - used, "   #f3b54a %d# #85918b 告警#", model.alert_accounts);
    text_kind(body, KIND_RICH_TEXT, buffer, 35, 17, 155, LEFT, CJK14, TEXT);
    status_label = text(body, "", 310, 17, 116, RIGHT, CJK14, DIM);
    update_status();
    rect(body, 0, 24, 320, 1, LINE, 0);

    text(body, "今日账号费用", 10, 44, 130, LEFT, CJK14, DIM);
    money(model.account_cost, buffer, sizeof(buffer)); text(body, buffer, 10, 73, 140, LEFT, &lv_font_montserrat_28, value);
    text(body, "请求", 152, 44, 70, LEFT, CJK14, DIM);
    compact(model.requests, buffer, sizeof(buffer)); text(body, buffer, 152, 73, 80, LEFT, &lv_font_montserrat_20, value);
    text(body, "Token", 234, 44, 60, LEFT, CJK14, DIM);
    compact(model.tokens, buffer, sizeof(buffer)); text(body, buffer, 234, 73, 80, LEFT, &lv_font_montserrat_20, value);
    text(body, LV_SYMBOL_RIGHT, 312, 44, 16, RIGHT, &lv_font_montserrat_12, FAINT);
    zone(body, 0, 25, 320, 56, open_today, 0);
    rect(body, 0, 81, 320, 1, LINE, 0);

    for (int i = 0; i < model.account_count; i++) render_row(i, 82 + i * 46, i < model.account_count - 1);
    if (!model.account_count) text(body, "当前页没有账号", 10, 120, 300, LEFT, CJK14, DIM);

    int pages = page_count();
    text(body, "Sub2API", 10, 235, 80, LEFT, &lv_font_montserrat_12, FAINT);
    snprintf(buffer, sizeof(buffer), "%d/%d", model.page, pages);
    text(body, buffer, 310, 235, 60, RIGHT, &lv_font_montserrat_12, FAINT);
    if (pages > 1 && pages <= 12) {
        int x = 160 - (pages * 11 + 3) / 2;
        for (int i = 1; i <= pages; i++) {
            if (i == model.page) { rect(body, x, 228, 14, 5, DIM, 2); x += 20; }
            else { rect(body, x, 228, 5, 5, FAINT, 2); x += 11; }
        }
    }
    zone(body, 0, 220, 107, 20, page_event, -1);
    zone(body, 213, 220, 107, 20, page_event, 1);
}

static void render_today(void)
{
    char buffer[96];
    header("今日", go_home);
    snprintf(buffer, sizeof(buffer), "%s · %s", model.day, model.timezone);
    text(body, buffer, 310, 17, 200, RIGHT, CJK14, DIM);
    text(body, "账号费用", 10, 46, 120, LEFT, CJK14, DIM);
    money(model.account_cost, buffer, sizeof(buffer)); text(body, buffer, 10, 88, 184, LEFT, &lv_font_montserrat_40, TEXT);
    text(body, "请求", 196, 50, 50, LEFT, CJK14, DIM);
    compact(model.requests, buffer, sizeof(buffer)); text(body, buffer, 310, 50, 70, RIGHT, &lv_font_montserrat_20, TEXT);
    text(body, "Token", 196, 82, 50, LEFT, CJK14, DIM);
    compact(model.tokens, buffer, sizeof(buffer)); text(body, buffer, 310, 82, 70, RIGHT, &lv_font_montserrat_20, TEXT);
    rect(body, 10, 100, 300, 1, LINE, 0);

    // Model distribution of sub2api's usage page, over the same accounts as the totals above. The bar is
    // each row's share of tokens, like the page's chart.
    text(body, "模型分布", 10, 120, 80, LEFT, CJK14, DIM);
    text(body, "请求", 180, 120, 40, RIGHT, CJK14, FAINT);
    text(body, "Token", 240, 120, 50, RIGHT, CJK14, FAINT);
    text(body, "账号费用", 310, 120, 60, RIGHT, CJK14, FAINT);
    double total = 0;
    for (int i = 0; i < model.model_count; i++) total += strtod(model.models[i].tokens, NULL);
    if (!model.models_complete || total <= 0) {
        rect(body, 10, 128, 300, 6, LINE, 0);
        text(body, model.models_complete ? "今日暂无模型用量" : "模型数据不完整", 10, 158, 300, LEFT, CJK14, GREY);
        return;
    }
    const uint32_t shades[] = {TEXT, NEUTRAL, DIM, FAINT};
    int x = 10;
    for (int i = 0; i < model.model_count; i++) {
        double tokens = strtod(model.models[i].tokens, NULL);
        int w = i == model.model_count - 1 ? 310 - x : (int)lround(tokens / total * 300);
        if (w > 0) rect(body, x, 128, w > 2 ? w - 2 : 1, 6, shades[i], 0);
        x += w;
    }
    for (int i = 0; i < model.model_count; i++) {
        model_usage_t *m = &model.models[i];
        int y = 158 + i * 22;
        rect(body, 10, y - 8, 6, 6, shades[i], 0);
        if (m->other_models) {
            snprintf(buffer, sizeof(buffer), "其余 %d 个", m->other_models);
            text(body, buffer, 22, y, 116, LEFT, CJK14, DIM);
        } else text(body, m->name, 22, y, 116, LEFT, &lv_font_montserrat_14, NEUTRAL);
        compact(m->requests, buffer, sizeof(buffer)); text(body, buffer, 180, y, 40, RIGHT, &lv_font_montserrat_14, TEXT);
        compact(m->tokens, buffer, sizeof(buffer)); text(body, buffer, 240, y, 52, RIGHT, &lv_font_montserrat_14, TEXT);
        money(m->cost, buffer, sizeof(buffer)); text(body, buffer, 310, y, 62, RIGHT, &lv_font_montserrat_14, TEXT);
    }
}

static void render_detail(void)
{
    display_account_t *account = &model.accounts[detail];
    char buffer[96];
    header(account->alias, go_home);
    if (account->flag[0]) {
        uint32_t color = data_stale() ? GREY : SEVERITY_COLOR[account->flag_severity];
        lv_obj_t *chip = claim(body, KIND_CHIP);
        if (!chip) {
            chip = lv_label_create(body);
            lv_obj_set_style_text_font(chip, CJK14, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0); lv_obj_set_style_radius(chip, 4, 0);
            lv_obj_set_style_pad_hor(chip, 6, 0);
            lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -10, 3);
            reset_claims(chip, KIND_CHIP);
        }
        set_text_color(chip, color);
        lv_color_t background = lv_color_mix(lv_color_hex(color), lv_color_hex(BG), 56);
        if (!lv_color_eq(lv_obj_get_style_bg_color(chip, 0), background)) lv_obj_set_style_bg_color(chip, background, 0);
        set_label_text(chip, account->flag);
    }
    int shown = account->window_count < 2 ? account->window_count : 2;
    for (int i = 0; i < shown; i++) {
        quota_window_t *w = &account->windows[i];
        int y = 30 + i * 60, severity = window_severity(w);
        snprintf(buffer, sizeof(buffer), "%s 窗口", w->label);
        text(body, buffer, 10, y + 20, 170, LEFT, CJK16, TEXT);
        snprintf(buffer, sizeof(buffer), "%.0f%%", w->used_percent);
        text(body, buffer, 310, y + 24, 110, RIGHT, &lv_font_montserrat_28, SEVERITY_COLOR[severity]);
        bar(body, 10, y + 31, 300, 6, w->used_percent, SEVERITY_COLOR[severity]);
        window_description(w, buffer, sizeof(buffer), true);
        track_window(text(body, buffer, 10, y + 54, w->has_stats ? 140 : 200, LEFT, CJK14, severity == SEVERITY_UNKNOWN ? GREY : DIM), detail, i, severity, true);
        if (w->has_stats) {
            // Same totals as sub2api's usage-window chips; the account cost matches the rest of the device.
            char requests[24], tokens[24], cost[24];
            compact(w->stats_requests, requests, sizeof(requests)); compact(w->stats_tokens, tokens, sizeof(tokens));
            money(w->stats_cost, cost, sizeof(cost));
            snprintf(buffer, sizeof(buffer), "%s 次 · %s · %s", requests, tokens, cost);
            text(body, buffer, 310, y + 54, 160, RIGHT, CJK14, NEUTRAL);
        } else text(body, quality_label(w), 310, y + 54, 90, RIGHT, CJK14, FAINT);
    }
    if (!shown) {
        if (account->balance[0]) snprintf(buffer, sizeof(buffer), "余额 %s", account->balance);
        else snprintf(buffer, sizeof(buffer), "未采样");
        text(body, buffer, 10, 60, 300, LEFT, CJK16, account->balance[0] ? TEXT : GREY);
        text(body, "该账号没有额度窗口", 10, 84, 300, LEFT, CJK14, DIM);
    }
    rect(body, 10, 152, 300, 1, LINE, 0);

    text(body, "并发", 10, 174, 46, LEFT, CJK14, DIM);
    if (account->concurrency >= 0 && account->concurrency_limit > 0) {
        int segments = account->concurrency_limit < 10 ? account->concurrency_limit : 10;
        int lit = (int)lroundf((float)account->concurrency / account->concurrency_limit * segments), width = (180 - 2 * (segments - 1)) / segments;
        for (int i = 0; i < segments; i++) rect(body, 60 + i * (width + 2), 166, width, 8, i < lit ? NEUTRAL : LINE, 1);
        snprintf(buffer, sizeof(buffer), "%d/%d", account->concurrency, account->concurrency_limit);
    } else snprintf(buffer, sizeof(buffer), "--");
    text(body, buffer, 310, 174, 60, RIGHT, &lv_font_montserrat_14, TEXT);

    text(body, "今日", 10, 200, 46, LEFT, CJK14, DIM);
    if (strcmp(account->today_cost, "--")) {
        char cost[24], tokens[24];
        money(account->today_cost, cost, sizeof(cost)); compact(account->today_tokens, tokens, sizeof(tokens));
        snprintf(buffer, sizeof(buffer), "%s · %s 次 · %s", cost, account->today_requests, tokens);
    } else snprintf(buffer, sizeof(buffer), "暂无今日数据");
    text(body, buffer, 310, 200, 250, RIGHT, CJK14, TEXT);

    if (account->windows_total > shown && shown) {
        text(body, "窗口", 10, 226, 46, LEFT, CJK14, DIM);
        snprintf(buffer, sizeof(buffer), "另有 %d 个未显示", account->windows_total - shown);
    } else {
        text(body, "平台", 10, 226, 46, LEFT, CJK14, DIM);
        snprintf(buffer, sizeof(buffer), "%s · %s", account->platform, account->type);
    }
    text(body, buffer, 310, 226, 250, RIGHT, CJK14, DIM);
}

// Local time of the sub2api day's timezone; the app's own TZ stays UTC.
static bool local_time(struct tm *out)
{
    if (!clock_valid() || !model.has_utc_offset) return false;
    time_t local = time(NULL) + model.utc_offset;
    gmtime_r(&local, out);
    return true;
}

static const char *WEEKDAYS[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

// The card that settles in the middle after a swipe; module pages return to it.
static void carousel_scrolled(lv_event_t *event)
{
    int card = (lv_obj_get_scroll_y(lv_event_get_target_obj(event)) + CARD_STEP / 2) / CARD_STEP;
    if (card < 0 || card >= CARD_COUNT || card == desk_card) return;
    desk_card = card;
    lv_async_call(render_async, NULL);
}

static void card_event(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    // A tap on a neighbour peeking in above or below brings it to the middle.
    if (index != desk_card) lv_obj_scroll_to_y(lv_obj_get_parent(lv_event_get_target_obj(event)), index * CARD_STEP, LV_ANIM_ON);
    else if (index == 0) next_quote(event);
    else show(MODULES[index - 1].view);
}

static bool wifi_linked(wifi_ap_record_t *ap) { return esp_wifi_sta_get_ap_info(ap) == ESP_OK; }

static void render_clock_card(lv_obj_t *card, const struct tm *local, bool known, bool linked)
{
    char buffer[160];
    if (known) strftime(buffer, sizeof(buffer), "%H:%M", local); else snprintf(buffer, sizeof(buffer), "--:--");
    int width = text_width(buffer, &lv_font_montserrat_40);
    text(card, buffer, 12, 50, width + 4, LEFT, &lv_font_montserrat_40, TEXT);
    if (known) snprintf(buffer, sizeof(buffer), ":%02d", local->tm_sec); else snprintf(buffer, sizeof(buffer), ":--");
    text(card, buffer, 14 + width, 50, 40, LEFT, &lv_font_montserrat_20, DIM);
    text(card, "一言", 260, 26, 60, RIGHT, CJK14, FAINT);
    text(card, quote_loading ? "获取中" : quote.type, 260, 50, 80, RIGHT, CJK14, DIM);
    rect(card, 12, 62, 248, 1, LINE, 0);
    // max_length=40 at 15 characters a line fills at most these three lines.
    paragraph(card, quote.text, 12, 64, 248, 76, 24, CJK16, linked ? TEXT : NEUTRAL);
    if (quote.source[0]) snprintf(buffer, sizeof(buffer), "—— %s", quote.source); else buffer[0] = 0;
    text(card, buffer, 260, 158, 248, RIGHT, CJK16, DIM);
}

// The window to show for an account on the card: the most severe, then the most used.
static int worst_window(const display_account_t *account)
{
    int worst = 0;
    for (int i = 1; i < account->window_count; i++) {
        int s = window_severity(&account->windows[i]), t = window_severity(&account->windows[worst]);
        if (s > t || (s == t && account->windows[i].used_percent > account->windows[worst].used_percent)) worst = i;
    }
    return worst;
}

static void render_sub2api_card(lv_obj_t *card)
{
    char buffer[96], number[32];
    text(card, MODULES[0].icon, 12, 24, 20, LEFT, &lv_font_montserrat_16, DIM);
    text(card, MODULES[0].title, 34, 24, 120, LEFT, CJK16, TEXT);
    if (!model.valid) {
        text(card, "暂无数据", 12, 70, 248, LEFT, CJK16, TEXT);
        text(card, connection_status, 12, 94, 248, LEFT, CJK16, DIM);
        return;
    }
    int used = snprintf(buffer, sizeof(buffer), "%d/%d #85918b 正常#", model.observed_normal, model.accounts_total);
    if (model.alert_accounts) snprintf(buffer + used, sizeof(buffer) - used, "  #f3b54a %d# #85918b 告警#", model.alert_accounts);
    text_kind(card, KIND_RICH_TEXT, buffer, 260, 24, 130, RIGHT, CJK14, TEXT);
    text(card, "今日账号费用", 12, 52, 130, LEFT, CJK14, DIM);
    money(model.account_cost, buffer, sizeof(buffer));
    text(card, buffer, 12, 92, 160, LEFT, &lv_font_montserrat_40, data_stale() ? DIM : TEXT);
    compact(model.requests, number, sizeof(number)); snprintf(buffer, sizeof(buffer), "%s 请求", number);
    text(card, buffer, 260, 70, 86, RIGHT, CJK14, DIM);
    compact(model.tokens, number, sizeof(number)); snprintf(buffer, sizeof(buffer), "%s Token", number);
    text(card, buffer, 260, 92, 86, RIGHT, CJK14, DIM);
    rect(card, 12, 106, 248, 1, LINE, 0);
    if (!model.account_count) text(card, "当前页没有账号", 12, 140, 248, LEFT, CJK14, DIM);
    for (int i = 0; i < model.account_count && i < 3; i++) {
        display_account_t *account = &model.accounts[i];
        int x = 12 + i * 84;
        text(card, account->alias, x, 128, 76, LEFT, &lv_font_montserrat_14, DIM);
        if (account->window_count) {
            quota_window_t *w = &account->windows[worst_window(account)];
            uint32_t color = SEVERITY_COLOR[window_severity(w)];
            bar(card, x, 136, 72, 4, w->used_percent, color);
            snprintf(buffer, sizeof(buffer), "%s %.0f%%", w->label, w->used_percent);
            text(card, buffer, x, 158, 78, LEFT, CJK16, color);
        } else text(card, account->balance[0] ? account->balance : "未采样", x, 158, 78, LEFT, CJK16, account->balance[0] ? TEXT : GREY);
    }
}

static bool printable_ascii(const char *value)
{
    for (; *value; value++) if (*value < 0x20 || *value > 0x7e) return false;
    return true;
}

static void render_system_card(lv_obj_t *card, const wifi_ap_record_t *ap, bool linked)
{
    char buffer[96], span[24];
    text(card, MODULES[1].icon, 12, 24, 20, LEFT, &lv_font_montserrat_16, DIM);
    text(card, MODULES[1].title, 34, 24, 120, LEFT, CJK16, TEXT);
    duration((long)(esp_timer_get_time() / 1000000), span, sizeof(span));
    snprintf(buffer, sizeof(buffer), "运行 %s", span);
    text(card, buffer, 260, 24, 110, RIGHT, CJK14, DIM);
    text(card, "Wi-Fi", 12, 52, 60, LEFT, CJK14, DIM);
    if (linked) {
        // Montserrat 28 has only ASCII; other SSIDs fall back to the GB2312 font.
        snprintf(buffer, sizeof(buffer), "%.32s", (const char *)ap->ssid);
        bool ascii = printable_ascii(buffer);
        text(card, buffer, 12, ascii ? 92 : 88, 176, LEFT, ascii ? &lv_font_montserrat_28 : CJK16, TEXT);
        snprintf(buffer, sizeof(buffer), "%d dBm", ap->rssi);
        text(card, buffer, 260, 92, 70, RIGHT, CJK14, DIM);
    } else text(card, connection_status, 12, 88, 248, LEFT, CJK16, model.demo || !strcmp(connection_status, "待配置") ? DIM : AMBER);
    rect(card, 12, 106, 248, 1, LINE, 0);
    static const char *labels[] = {"内存", "PSRAM", "版本"};
    for (int i = 0; i < 3; i++) {
        if (i == 0) snprintf(buffer, sizeof(buffer), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        else if (i == 1) snprintf(buffer, sizeof(buffer), "%.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
        else snprintf(buffer, sizeof(buffer), "%.7s", esp_app_get_description()->version);
        text(card, labels[i], 12 + i * 84, 128, 78, LEFT, CJK14, DIM);
        text(card, buffer, 12 + i * 84, 152, 78, LEFT, &lv_font_montserrat_16, TEXT);
    }
}

static void render_desk(void)
{
    char buffer[96];
    struct tm local;
    bool known = local_time(&local);
    wifi_ap_record_t ap;
    bool linked = wifi_linked(&ap);

    // The clock card already shows the time, so the bar adds it only on the other cards.
    char date[48];
    if (known) snprintf(date, sizeof(date), "%d月%d日 %s", local.tm_mon + 1, local.tm_mday, WEEKDAYS[local.tm_wday]);
    else snprintf(date, sizeof(date), "%s", clock_valid() ? "等待 bridge 时区" : "等待校时");
    if (known && desk_card) snprintf(buffer, sizeof(buffer), "%02d:%02d  #85918b %s#", local.tm_hour, local.tm_min, date);
    else snprintf(buffer, sizeof(buffer), "#85918b %s#", date);
    text_kind(body, KIND_RICH_TEXT, buffer, 10, 17, 190, LEFT, CJK14, TEXT);
    if (linked) snprintf(buffer, sizeof(buffer), "%d dBm", ap.rssi); else snprintf(buffer, sizeof(buffer), "%s", connection_status);
    uint32_t color = linked || model.demo || !strcmp(connection_status, "待配置") ? DIM : AMBER;
    text(body, buffer, 310, 17, 110, RIGHT, CJK14, color);
    text(body, LV_SYMBOL_WIFI, 306 - text_width(buffer, CJK14), 17, 20, RIGHT, &lv_font_montserrat_14, color);

    // Cards stack 8 px apart with 16 px of the neighbours showing above and below; snapping centres one card per
    // swipe. Swipes are vertical and the dots sit in the left column, because the panel's left edge reports
    // touches poorly (section 8 of docs/display-ui-and-panel.md).
    lv_obj_t *carousel = claim(body, KIND_CAROUSEL);
    bool created = !carousel;
    if (created) {
        carousel = lv_obj_create(body);
        lv_obj_remove_style_all(carousel);
        lv_obj_set_scroll_dir(carousel, LV_DIR_VER);
        lv_obj_set_scroll_snap_y(carousel, LV_SCROLL_SNAP_CENTER);
        lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(carousel, LV_OBJ_FLAG_SCROLL_ONE);
        lv_obj_set_style_pad_bottom(carousel, 24, 0);
        lv_obj_add_event_cb(carousel, carousel_scrolled, LV_EVENT_SCROLL_END, NULL);
        reset_claims(carousel, KIND_CAROUSEL);
    }
    place(carousel, 0, 24, 320, 216);
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_obj_t *card = zone_kind(carousel, KIND_CARD, CARD_X, 24 + i * CARD_STEP, CARD_W, CARD_H, card_event, i);
        if (i == 0) render_clock_card(card, &local, known, linked);
        else if (MODULES[i - 1].view == VIEW_HOME) render_sub2api_card(card);
        else render_system_card(card, &ap, linked);
    }
    if (created) {
        lv_obj_update_layout(carousel);
        lv_obj_scroll_to_y(carousel, desk_card * CARD_STEP, LV_ANIM_OFF);
    }

    // Position dots, centred on the middle card; display only.
    int y = 132 - (CARD_COUNT * 11 + 3) / 2;
    for (int i = 0; i < CARD_COUNT; i++) {
        if (i == desk_card) { rect(body, 14, y, 5, 14, DIM, 2); y += 20; }
        else { rect(body, 14, y, 5, 5, FAINT, 2); y += 11; }
    }
}

static void render_system(void)
{
    char buffer[96];
    header("系统信息", go_desk);
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    const esp_app_desc_t *app = esp_app_get_description();
    for (int i = 0; i < 6; i++) {
        static const char *labels[] = {"Wi-Fi", "IP 地址", "运行时长", "内部 RAM 可用", "PSRAM 可用", "版本"};
        int y = 48 + i * 28;
        const lv_font_t *font = CJK14;
        text(body, labels[i], 10, y, 110, LEFT, CJK14, DIM);
        if (i == 0) {
            // SSIDs can be any text, so use the GB2312 font.
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { snprintf(buffer, sizeof(buffer), "%.32s · %d dBm", (char *)ap.ssid, ap.rssi); font = CJK16; }
            else snprintf(buffer, sizeof(buffer), "%s", connection_status);
        } else if (i == 1) {
            if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) snprintf(buffer, sizeof(buffer), IPSTR, IP2STR(&ip.ip));
            else snprintf(buffer, sizeof(buffer), "--");
        } else if (i == 2) duration((long)(esp_timer_get_time() / 1000000), buffer, sizeof(buffer));
        else if (i == 3) snprintf(buffer, sizeof(buffer), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        else if (i == 4) snprintf(buffer, sizeof(buffer), "%.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
        else snprintf(buffer, sizeof(buffer), "%s · IDF %s", app->version, app->idf_ver);
        text(body, buffer, 310, y, 190, RIGHT, font, TEXT);
        rect(body, 10, y + 9, 300, 1, LINE, 0);
    }
    // Touch tools: the paint board shows which places can be reached, calibration fixes the offsets.
    text(body, "触摸", 10, 234, 120, LEFT, CJK14, DIM);
    lv_obj_t *row = zone_kind(body, KIND_ROW, 170, 216, 70, 24, open_paint, 0);
    text(row, "画板", 0, 18, 70, CENTER, CJK14, TEXT);
    row = zone_kind(body, KIND_ROW, 250, 216, 70, 24, start_calibration, 0);
    text(row, "校准", 0, 18, 70, CENTER, CJK14, TEXT);
}

static void render_empty(void)
{
    zone(body, 0, 0, 320, 25, go_desk, 0);
    text(body, LV_SYMBOL_LEFT, 8, 17, 12, LEFT, &lv_font_montserrat_12, DIM);
    text(body, "Sub2API", 22, 17, 120, LEFT, &lv_font_montserrat_14, TEXT);
    status_label = text(body, "", 310, 17, 160, RIGHT, CJK14, DIM);
    update_status();
    rect(body, 0, 24, 320, 1, LINE, 0);
    text(body, "暂无数据", 10, 76, 300, LEFT, CJK16, TEXT);
    text(body, "请通过 USB 配置联网", 10, 102, 300, LEFT, CJK14, DIM);
}

static void render(void)
{
    live_count = 0; status_label = NULL;
    rendered_stale = data_stale();
    reset_claims(body, KIND_ZONE);
    if (view == VIEW_DESK) render_desk();
    else if (view == VIEW_SYSTEM) render_system();
    else if (!model.valid) render_empty();
    else {
        if (view == VIEW_DETAIL && (detail < 0 || detail >= model.account_count)) view = VIEW_HOME;
        if (view == VIEW_TODAY) render_today();
        else if (view == VIEW_DETAIL) render_detail();
        else render_home();
    }
    trim(body);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (screen_off) return;
    // The desk clock and system figures change every second; retained drawing writes only what changed.
    if (view == VIEW_DESK || view == VIEW_SYSTEM) { render(); return; }
    if (model.valid && data_stale() != rendered_stale) { render(); return; }
    update_status();
    char buffer[48];
    for (int i = 0; i < live_count; i++) {
        quota_window_t *w = &model.accounts[live[i].account].windows[live[i].window];
        // Reset or staleness changes colours as well as text, so redraw the view.
        if (window_severity(w) != live[i].severity) { render(); return; }
        window_description(w, buffer, sizeof(buffer), live[i].detailed);
        if (strcmp(lv_label_get_text(live[i].label), buffer)) lv_label_set_text(live[i].label, buffer);
    }
}

// Screen off darkens the backlight and pauses drawing while data keeps arriving. Waking redraws the current
// values and lights the panel only once that frame has been sent, so the stale frame never shows.
static void set_screen(bool on)
{
    if (!on && board_set_backlight(false) != ESP_OK) return;
    screen_off = !on;
    waking = on;
    lv_obj_set_flag(wake_layer, LV_OBJ_FLAG_HIDDEN, on);
    if (on) { render(); lv_obj_invalidate(lv_screen_active()); }
    ESP_LOGI("ui", "screen %s", on ? "on" : "off");
}

static void refresh_ready(lv_event_t *event)
{
    (void)event;
    if (!waking) return;
    waking = false;
    if (board_set_backlight(true) != ESP_OK) ESP_LOGW("ui", "backlight on failed");
}

static void wake_event(lv_event_t *event)
{
    (void)event;
    lv_indev_wait_release(lv_indev_active()); // The waking touch must not also press the view below.
    set_screen(true);
}

static void key_poll(lv_timer_t *timer)
{
    (void)timer;
    static bool last, stable;
    bool pressed = board_key_pressed();
    // Two equal 20 ms samples debounce the contact; a press toggles the screen.
    if (pressed == last && pressed != stable) {
        stable = pressed;
        if (pressed && paint_active()) paint_close();
        else if (pressed) set_screen(screen_off);
    }
    last = pressed;
}

void ui_init(lv_display_t *display)
{
    lvgl_port_lock(0);
    ui_display = display;
    lv_obj_t *root = lv_display_get_screen_active(display);
    app_screen = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    body = lv_obj_create(root); lv_obj_remove_style_all(body); lv_obj_set_pos(body, 0, 0); lv_obj_set_size(body, 320, 240);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(body, gesture_event, LV_EVENT_GESTURE, NULL);
    // Transparent top-layer cover, shown only while the screen is off, so the first touch just wakes it.
    wake_layer = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(wake_layer); lv_obj_set_size(wake_layer, 320, 240);
    lv_obj_add_flag(wake_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wake_layer, wake_event, LV_EVENT_PRESSED, NULL);
    lv_display_add_event_cb(display, refresh_ready, LV_EVENT_REFR_READY, NULL);
    render(); lv_timer_create(tick, 1000, NULL); lv_timer_create(key_poll, 20, NULL);
    lvgl_port_unlock();
}

void ui_set_model(const display_model_t *incoming)
{
    lvgl_port_lock(0);
    model = *incoming;
    if (view == VIEW_DETAIL) {
        // Keep the detail page only while the same account is still on this page.
        detail = -1;
        for (int i = 0; i < model.account_count; i++) if (!strcmp(model.accounts[i].id, detail_id)) detail = i;
        if (detail < 0) view = VIEW_HOME;
    }
    if (!screen_off) render(); // Waking renders whatever arrived meanwhile.
    lvgl_port_unlock();
}

void ui_set_quote(const quote_t *incoming)
{
    lvgl_port_lock(0);
    if (incoming) quote = *incoming;
    quote_loading = false;
    if (!screen_off && view == VIEW_DESK) render();
    lvgl_port_unlock();
}

static uint32_t utf8_next(const char **cursor)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    int extra = s[0] < 0x80 ? 0 : s[0] >= 0xf0 ? 3 : s[0] >= 0xe0 ? 2 : s[0] >= 0xc0 ? 1 : -1;
    if (extra < 0) { (*cursor)++; return 0xfffd; }
    uint32_t letter = s[0] & (extra ? 0x3f >> extra : 0x7f);
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xc0) != 0x80) { *cursor += i; return 0xfffd; }
        letter = letter << 6 | (s[i] & 0x3f);
    }
    *cursor += extra + 1;
    return letter;
}

// True when the 16 px desk font has every character, so network text never draws blank boxes.
bool ui_text_supported(const char *value)
{
    lv_font_glyph_dsc_t glyph;
    bool supported = true;
    lvgl_port_lock(0);
    while (supported && *value) supported = lv_font_get_glyph_dsc(CJK16, &glyph, utf8_next(&value), 0);
    lvgl_port_unlock();
    return supported;
}

void ui_set_status(const char *status)
{
    lvgl_port_lock(0); snprintf(connection_status, sizeof(connection_status), "%s", status);
    update_status(); lvgl_port_unlock();
}

// USB `view` command: 0 Sub2API home, 1 today, 2 detail of the first account on the page, 3 desk, 4 system,
// 5 paint board.
void ui_select_view(int selected)
{
    static const int views[] = {VIEW_HOME, VIEW_TODAY, VIEW_DETAIL, VIEW_DESK, VIEW_SYSTEM};
    if (selected < 0 || selected > 5) return;
    lvgl_port_lock(0);
    if (selected == 5) { paint_open(); lvgl_port_unlock(); return; }
    if (paint_active()) paint_close();
    view = views[selected];
    detail = 0;
    if (model.account_count) snprintf(detail_id, sizeof(detail_id), "%s", model.accounts[0].id);
    render();
    lvgl_port_unlock();
}

// Touch calibration: tap each of nine crosshairs three times. Raw controller points are averaged per target
// and fitted (board_calibrate_touch); afterwards a dot shows the corrected point so the result can be checked.
#define CAL_TARGETS 9
#define CAL_TAPS 3
static lv_obj_t *touch_screen, *touch_dot, *touch_prompt, *touch_done, *touch_cross[CAL_TARGETS][2];
static int cal_index, cal_taps, cal_sum[2], cal_raw[CAL_TARGETS][2], cal_screen[CAL_TARGETS][2];

static lv_obj_t *test_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *out = lv_obj_create(parent);
    lv_obj_remove_style_all(out);
    lv_obj_remove_flag(out, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(out, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(out, lv_color_hex(color), 0);
    lv_obj_set_style_radius(out, radius, 0);
    lv_obj_set_pos(out, x, y); lv_obj_set_size(out, w, h);
    return out;
}

static void calibration_show(void)
{
    char buffer[64];
    for (int i = 0; i < CAL_TARGETS; i++) {
        bool current = i == cal_index;
        int x = cal_screen[i][0], y = cal_screen[i][1], arm = current ? 15 : 8;
        for (int k = 0; k < 2; k++) {
            lv_obj_set_style_bg_color(touch_cross[i][k], lv_color_hex(current ? AMBER : cal_index > i ? GREEN : FAINT), 0);
            if (k == 0) { lv_obj_set_pos(touch_cross[i][k], x - arm, y); lv_obj_set_size(touch_cross[i][k], 2 * arm + 1, 1); }
            else { lv_obj_set_pos(touch_cross[i][k], x, y - arm); lv_obj_set_size(touch_cross[i][k], 1, 2 * arm + 1); }
        }
    }
    lv_obj_set_flag(touch_done, LV_OBJ_FLAG_HIDDEN, cal_index < CAL_TARGETS);
    if (cal_index < CAL_TARGETS) {
        snprintf(buffer, sizeof(buffer), "点橙色十字中心  %d/%d · 第 %d 下", cal_index + 1, CAL_TARGETS, cal_taps + 1);
        lv_label_set_text(touch_prompt, buffer);
    }
}

static void calibration_start(void)
{
    cal_index = cal_taps = cal_sum[0] = cal_sum[1] = 0;
    board_touch_raw(true);
    lv_obj_add_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
    calibration_show();
}

static void calibration_finish(void)
{
    float worst = 0;
    esp_err_t result = board_calibrate_touch(cal_raw, cal_screen, CAL_TARGETS, &worst);
    ESP_LOGI("touch", "calibration %s, worst target %.1f px", esp_err_to_name(result), worst);
    if (result != ESP_OK) {
        calibration_start();
        lv_label_set_text_fmt(touch_prompt, "误差过大 (%d px)，请重新开始  1/%d", (int)worst, CAL_TARGETS);
        return;
    }
    board_touch_raw(false);
    calibration_show();
    lv_label_set_text(touch_prompt, "校准完成：点十字检查，红点应在指下");
}

static void calibration_event(lv_event_t *event)
{
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    lv_obj_remove_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(touch_dot, point.x - 4, point.y - 4);
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    if (cal_index >= CAL_TARGETS) {
        ESP_LOGI("touch", "check x=%d y=%d", (int)point.x, (int)point.y);
        if (point.x >= 110 && point.x < 210 && point.y >= 60 && point.y < 96) {
            lv_screen_load(app_screen);
            render(); lv_obj_invalidate(app_screen);
        }
        return;
    }
    // Uncorrected taps land up to ~50 px away; anything further is a stray touch.
    int dx = point.x - cal_screen[cal_index][0], dy = point.y - cal_screen[cal_index][1];
    if (dx * dx + dy * dy > 90 * 90) return;
    ESP_LOGI("touch", "target %d raw x=%d y=%d", cal_index + 1, (int)point.x, (int)point.y);
    cal_sum[0] += point.x; cal_sum[1] += point.y;
    if (++cal_taps == CAL_TAPS) {
        cal_raw[cal_index][0] = cal_sum[0] / CAL_TAPS; cal_raw[cal_index][1] = cal_sum[1] / CAL_TAPS;
        cal_taps = cal_sum[0] = cal_sum[1] = 0;
        if (++cal_index == CAL_TARGETS) { calibration_finish(); return; }
    }
    calibration_show();
}

static void calibration_open(void)
{
    if (!touch_screen) {
        touch_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(touch_screen);
        lv_obj_remove_flag(touch_screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_style_bg_opa(touch_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(touch_screen, lv_color_hex(BG), 0);
        for (int i = 0; i < CAL_TARGETS; i++) {
            cal_screen[i][0] = 30 + i % 3 * 130; cal_screen[i][1] = 30 + i / 3 * 90;
            for (int k = 0; k < 2; k++) touch_cross[i][k] = test_rect(touch_screen, 0, 0, 1, 1, FAINT, 0);
        }
        touch_prompt = lv_label_create(touch_screen);
        lv_obj_set_style_text_font(touch_prompt, CJK14, 0);
        lv_obj_set_style_text_color(touch_prompt, lv_color_hex(TEXT), 0);
        lv_obj_align(touch_prompt, LV_ALIGN_TOP_MID, 0, 160);
        touch_done = test_rect(touch_screen, 110, 60, 100, 36, LINE, 8);
        lv_obj_t *label = lv_label_create(touch_done);
        lv_obj_set_style_text_font(label, CJK16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TEXT), 0);
        lv_label_set_text(label, "完成");
        lv_obj_center(label);
        touch_dot = test_rect(touch_screen, 0, 0, 9, 9, RED, 4);
        lv_obj_add_event_cb(touch_screen, calibration_event, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(touch_screen, calibration_event, LV_EVENT_RELEASED, NULL);
    }
    calibration_start();
    lv_screen_load(touch_screen);
}

static void start_calibration(lv_event_t *event) { (void)event; calibration_open(); }

// Paint board: the calibrated point paints a white brush and the controller's raw point a red dot, so the
// screen shows which places can be reached at all. Coverage is counted in 10 px cells and logged on exit.
#define PAINT_COLS 32
#define PAINT_ROWS 24
static lv_obj_t *paint_screen, *paint_canvas, *paint_label;
static uint16_t *paint_pixels;
static bool paint_cells[2][PAINT_ROWS][PAINT_COLS], paint_has_last;
static lv_area_t paint_box[2];
static lv_point_t paint_last;

static void paint_mark(int layer, int x, int y)
{
    if (x < 0 || x > 319 || y < 0 || y > 239) return;
    paint_cells[layer][y / 10][x / 10] = true;
    lv_area_t *box = &paint_box[layer];
    if (x < box->x1) box->x1 = x;
    if (x > box->x2) box->x2 = x;
    if (y < box->y1) box->y1 = y;
    if (y > box->y2) box->y2 = y;
}

static int paint_coverage(int layer)
{
    int cells = 0;
    for (int row = 0; row < PAINT_ROWS; row++) for (int col = 0; col < PAINT_COLS; col++) cells += paint_cells[layer][row][col];
    return cells * 100 / (PAINT_ROWS * PAINT_COLS);
}

static void paint_dot(int cx, int cy, int radius, uint32_t color)
{
    uint16_t value = lv_color_to_u16(lv_color_hex(color));
    lv_area_t area = {cx - radius, cy - radius, cx + radius, cy + radius};
    if (area.x1 < 0) area.x1 = 0;
    if (area.y1 < 0) area.y1 = 0;
    if (area.x2 > 319) area.x2 = 319;
    if (area.y2 > 239) area.y2 = 239;
    if (area.x1 > area.x2 || area.y1 > area.y2) return;
    for (int y = area.y1; y <= area.y2; y++)
        for (int x = area.x1; x <= area.x2; x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius) paint_pixels[y * 320 + x] = value;
    lv_obj_invalidate_area(paint_canvas, &area);
}

static void paint_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) { paint_has_last = false; lv_obj_add_flag(paint_label, LV_OBJ_FLAG_HIDDEN); }
    if (code == LV_EVENT_RELEASED) {
        lv_label_set_text_fmt(paint_label, "覆盖 %d%% · 原始 %d%% · 红点为原始坐标 · 用户键退出", paint_coverage(0), paint_coverage(1));
        lv_obj_remove_flag(paint_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int count;
    lv_point_t point, second, raw = board_touch_last_raw(&count, &second);
    lv_indev_get_point(lv_indev_active(), &point);
    if (count > 1) ESP_LOGI("touch", "paint n=%d raw %d %d second %d %d -> %d %d", count, (int)raw.x, (int)raw.y,
                            (int)second.x, (int)second.y, (int)point.x, (int)point.y);
    else ESP_LOGI("touch", "paint n=1 raw %d %d -> %d %d", (int)raw.x, (int)raw.y, (int)point.x, (int)point.y);
    ESP_LOGI("touch", "paint raw %d %d -> %d %d", (int)raw.x, (int)raw.y, (int)point.x, (int)point.y);
    // Stamp the white brush every 2 px along the move so fast strokes stay continuous.
    int steps = 1;
    if (paint_has_last) {
        int dx = point.x - paint_last.x, dy = point.y - paint_last.y;
        steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 2 + 1;
    }
    for (int i = 1; i <= steps; i++) {
        int x = paint_has_last ? paint_last.x + (point.x - paint_last.x) * i / steps : point.x;
        int y = paint_has_last ? paint_last.y + (point.y - paint_last.y) * i / steps : point.y;
        paint_dot(x, y, 4, TEXT);
        paint_mark(0, x, y);
    }
    paint_dot(raw.x, raw.y, 1, RED);
    paint_mark(1, raw.x, raw.y);
    paint_last = point;
    paint_has_last = true;
}

static void paint_open(void)
{
    if (!paint_screen) {
        paint_pixels = heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!paint_pixels) { ESP_LOGW("ui", "paint board: no memory"); return; }
        paint_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(paint_screen);
        lv_obj_remove_flag(paint_screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        paint_canvas = lv_canvas_create(paint_screen);
        lv_obj_remove_flag(paint_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_buffer(paint_canvas, paint_pixels, 320, 240, LV_COLOR_FORMAT_RGB565);
        paint_label = lv_label_create(paint_screen);
        lv_obj_set_style_text_font(paint_label, CJK14, 0);
        lv_obj_set_style_text_color(paint_label, lv_color_hex(DIM), 0);
        lv_obj_set_style_bg_color(paint_label, lv_color_hex(SURFACE), 0);
        lv_obj_set_style_bg_opa(paint_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(paint_label, 6, 0);
        lv_obj_align(paint_label, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_add_event_cb(paint_screen, paint_event, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(paint_screen, paint_event, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(paint_screen, paint_event, LV_EVENT_RELEASED, NULL);
    }
    lv_canvas_fill_bg(paint_canvas, lv_color_hex(BG), LV_OPA_COVER);
    memset(paint_cells, 0, sizeof(paint_cells));
    for (int layer = 0; layer < 2; layer++) paint_box[layer] = (lv_area_t){319, 239, 0, 0};
    lv_label_set_text(paint_label, "在屏幕各处涂抹 · 红点为原始坐标 · 用户键退出");
    lv_obj_remove_flag(paint_label, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(paint_screen);
}

static bool paint_active(void) { return paint_screen && lv_screen_active() == paint_screen; }

static void paint_close(void)
{
    for (int layer = 0; layer < 2; layer++)
        ESP_LOGI("touch", "paint %s: coverage %d%%, reached x %d..%d y %d..%d", layer ? "raw" : "calibrated", paint_coverage(layer),
                 (int)paint_box[layer].x1, (int)paint_box[layer].x2, (int)paint_box[layer].y1, (int)paint_box[layer].y2);
    // Maps of the 10 px cells reached, calibrated on the left and raw on the right.
    char line[2 * PAINT_COLS + 2];
    for (int row = 0; row < PAINT_ROWS; row++) {
        for (int col = 0; col < PAINT_COLS; col++) {
            line[col] = paint_cells[0][row][col] ? '#' : '.';
            line[PAINT_COLS + 1 + col] = paint_cells[1][row][col] ? '#' : '.';
        }
        line[PAINT_COLS] = ' ';
        line[2 * PAINT_COLS + 1] = 0;
        ESP_LOGI("touch", "paint %3d %s", row * 10, line);
    }
    lv_screen_load(app_screen);
    view = VIEW_SYSTEM; // The paint board is opened from the system page.
    render(); lv_obj_invalidate(app_screen);
}

esp_err_t ui_display_test(const char *pattern)
{
    const struct { const char *name; uint32_t color; } patterns[] = {
        {"black", 0x000000}, {"gray", 0x808080}, {"white", 0xffffff},
        {"red", 0xff0000}, {"green", 0x00ff00}, {"blue", 0x0000ff},
    };
    bool restore = !strcmp(pattern, "ui");
    bool off = !strcmp(pattern, "off");
    bool ramp = !strcmp(pattern, "ramp");
    bool touch = !strcmp(pattern, "touch");
    uint32_t color = 0;
    bool valid = restore || off || ramp || touch;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (!strcmp(pattern, patterns[i].name)) {
            color = patterns[i].color;
            valid = true;
            break;
        }
    }
    if (!valid) return ESP_ERR_INVALID_ARG;
    lvgl_port_lock(0);
    esp_err_t result = board_set_backlight(false);
    if (result == ESP_OK && !off) {
        if (restore) {
            lv_screen_load(app_screen);
            lv_obj_invalidate(app_screen);
        } else if (touch) {
            calibration_open();
            lv_obj_invalidate(touch_screen);
        } else {
            if (!test_screen) {
                test_screen = lv_obj_create(NULL);
                lv_obj_remove_style_all(test_screen);
                lv_obj_remove_flag(test_screen, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_bg_opa(test_screen, LV_OPA_COVER, 0);
            }
            lv_obj_set_style_bg_color(test_screen, lv_color_hex(color), 0);
            lv_screen_load(test_screen);
            lv_obj_invalidate(test_screen);
        }
        lv_refr_now(ui_display);
        // Bypass LVGL's drawing buffers as well: every LCD pixel is overwritten.
        if (ramp) result = board_draw_ramp();
        else if (!restore && !touch) result = board_fill_color(color);
        if (result == ESP_OK) result = board_set_backlight(true);
    }
    lvgl_port_unlock();
    return result;
}

esp_err_t ui_lcd_command(uint8_t command, const uint8_t *data, size_t length)
{
    // Only gamma, voltage and inversion registers, so tuning cannot move or corrupt the frame.
    static const uint8_t allowed[] = {0x20, 0x21, 0x26, 0xb7, 0xbb, 0xc0, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xd0, 0xe0, 0xe1};
    if (length > 14 || !memchr(allowed, command, sizeof(allowed))) return ESP_ERR_INVALID_ARG;
    lvgl_port_lock(0);
    esp_err_t result = board_lcd_command(command, data, length);
    lvgl_port_unlock();
    ESP_LOGI("ui", "lcd command 0x%02x len=%u result=%s", command, (unsigned)length, esp_err_to_name(result));
    return result;
}

esp_err_t ui_set_brightness(int percent)
{
    lvgl_port_lock(0);
    esp_err_t result = board_set_brightness(percent);
    lvgl_port_unlock();
    return result;
}
