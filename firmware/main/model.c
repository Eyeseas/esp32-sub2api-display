#define _XOPEN_SOURCE 700
#include "display.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static cJSON *field(const cJSON *root, const char *name) { return cJSON_GetObjectItemCaseSensitive(root, name); }

static void string_field(const cJSON *root, const char *name, char *out, size_t size)
{
    cJSON *value = field(root, name);
    snprintf(out, size, "%s", cJSON_IsString(value) && value->valuestring ? value->valuestring : "--");
}

static time_t parse_time(cJSON *value)
{
    if (!cJSON_IsString(value) || !value->valuestring) return 0;
    struct tm tm = {0};
    char *end = strptime(value->valuestring, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) return 0;
    if (*end == '.') { end++; while (*end >= '0' && *end <= '9') end++; }
    int offset = 0, hours, minutes;
    if (*end == '+' || *end == '-') {
        if (sscanf(end + 1, "%2d:%2d", &hours, &minutes) != 2 || hours > 23 || minutes > 59) return 0;
        offset = (hours * 60 + minutes) * 60 * (*end == '+' ? 1 : -1);
    } else if (*end != 'Z') return 0;
    return mktime(&tm) - offset; // App TZ is UTC; displayed date/timezone comes from the bridge.
}

static int flag_severity(const char *flag)
{
    if (!strcmp(flag, "error") || !strcmp(flag, "paused") || !strcmp(flag, "expired")) return SEVERITY_CRIT;
    if (!strcmp(flag, "stale") || !strcmp(flag, "unknown")) return SEVERITY_UNKNOWN;
    return SEVERITY_WARN;
}

static const char *flag_label(const char *flag)
{
    if (!strcmp(flag, "error")) return "错误";
    if (!strcmp(flag, "paused")) return "暂停";
    if (!strcmp(flag, "expired")) return "到期";
    if (!strcmp(flag, "rate_limited")) return "限流";
    if (!strcmp(flag, "overloaded")) return "过载";
    if (!strcmp(flag, "temp_unschedulable")) return "冷却";
    if (!strcmp(flag, "local_quota_exceeded")) return "本地超限";
    if (!strcmp(flag, "model_rate_limited")) return "模型限流";
    if (!strcmp(flag, "quota_warning")) return "额度提醒";
    if (!strcmp(flag, "stale")) return "过期";
    return "未知";
}

