#ifndef PASSPORT_DISPLAY_H
#define PASSPORT_DISPLAY_H

#include "display/lcd_display.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// AI Passport "home face" display.
//
// While the device is idle, the upper half of the 240x320 panel shows a
// big digital clock with the date and current weather underneath; the
// emotion (scaled up 2x) sits in the lower half. In every other state
// the stock chat UI takes over — only the enlarged emotion placement is
// kept, so the face stays cute during conversations too.
class PassportDisplay : public SpiLcdDisplay {
public:
    PassportDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                    bool swap_xy);

    virtual void SetupUI() override;
    virtual void UpdateStatusBar(bool update_all = false) override;

private:
    void RefreshHomeFace();
    void MaybeFetchWeather();
    static void WeatherTaskEntry(void* arg);
    bool FetchWeatherOnce();
    static const char* WeatherText(int wmo_code);

    lv_obj_t* home_panel_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;

    int last_clock_min_ = -1;
    std::atomic<bool> weather_fetching_{false};
    std::atomic<int64_t> weather_next_fetch_us_{0};
    std::mutex weather_mutex_;
    std::string weather_text_;
};

#endif  // PASSPORT_DISPLAY_H
