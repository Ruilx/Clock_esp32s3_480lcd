#include "clock_ui.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "clock_time.hpp"
#include "lvgl.h"
#include "network.hpp"

extern "C" const lv_font_t lv_font_noto_sc_16;

namespace clock_ui {
namespace {
constexpr int center = 240;
constexpr double rad = 3.14159265358979323846 / 180.0;
struct Segment {
    lv_obj_t *line{};
    lv_point_precise_t points[2]{};
    lv_point_precise_t render_points[2]{};
};
std::array<Segment, 60> ticks;
std::array<Segment, 3> hands;
lv_obj_t *readout, *status, *hub;
lv_obj_t *date_readout;

lv_point_precise_t point(double angle, int radius)
{
    return {static_cast<int32_t>(center + std::lround(radius * std::sin(angle * rad))),
            static_cast<int32_t>(center - std::lround(radius * std::cos(angle * rad)))};
}

void line(Segment &segment, lv_obj_t *parent, int width, uint32_t color)
{
    segment.line = lv_line_create(parent);
    const int32_t min_x = std::min(segment.points[0].x, segment.points[1].x);
    const int32_t min_y = std::min(segment.points[0].y, segment.points[1].y);
    const int32_t max_x = std::max(segment.points[0].x, segment.points[1].x);
    const int32_t max_y = std::max(segment.points[0].y, segment.points[1].y);
    lv_obj_set_pos(segment.line, min_x, min_y);
    lv_obj_set_size(segment.line, std::max<int32_t>(1, max_x - min_x + 1),
                    std::max<int32_t>(1, max_y - min_y + 1));
    lv_obj_set_style_line_width(segment.line, width, 0);
    lv_obj_set_style_line_color(segment.line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(segment.line, true, 0);
    lv_obj_remove_flag(segment.line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(segment.line, LV_OBJ_FLAG_SCROLLABLE);
    segment.render_points[0] = {segment.points[0].x - min_x, segment.points[0].y - min_y};
    segment.render_points[1] = {segment.points[1].x - min_x, segment.points[1].y - min_y};
    lv_line_set_points(segment.line, segment.render_points, 2);
}

void hand(Segment &segment, double angle, int radius)
{
    const auto old_end = segment.points[1];
    segment.points[1] = point(angle, radius);
    const int32_t min_x = std::min({segment.points[0].x, old_end.x, segment.points[1].x});
    const int32_t min_y = std::min({segment.points[0].y, old_end.y, segment.points[1].y});
    const int32_t max_x = std::max({segment.points[0].x, old_end.x, segment.points[1].x});
    const int32_t max_y = std::max({segment.points[0].y, old_end.y, segment.points[1].y});
    lv_obj_set_pos(segment.line, min_x, min_y);
    lv_obj_set_size(segment.line, std::max<int32_t>(1, max_x - min_x + 1),
                    std::max<int32_t>(1, max_y - min_y + 1));
    segment.render_points[0] = {segment.points[0].x - min_x, segment.points[0].y - min_y};
    segment.render_points[1] = {segment.points[1].x - min_x, segment.points[1].y - min_y};
    lv_line_set_points(segment.line, segment.render_points, 2);
    lv_obj_invalidate(segment.line);
}

void visible(lv_obj_t *obj, bool show)
{
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *label(lv_obj_t *parent, int top, const lv_font_t *font)
{
    auto *obj = lv_label_create(parent);
    lv_obj_set_width(obj, 456);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe5edf5), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, top);
    return obj;
}
}

void init()
{
    auto *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x14191f), 0);
    for (size_t i = 0; i < ticks.size(); ++i) {
        auto &tick = ticks[i];
        const bool major = i % 5 == 0;
        tick.points[0] = point(6.0 * i, major ? 191 : 205);
        tick.points[1] = point(6.0 * i, 215);
        line(tick, screen, major ? 4 : 2, major ? 0xe5edf5 : 0x717d88);
    }
    for (auto &segment : hands) segment.points[0] = segment.points[1] = point(0, 0);
    line(hands[0], screen, 11, 0xf5f6f9);
    line(hands[1], screen, 7, 0xcbe8ff);
    line(hands[2], screen, 3, 0xff986b);

    hub = lv_obj_create(screen);
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, 14, 14);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0xff986b), 0);
    date_readout = label(screen, 78, &lv_font_noto_sc_16);
    readout = label(screen, 116, &lv_font_montserrat_32);
    status = label(screen, 325, &lv_font_noto_sc_16);
    refresh();
}

void refresh()
{
    using clock_time::Source;
    static time_t last_local = -1;
    static int last_year = -1;
    static int last_yday = -1;
    static bool last_valid = false;
    static bool first = true;
    const auto source = clock_time::source();
    const time_t local = std::time(nullptr) + static_cast<time_t>(network_utc_offset_minutes()) * 60;
    tm date{};
    const bool valid = source != Source::Invalid && gmtime_r(&local, &date) != nullptr;
    if (first || valid != last_valid) {
        for (auto &segment : hands) visible(segment.line, valid);
        visible(hub, valid);
        if (!valid) {
            lv_label_set_text(date_readout, "等待校时");
            lv_label_set_text(readout, "--:--:--");
        }
    }
    if (valid && (!last_valid || last_local != local)) {
        hand(hands[0], 30.0 * (date.tm_hour % 12) + 0.5 * date.tm_min + date.tm_sec / 120.0, 127);
        hand(hands[1], 6.0 * date.tm_min + 0.1 * date.tm_sec, 174);
        hand(hands[2], 6.0 * date.tm_sec, 192);
        char time_text[24];
        if (std::strftime(time_text, sizeof(time_text), "%H:%M:%S", &date)) {
            lv_label_set_text(readout, time_text);
        }
        if (first || last_valid != valid || date.tm_year != last_year || date.tm_yday != last_yday) {
            char date_text[24];
            if (std::strftime(date_text, sizeof(date_text), "%Y-%m-%d", &date)) {
                lv_label_set_text(date_readout, date_text);
            }
            last_year = date.tm_year;
            last_yday = date.tm_yday;
        }
    }
    first = false;
    last_valid = valid;
    last_local = local;

    const NetworkStatus net = network_status();
    char text[128];
    if (net.ap_active) {
        std::snprintf(text, sizeof(text), "Wi-Fi: %s\n密钥: %s\nhttp://192.168.4.1", net.ap_ssid, net.setup_key);
    } else if (net.sta_online) {
        std::snprintf(text, sizeof(text), "%s\nIP: %s", net.sta_ssid[0] ? net.sta_ssid : "Wi-Fi",
                       net.sta_ip[0] ? net.sta_ip : "getting...");
    } else {
        std::snprintf(text, sizeof(text), "Wi-Fi 离线");
    }
    if (std::strcmp(text, lv_label_get_text(status))) lv_label_set_text(status, text);
}
}