esp_err_t model_parse(const char *body, display_model_t *model)
{
    cJSON *root = cJSON_ParseWithLengthOpts(body, strlen(body) + 1, NULL, true);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    cJSON *data = field(root, "data"), *summary = field(data, "summary"), *today = field(data, "today"),
          *day = field(data, "day"), *pagination = field(data, "pagination"), *accounts = field(data, "accounts");
    if (!cJSON_IsNumber(field(root, "code")) || field(root, "code")->valuedouble != 0 ||
        !cJSON_IsNumber(field(data, "schema_version")) || field(data, "schema_version")->valueint != 1 ||
        !cJSON_IsObject(summary) || !cJSON_IsObject(today) || !cJSON_IsObject(day) ||
        !cJSON_IsObject(pagination) || !cJSON_IsArray(accounts) || cJSON_GetArraySize(accounts) > DISPLAY_MAX_ACCOUNTS) goto done;
    memset(model, 0, sizeof(*model));
    model->partial = cJSON_IsTrue(field(data, "partial"));
    const char *summary_keys[] = {"accounts_total", "observed_normal", "alert_accounts"};
    int *targets[] = {&model->accounts_total, &model->observed_normal, &model->alert_accounts};
    for (int i = 0; i < 3; i++) {
        cJSON *value = field(summary, summary_keys[i]);
        if (!cJSON_IsNumber(value) || value->valueint < 0 || value->valuedouble != value->valueint) goto done;
        *targets[i] = value->valueint;
    }
    if (!cJSON_IsNumber(field(pagination, "page")) || field(pagination, "page")->valueint < 1) goto done;
    model->page = field(pagination, "page")->valueint;
    model->has_more = cJSON_IsTrue(field(pagination, "has_more"));
    model->generated_at = parse_time(field(data, "generated_at"));
    string_field(day, "date", model->day, sizeof(model->day));
    string_field(day, "timezone", model->timezone, sizeof(model->timezone));
    // day.start_at is local midnight of day.date, so their difference is the timezone's UTC offset.
    struct tm midnight = {0};
    time_t start = parse_time(field(day, "start_at"));
    if (start && strptime(model->day, "%Y-%m-%d", &midnight)) {
        long offset = (long)(mktime(&midnight) - start);
        if (offset >= -14 * 3600 && offset <= 14 * 3600 && offset % 900 == 0) { model->utc_offset = offset; model->has_utc_offset = true; }
    }
    string_field(today, "requests", model->requests, sizeof(model->requests));
    string_field(today, "tokens", model->tokens, sizeof(model->tokens));
    string_field(today, "account_cost_usd", model->account_cost, sizeof(model->account_cost));
    string_field(today, "user_cost_usd", model->user_cost, sizeof(model->user_cost));
    string_field(today, "standard_cost_usd", model->standard_cost, sizeof(model->standard_cost));
    cJSON *models = field(data, "models"), *items = field(models, "items");
    model->models_complete = cJSON_IsTrue(field(models, "complete")) && cJSON_IsArray(items);
    if (model->models_complete) {
        if (cJSON_GetArraySize(items) > DISPLAY_MAX_MODELS) goto done;
        model->model_count = cJSON_GetArraySize(items);
        for (int i = 0; i < model->model_count; i++) {
            cJSON *item = cJSON_GetArrayItem(items, i), *other = field(item, "other_models");
            model_usage_t *out = &model->models[i];
            out->other_models = cJSON_IsNumber(other) && other->valueint > 0 ? other->valueint : 0;
            if (!out->other_models) {
                string_field(item, "model", out->name, sizeof(out->name));
                // Model names are drawn in an ASCII-only font.
                for (unsigned char *c = (unsigned char *)out->name; *c; c++) if (*c >= 128 || *c < 32) *c = '?';
            }
            string_field(item, "requests", out->requests, sizeof(out->requests));
            string_field(item, "tokens", out->tokens, sizeof(out->tokens));
            string_field(item, "account_cost_usd", out->cost, sizeof(out->cost));
        }
    }
    model->account_count = cJSON_GetArraySize(accounts);
    for (int i = 0; i < model->account_count; i++) {
        cJSON *raw = cJSON_GetArrayItem(accounts, i), *runtime = field(raw, "runtime"), *windows = field(raw, "windows");
        display_account_t *account = &model->accounts[i];
        string_field(raw, "id", account->id, sizeof(account->id));
        if (!account->id[0] || strspn(account->id, "0123456789") != strlen(account->id)) goto done;
        string_field(raw, "alias", account->alias, sizeof(account->alias));
        for (unsigned char *c = (unsigned char *)account->alias; *c; c++) {
            if (*c >= 128 || *c < 32) {
                char id[sizeof(account->id)];
                memcpy(id, account->id, sizeof(id));
                snprintf(account->alias, sizeof(account->alias), "#%s", id);
                break;
            }
        }
        string_field(raw, "platform", account->platform, sizeof(account->platform));
        string_field(raw, "type", account->type, sizeof(account->type));
        cJSON *flags = field(raw, "flags"), *flag;
        bool reminder_only = true;
        cJSON_ArrayForEach(flag, flags) {
            if (!cJSON_IsString(flag)) continue;
            bool reminder = !strcmp(flag->valuestring, "quota_warning");
            int severity = flag_severity(flag->valuestring);
            if (!reminder && severity > account->block_severity) account->block_severity = severity;
            // Quota reminders yield to any other flag of the same severity.
            if (!account->flag[0] || severity > account->flag_severity || (severity == account->flag_severity && reminder_only && !reminder)) {
                snprintf(account->flag, sizeof(account->flag), "%s", flag_label(flag->valuestring));
                account->flag_severity = severity;
                reminder_only = reminder;
            }
        }
        cJSON *account_today = field(raw, "today");
        string_field(account_today, "requests", account->today_requests, sizeof(account->today_requests));
        string_field(account_today, "tokens", account->today_tokens, sizeof(account->today_tokens));
        string_field(account_today, "account_cost_usd", account->today_cost, sizeof(account->today_cost));
        account->concurrency = cJSON_IsNumber(field(runtime, "concurrency")) ? field(runtime, "concurrency")->valueint : -1;
        account->concurrency_limit = cJSON_IsNumber(field(runtime, "concurrency_limit")) ? field(runtime, "concurrency_limit")->valueint : -1;
        if (!cJSON_IsArray(windows) || cJSON_GetArraySize(windows) > DISPLAY_MAX_WINDOWS) goto done;
        account->window_count = cJSON_GetArraySize(windows);
        account->windows_total = cJSON_IsNumber(field(raw, "windows_total")) ? field(raw, "windows_total")->valueint : account->window_count;
        for (int j = 0; j < account->window_count; j++) {
            cJSON *w = cJSON_GetArrayItem(windows, j), *percent = field(w, "used_percent");
            quota_window_t *out = &account->windows[j];
            if (!cJSON_IsNumber(percent) || !isfinite(percent->valuedouble) || percent->valuedouble < 0) goto done;
            out->used_percent = percent->valuedouble;
            string_field(w, "label", out->label, sizeof(out->label));
            string_field(w, "quality", out->quality, sizeof(out->quality));
            string_field(w, "kind", out->kind, sizeof(out->kind));
            out->reset_at = parse_time(field(w, "reset_at"));
            out->sampled_at = parse_time(field(w, "source_updated_at"));
            out->stale_at = parse_time(field(w, "stale_at"));
            cJSON *stats = field(w, "stats");
            out->has_stats = cJSON_IsObject(stats);
            if (out->has_stats) {
                string_field(stats, "requests", out->stats_requests, sizeof(out->stats_requests));
                string_field(stats, "tokens", out->stats_tokens, sizeof(out->stats_tokens));
                string_field(stats, "account_cost_usd", out->stats_cost, sizeof(out->stats_cost));
            }
        }
        cJSON *balances = field(raw, "balances");
        if (cJSON_IsArray(balances) && cJSON_GetArraySize(balances)) {
            cJSON *balance = cJSON_GetArrayItem(balances, 0);
            char amount[24], currency[12];
            string_field(balance, "balance", amount, sizeof(amount));
            string_field(balance, "currency", currency, sizeof(currency));
            snprintf(account->balance, sizeof(account->balance), "%s %s", currency, amount);
        }
    }
    model->valid = true;
    result = ESP_OK;
done:
    cJSON_Delete(root);
    return result;
}

