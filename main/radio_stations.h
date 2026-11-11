#pragma once

/*
 * Preset list. Only direct MP3 (audio/mpeg) streams are supported -- HLS
 * (.m3u8) and AAC stations cannot be played by the libhelix decoder.
 *
 * The UI paginates 8 presets per page (2 x 4 grid) and flips pages with the
 * header pager, so the list length is not limited. Keep display names within
 * ~7 CJK characters so they fit the 128 px buttons, and rerun
 * tools/gen_radio_font.sh when a new name or genre introduces Chinese
 * characters that the font subset does not contain yet.
 *
 * All URLs below were verified to return HTTP 200 + audio/mpeg.
 */
typedef struct {
    const char *name;
    const char *genre;
    const char *url;
} radio_station_t;

static const radio_station_t k_radio_stations[] = {
    /* page 1 */
    {"中国之声",       "新闻", "https://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"上海动感101",    "音乐", "https://lhttp.qingting.fm/live/274/64k.mp3"},
    {"江苏经典流行",   "音乐", "https://lhttp.qtfm.cn/live/4938/64k.mp3"},
    {"清晨音乐台",     "音乐", "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    {"第一财经",       "财经", "http://lhttp.qingting.fm/live/276/64k.mp3"},
    {"北京新闻广播",   "新闻", "https://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"怀集音乐之声",   "音乐", "https://lhttp.qingting.fm/live/4804/64k.mp3"},
    {"法国国际广播",   "国际", "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3"},
    /* page 2 */
    {"国际新闻",       "新闻", "https://lhttp.qtfm.cn/live/20500172/64k.mp3"},
    {"上海新闻广播",   "新闻", "http://lhttp.qingting.fm/live/270/64k.mp3"},
    {"广东音乐之声",   "音乐", "https://lhttp.qtfm.cn/live/1260/64k.mp3"},
    {"广东珠江经济台", "财经", "https://lhttp.qtfm.cn/live/1259/64k.mp3"},
    {"安徽小说评书",   "评书", "https://lhttp.qtfm.cn/live/1951/64k.mp3"},
    {"故事广播",       "故事", "https://lhttp.qtfm.cn/live/20500182/64k.mp3"},
    {"上海经典94.7",   "音乐", "https://lhttp-hw.qtfm.cn/live/267/64k.mp3"},
    {"两广之声音乐台", "音乐", "https://lhttp.qtfm.cn/live/20500149/64k.mp3"},
    /* page 3 */
    {"AsiaFM经典台",   "音乐", "https://lhttp.qingting.fm/live/5021912/64k.mp3"},
    {"Fm891线上音乐台", "音乐", "https://lhttp.qtfm.cn/live/20500215/64k.mp3"},
    {"星空电台",       "音乐", "https://lhttp-hw.qtfm.cn/live/5022379/64k.mp3"},
    {"动听音乐台",     "音乐", "https://lhttp-hw.qtfm.cn/live/5022107/64k.mp3"},
    {"50首古典音乐",   "音乐", "https://lhttp-hw.qtfm.cn/live/5022308/64k.mp3"},
    {"听云相声台",     "相声", "https://m.tyradio.uk/tyxs.mp3"},
    {"听云评书台",     "评书", "https://m.tyradio.uk/typs.mp3"},
    {"REPLAY新闻",     "新闻", "https://replaynewszh.ice.infomaniak.ch/replaynewszh-128.mp3"},
};
