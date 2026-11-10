#pragma once

/*
 * Preset list. Only direct MP3 (audio/mpeg) streams are supported -- HLS
 * (.m3u8) and AAC stations cannot be played by the libhelix decoder.
 *
 * Add/remove entries freely, but the current UI only shows the first 8
 * (a 2x4 grid on the 480x320 landscape page). Keep display names short enough
 * to fit a 128 px wide button at 14 px.
 */
typedef struct {
    const char *name;
    const char *genre;
    const char *url;
} radio_station_t;

static const radio_station_t k_radio_stations[] = {
    {"中国之声",     "新闻", "https://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"上海动感101",  "音乐", "https://lhttp.qingting.fm/live/274/64k.mp3"},
    {"江苏经典流行", "音乐", "https://lhttp.qtfm.cn/live/4938/64k.mp3"},
    {"清晨音乐台",   "音乐", "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    {"第一财经",     "财经", "http://lhttp.qingting.fm/live/276/64k.mp3"},
    {"北京新闻广播", "新闻", "https://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"怀集音乐之声", "音乐", "https://lhttp.qingting.fm/live/4804/64k.mp3"},
    {"法国国际广播", "国际", "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3"},
};