void model_demo(display_model_t *model)
{
    memset(model, 0, sizeof(*model));
    model->valid = model->demo = true;
    model->page = 1; model->accounts_total = 3; model->observed_normal = 2; model->alert_accounts = 2;
    strcpy(model->day, "演示数据"); strcpy(model->timezone, "Asia/Shanghai");
    model->has_utc_offset = true; model->utc_offset = 8 * 3600;
    strcpy(model->requests, "3702"); strcpy(model->tokens, "14074068");
    strcpy(model->account_cost, "37.02"); strcpy(model->user_cost, "55.53"); strcpy(model->standard_cost, "45.00");
    model->models_complete = true; model->model_count = 4;
    const char *model_names[] = {"gpt-6.1-sol", "claude-opus-5-5", "gpt-5.6-luna", ""},
               *model_requests[] = {"2240", "1430", "26", "6"}, *model_tokens[] = {"8102000", "5600468", "321000", "50600"},
               *model_costs[] = {"19.30", "17.40", "0.16", "0.16"};
    for (int i = 0; i < 4; i++) {
        model_usage_t *out = &model->models[i];
        strcpy(out->name, model_names[i]); out->other_models = i == 3 ? 2 : 0;
        strcpy(out->requests, model_requests[i]); strcpy(out->tokens, model_tokens[i]); strcpy(out->cost, model_costs[i]);
    }
    model->account_count = 3;
    const char *aliases[] = {"Claude-01", "Codex-02", "API-03"};
    const char *platforms[] = {"anthropic", "openai", "openai"};
    const char *labels[][2] = {{"5h", "7d"}, {"5h", "7d"}, {"daily", "weekly"}};
    const float percentages[][2] = {{85, 41}, {100, 62}, {17, 9}};
    const char *requests[] = {"1980", "1210", "512"}, *tokens[] = {"7801200", "4630100", "1642768"},
               *costs[] = {"20.11", "12.40", "4.51"};
    for (int i = 0; i < 3; i++) {
        display_account_t *account = &model->accounts[i];
        snprintf(account->id, sizeof(account->id), "%d", 101 + i);
        strcpy(account->alias, aliases[i]); strcpy(account->platform, platforms[i]);
        strcpy(account->type, i < 2 ? "oauth" : "apikey");
        if (i < 2) {
            strcpy(account->flag, i == 0 ? "额度提醒" : "限流");
            account->flag_severity = SEVERITY_WARN;
            account->block_severity = i == 1 ? SEVERITY_WARN : SEVERITY_OK;
        }
        account->concurrency = i + 1; account->concurrency_limit = 10;
        account->window_count = account->windows_total = 2;
        for (int j = 0; j < 2; j++) {
            account->windows[j].used_percent = percentages[i][j];
            strcpy(account->windows[j].label, labels[i][j]);
            strcpy(account->windows[j].quality, "observed");
        }
        quota_window_t *five_hour = &account->windows[0];
        five_hour->has_stats = true;
        strcpy(five_hour->stats_requests, requests[i]); strcpy(five_hour->stats_tokens, tokens[i]);
        strcpy(five_hour->stats_cost, costs[i]);
        strcpy(account->today_requests, requests[i]); strcpy(account->today_tokens, tokens[i]);
        strcpy(account->today_cost, costs[i]);
    }
}

