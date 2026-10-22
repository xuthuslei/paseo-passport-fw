#include "passport_display.h"

#include "application.h"
#include "board.h"
#include "config.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <lvgl.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#define TAG "PassportDisplay"

// Emotion zoom relative to the bundled asset size (256 = 1x). The
// passport build ships the 64px emoji collection, so 2x renders ~128px.
#define PASSPORT_EMOJI_ZOOM 512
// Vertical center of the emotion zone on the 320px-tall panel.
#define PASSPORT_EMOJI_CENTER_Y 226

PassportDisplay::PassportDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void PassportDisplay::SetupUI() {
    LcdDisplay::SetupUI();

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    lv_obj_t* screen = lv_screen_active();

    // ---- Home panel: big clock + date + weather (upper half) ----
    home_panel_ = lv_obj_create(screen);
    lv_obj_set_size(home_panel_, width(), 124);
    lv_obj_align(home_panel_, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_opa(home_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_panel_, 0, 0);
    lv_obj_set_style_pad_all(home_panel_, 0, 0);
    lv_obj_set_scrollbar_mode(home_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(home_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(home_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(home_panel_, 2, 0);
    lv_obj_add_flag(home_panel_, LV_OBJ_FLAG_HIDDEN);  // shown only while idle

    clock_label_ = lv_label_create(home_panel_);
    lv_obj_set_width(clock_label_, width());
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(clock_label_, lvgl_theme->text_color(), 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(clock_label_, &lv_font_montserrat_48, 0);
#else
    lv_obj_set_style_text_font(clock_label_, text_font, 0);
#endif
    lv_label_set_text(clock_label_, "--:--");

    date_label_ = lv_label_create(home_panel_);
    lv_obj_set_width(date_label_, width());
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(date_label_, text_font, 0);
    lv_obj_set_style_text_color(date_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(date_label_, "");

    weather_label_ = lv_label_create(home_panel_);
    lv_obj_set_width(weather_label_, width());
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(weather_label_, text_font, 0);
    lv_obj_set_style_text_color(weather_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(weather_label_, "天气获取中");

    // ---- Emotion: bigger, in the lower half ----
    if (emoji_image_ != nullptr) {
        lv_image_set_scale(emoji_image_, PASSPORT_EMOJI_ZOOM);
        lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, PASSPORT_EMOJI_CENTER_Y - height() / 2);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_transform_pivot_x(emoji_label_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(emoji_label_, lv_pct(50), 0);
        lv_obj_set_style_transform_scale_x(emoji_label_, PASSPORT_EMOJI_ZOOM, 0);
        lv_obj_set_style_transform_scale_y(emoji_label_, PASSPORT_EMOJI_ZOOM, 0);
        lv_obj_align(emoji_label_, LV_ALIGN_CENTER, 0, PASSPORT_EMOJI_CENTER_Y - height() / 2);
    }

    // Keep chat bubbles out of the emotion zone.
    if (content_ != nullptr) {
        lv_obj_set_style_pad_bottom(content_, height() - PASSPORT_EMOJI_CENTER_Y + 40, 0);
    }
}

void PassportDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    RefreshHomeFace();
    MaybeFetchWeather();
}

void PassportDisplay::RefreshHomeFace() {
    if (home_panel_ == nullptr) {
        return;
    }
    bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;

    DisplayLockGuard lock(this);
    if (!idle) {
        if (!lv_obj_has_flag(home_panel_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(home_panel_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (lv_obj_has_flag(home_panel_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(home_panel_, LV_OBJ_FLAG_HIDDEN);
    }

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < (2025 - 1900)) {
        return;  // server time not synced yet; keep the "--:--" placeholder
    }
    int cur_min = tm_now.tm_hour * 60 + tm_now.tm_min;
    if (cur_min != last_clock_min_) {
        last_clock_min_ = cur_min;
        char buf[80];
        strftime(buf, sizeof(buf), "%H:%M", &tm_now);
        lv_label_set_text(clock_label_, buf);
        static const char* kWeekday[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(buf, sizeof(buf), "%d月%d日 星期%s", tm_now.tm_mon + 1, tm_now.tm_mday,
                 kWeekday[tm_now.tm_wday]);
        lv_label_set_text(date_label_, buf);
    }

    std::string weather;
    {
        std::lock_guard<std::mutex> guard(weather_mutex_);
        weather = weather_text_;
    }
    if (!weather.empty()) {
        lv_label_set_text(weather_label_, weather.c_str());
    }
}

// ---- Weather (open-meteo, no API key; refreshed every 30 minutes) ----

void PassportDisplay::MaybeFetchWeather() {
    if (weather_fetching_) {
        return;
    }
    if (esp_timer_get_time() < weather_next_fetch_us_) {
        return;
    }
    // Watchdog: if the task dies without rescheduling, allow a retry.
    weather_next_fetch_us_ = esp_timer_get_time() + 10LL * 60 * 1000 * 1000;
    weather_fetching_ = true;
    if (xTaskCreate(&PassportDisplay::WeatherTaskEntry, "weather", 10240, this, 5, nullptr) !=
        pdPASS) {
        weather_fetching_ = false;
    }
}

void PassportDisplay::WeatherTaskEntry(void* arg) {
    auto* self = static_cast<PassportDisplay*>(arg);
    bool ok = self->FetchWeatherOnce();
    int64_t delay_min = ok ? 30 : 5;
    self->weather_next_fetch_us_ = esp_timer_get_time() + delay_min * 60LL * 1000 * 1000;
    self->weather_fetching_ = false;
    vTaskDelete(nullptr);
}

bool PassportDisplay::FetchWeatherOnce() {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    char url[200];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code",
             PASSPORT_WEATHER_LAT, PASSPORT_WEATHER_LON);
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "weather request failed to open");
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGW(TAG, "weather request status failed");
        http->Close();
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* temp = current ? cJSON_GetObjectItem(current, "temperature_2m") : nullptr;
    cJSON* code = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
    bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(code);
    if (ok) {
        const char* text = WeatherText(code->valueint);
        char buf[80];
        if (text != nullptr && text[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s %s %d℃", PASSPORT_WEATHER_CITY, text,
                     (int)lround(temp->valuedouble));
        } else {
            snprintf(buf, sizeof(buf), "%s %d℃", PASSPORT_WEATHER_CITY,
                     (int)lround(temp->valuedouble));
        }
        std::lock_guard<std::mutex> guard(weather_mutex_);
        weather_text_ = buf;
        ESP_LOGI(TAG, "weather updated: %s", buf);
    }
    cJSON_Delete(root);
    return ok;
}

const char* PassportDisplay::WeatherText(int wmo_code) {
    switch (wmo_code) {
        case 0:
            return "晴";
        case 1:
        case 2:
            return "多云";
        case 3:
            return "阴";
        case 45:
        case 48:
            return "雾";
        case 51:
        case 53:
        case 55:
            return "毛毛雨";
        case 56:
        case 57:
            return "冻雨";
        case 61:
            return "小雨";
        case 63:
            return "中雨";
        case 65:
            return "大雨";
        case 66:
        case 67:
            return "冻雨";
        case 71:
            return "小雪";
        case 73:
            return "中雪";
        case 75:
            return "大雪";
        case 77:
            return "雪粒";
        case 80:
        case 81:
            return "阵雨";
        case 82:
            return "暴雨";
        case 85:
        case 86:
            return "阵雪";
        case 95:
            return "雷阵雨";
        case 96:
        case 99:
            return "冰雹";
        default:
            return "";
    }
}