// Copies at most size - 1 bytes without splitting a UTF-8 sequence.
static void utf8_copy(char *out, size_t size, const char *value)
{
    size_t length = strlen(value);
    if (length >= size) { length = size - 1; while (length && ((unsigned char)value[length] & 0xc0) == 0x80) length--; }
    memcpy(out, value, length);
    out[length] = 0;
}

esp_err_t quote_parse(const char *body, quote_t *quote)
{
    static const char *types[][2] = {{"a", "动画"}, {"b", "漫画"}, {"c", "游戏"}, {"d", "文学"}, {"e", "原创"}, {"f", "网络"},
                                     {"g", "其他"}, {"h", "影视"}, {"i", "诗词"}, {"j", "网易云"}, {"k", "哲学"}, {"l", "抖机灵"}};
    cJSON *root = cJSON_ParseWithLengthOpts(body, strlen(body) + 1, NULL, true);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *text = field(root, "hitokoto"), *from = field(root, "from"), *who = field(root, "from_who"), *type = field(root, "type");
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) >= sizeof(quote->text)) goto done;
    memset(quote, 0, sizeof(*quote));
    strcpy(quote->text, text->valuestring);
    for (char *c = quote->text; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    const char *work = cJSON_IsString(from) ? from->valuestring : "", *author = cJSON_IsString(who) ? who->valuestring : "";
    char source[sizeof(quote->source) * 2];
    if (author[0] && strcmp(author, work) && work[0]) snprintf(source, sizeof(source), "%s「%s」", author, work);
    else if (author[0] && strcmp(author, work)) snprintf(source, sizeof(source), "%s", author);
    else if (work[0]) snprintf(source, sizeof(source), "「%s」", work);
    else source[0] = 0;
    utf8_copy(quote->source, sizeof(quote->source), source);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (cJSON_IsString(type) && !strcmp(type->valuestring, types[i][0])) strcpy(quote->type, types[i][1]);
    result = ESP_OK;
done:
    cJSON_Delete(root);
    return result;
}
