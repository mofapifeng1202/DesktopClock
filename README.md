# lvgl_st77922_demo

ESP32-S3 (N16R8) + LCDWiki ES3C35P 3.5" 320x480 IPS（ST77922 QSPI + ST7123 触摸）桌面时钟 / 天气站。
ESP-IDF 5.4.1 + LVGL 9 + esp_lvgl_port。

## 硬件

![产品参数](docs/spec.jpg)

| 部件 | 规格 |
|---|---|
| 模组 | ESP32-S3R8（8MB PSRAM + 16MB Flash，Wi-Fi 2.4G + BLE5） |
| 屏幕 | 3.5" IPS 320×480，ST77922，QSPI |
| 触摸 | ST7123 电容触摸，I²C（5 点） |
| 音频 | ES8311 codec + SC8002B 功放 + 1.25mm 喇叭座 |
| 供电 | USB-C 5V；TP4054 充电管理（4.2V 恒压）+ SL2305 电源切换 |
| 扩展 | microSD（4D SDIO）、I²C/UART/扩展 IO 座、WS2812 指示灯 |

引脚映射、充电电路、eFuse 摘要、横屏与触摸的实现陷阱见 **[docs/hardware.md](docs/hardware.md)**。

> 注：横屏是**软件旋转**实现的（本模块开启 MADCTL.MV 硬件旋转会花屏），
> 触摸噪声较大，固件里做了限幅滤波，详见上面的硬件文档。

## 功能

- 时钟页：48 号数字时钟、秒、星期日期、NTP 对时状态、音量滑块
- 天气页：当前温度/天气/湿度/风速 + 3 天预报，城市和数据源**可在网页里换**（内置 38 个城市预设），
  30 分钟刷新，换设置后立即重取
- 收音机页：24 个 MP3 网络电台（每页 8 个、头部 < > 翻页），播放/暂停、上一台/下一台、实时频谱与缓冲显示
- 电视页：接入内网 IPTV（m3u 运行时解析，86 个频道），只出声音不出画面（ESP32 无法解 HEVC）
- 设置页（第 5 页）：音量滑块、亮度滑块、自动熄屏时间、手机配网、重启、恢复出厂、Wi-Fi/网页地址
- **网页配置**：浏览器打开设备 IP 即可配置 Wi-Fi / 音量 / 亮度 / 熄屏 / 频道表地址 / 背景图 / 重启 / 出厂；
  没配网时自动开热点 `DesktopClock-Setup`，手机连上去打开 `http://192.168.4.1` 配网
- 顶部状态栏：电池电压 + 百分比（ADC1_CH7，1:2 分压）、Wi-Fi 信号、对时状态
- 左右滑动切换五页；BOOT 键切背光（点屏即可唤醒）；RGB LED 指示 Wi-Fi 状态
- 背光改成 LEDC PWM（GPIO41，5 kHz/10 bit），亮度 5%~100% 可调；默认 **5 分钟无操作自动熄屏**，
  熄屏只是背光关掉，收音机/电视的**声音继续播**
- ES8311 音频（I2S），开机提示音 + 全局音量控制（时钟页/收音机页/电视页/设置页滑块同步）

引脚映射见 `main/main.c` 头部注释。

## 编译与烧写

本机已装好 ESP-IDF **5.4.1**（`~/esp/esp-idf`）与 esp32s3 工具链，直接用脚本：

```bash
./tools/build.sh        # 编译
./tools/flash.sh        # 烧写（自动找 /dev/cu.usbmodem*）
```

或手动：

```bash
. ~/esp/esp-idf/export.sh
cd lvgl_st77922_demo
idf.py build
idf.py -p /dev/cu.usbmodem14101 flash monitor
```

首次编译前可以先填好 Wi-Fi（也可以跳过，开机后用手机连 `DesktopClock-Setup` 热点，
打开 `http://192.168.4.1` 配网）：

```bash
cp main/wifi_app_secret.h.example main/wifi_app_secret.h
# 然后填入你的 2.4GHz Wi-Fi 名称/密码（该文件不入库）
```

频道表地址同理：`cp main/tv_config.h.example main/tv_config.h` 后填入内网 IPTV 中继地址，
或开机后在网页里改（网页里改的值存在 NVS，优先于编译期默认值）。

## 网络收音机

第三页是网络收音机：HTTP(S) 拉取 MP3 流 → libhelix 软件解码 → ES8311/I2S 出声。

- 24 个预置台在 `main/radio_stations.h`。只支持直接返回 `audio/mpeg` 的 **MP3 流**；
  HLS（`.m3u8`）和 AAC 不能播放。UI 每页 8 个、用标题栏的 `<` / `>` 翻页，
  列表长度不限（每页 8 个）。改表后重新编译；新台名/新分类出现新汉字时，
  跑一次 `tools/gen_radio_font.sh` 重新生成收音机字体（脚本会扫描
  `radio.c` / `radio_ui.c` / `radio_stations.h` / `tv_ui.c` / `media_list.c` /
  `settings_ui.c` / `web_config.c`，改这几处的界面文案同样要跑一次）。
- 播放/暂停、上一台/下一台、缓冲百分比、输出电平；音量和时钟页滑块双向同步。
- 频谱：播放任务对每帧解码出的 PCM 做 256 点 FFT（约 0.3 ms/帧，PSRAM 存表），
  映射成 16 个对数频段；UI 用独立的 ~30fps 定时器采样自绘（与 200ms 状态刷新解耦，
  否则会像掉帧），回落速度按时间计算（约 110%/s）。换台/断流时自动回落，
  低码率流（如 22050 Hz）同样支持。灵敏度可调 spectrum.c 里的 BAND_SCALE / BAND_HF_TILT。
- 引擎分两个任务：`radio_net` 负责 HTTP/HTTPS 和 128 KB PSRAM 环形缓冲，
  `radio_play` 负责整帧解析/MP3 解码/I2S 写入。网络抖动不会直接卡住音频。
- 首次连接预缓冲 24 KB（64 kbps 约 3 秒），稳定后缓冲约 40%~50%。
- HTTPS 的 mbedTLS 工作缓冲放到 PSRAM（`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`），
  同时关闭未使用的蓝牙（`CONFIG_BT_ENABLED=n`）腾出内部 RAM；两者都已写进
  `sdkconfig.defaults`。
- 调试：`main/main.c` 顶部 `RADIO_SELFTEST` 置 1，会在连上 Wi-Fi 后自动依次
  播放/切台/暂停/恢复/停止，串口日志打印 `frames / underruns / buf / level`。

已知限制：

- 只支持 MP3 Layer III，不支持 HLS/AAC；电台源可能失效或有地区限制。
- 没有解析 ICY `StreamTitle`，界面显示电台名，不显示当前曲目。
- 换台要改 `radio_stations.h` 重新编译，暂未做网页配置/自定义 URL 输入。
- 采样率切换时可能打印两条 `i2s_channel_disable ... has not been enabled yet`，
  这是 `esp_codec_dev` 重配 I2S 的良性日志，不影响播放。

## 网络电视（IPTV，音频）

tileview 第 4 页是 IPTV：开机后自动从频道表地址拉取 m3u 并解析，按 央视/地方卫视/其他
分组排序。地址存在 NVS 里，**直接网页改**（设置页 →“电视频道表”）；首次开机的默认值来自
`main/tv_config.h`（复制 `tv_config.h.example` 后填入你的内网 IPTV 中继地址，该文件已 gitignore，
仓库里只有占位地址）。

- 交互：点中间大面板 → 左侧滑出频道抽屉（可滚动，按 **央视 → 地方卫视 → 其他**
  分组排序，组内按名称），点频道即换台并收起；底部有播放/暂停和上一台/下一台，
  音量与时钟页同步。
- 抽屉是虚拟滚动（只建 8 行控件循环复用），并且针对这块屏的触摸噪声做了三处保护：
  打开后 700 ms 内忽略点击（否则鬼点会立刻把它关掉，表现为"一闪而过"）；
  切换动画启动前先 `lv_anim_delete`、关闭动画结束时再确认一次状态（避免重新打开被
  上一次的关闭动画重新隐藏，表现为"点不开、要点好几次"）；中间的卡片除了 CLICKED，
  还会在 `PRESS_LOST` 时检查按下到抬起的手势位移（≤45 px 视为点击），
  这样被噪声顶成滚动的"假滑动"也能正常打开抽屉。
- **只能听，不能看**：IPTV 视频是 HEVC 编码，ESP32-S3 没有硬解，软解不可能。
  页面显示频道名/频谱/缓冲，扬声器出电视声音。
- 管线：HLS 播放列表刷新 + 分段下载 → 自研 TS 解复用（`ts_demux.c`，约 150 行：
  PAT → PMT → 只挑音频流 → PES 重组；esp_audio_codec 自带的 TS 解析器对
  HEVC-first 的 PMT 选流会出错，所以自己写了）→ esp_audio_codec AAC 解码 →
  现有输出管线（频谱/音量/ES8311 全部复用）。
- 频道表地址在网页里改（存 NVS），编译期默认值见 `main/tv_config.h`。
  m3u 是运行时解析的，频道名并不写死在代码里：`tools/gen_radio_font.sh` 生成字体时
  会抓一次当前频道表，把用到的汉字都打进 `font_radio_14/20` 的子集（快照缓存在
  `tools/tv_chars.txt`，断网也能重新生成）；另外还有 `font_zh_14`（GB2312 一级
  3755 个常用汉字，约 +370 KB flash）通过 `.fallback` 挂在 `font_radio_14/20` 后面，
  所以以后网页里换一份新频道表也不会再出现"口口"空白字。
- IPTV 预缓冲 100 KB（约 0.5 s）再开播，跨 HLS 分段的间隙不会断音；
  AAC 解码占单核 16~19%，实测连续播放无报错。flash 余量约 45%。
- `udp://@` 组播源暂不支持（列表里会保留，点播时提示）——需要 udpxy 转 http。
- `udp://@` 之外的所有条目按 TS over HTTP 处理；fMP4（`#EXT-X-MAP`）的
  HLS 会提示暂不支持。

## 天气数据源

天气的城市和数据源都存在 NVS 里，网页"天气"一栏改，保存后马上重新拉取（不用重启）。

| 数据源 | 地址 | 说明 |
|---|---|---|
| `WX_SRC_CN`（默认） | `t.weather.itboy.net/api/weather/city/<城市编码>` | 中国天气网镜像，**纯 HTTP、无需 key、国内直连**，直接给中文天气（霾/阴/小雨），有 3 天预报 |
| `WX_SRC_OPENMETEO` | `api.open-meteo.com`（经纬度） | 免 key、数据规整，但**国内网络经常连不上**（实测本机 15 秒超时），适合挂梯子或海外 |
| `WX_SRC_WTTR` | `wttr.in/<lat>,<lon>?format=...` | 免 key，给的是英文天气，固件里映射成中文；没有 3 天预报 |

- 网页里选一个城市（北京/上海/重庆/广州… 共 38 个）会自动填好城市编码和经纬度；
  也可以手填"城市编码"或"经纬度 + 显示名称"。
- 城市编码是 [中国天气网的城市代码](http://www.weather.com.cn/)（例如重庆 `101040100`）；
  其他城市在天气网上搜到对应城市后，地址栏里就是那个 9 位数字。
- 换源/换城市后 `weather_refresh()` 会让后台任务立刻重试；取不到数据时 1 分钟后重试，
  成功则 30 分钟一次。串口每次失败都会打印 `weather: fetch failed ... url=...`。
- 时钟/天气页的城市名、天气描述都是运行时来的，靠两级兜底字体（一级+二级 GB2312）保证
  不会出现占位方框；字符串按 UTF-8 边界截断，不会切出半个汉字。

## 系统设置与网页配置

设置项统一存在 NVS（namespace `cfg`，一个 blob，滑块松手/网页 POST 时才写 flash），
编译期默认值来自 `main/wifi_app_secret.h` 和 `main/tv_config.h`。

**设备上有两个入口**：

1. 第 5 页“设置”：音量 / 亮度两个滑块、自动熄屏（点一下循环 0/1/5/10/30 分钟）、
   手机配网、重启、恢复出厂，底部显示 Wi-Fi、IP 和网页地址。
2. 网页 `http://<设备IP>/`（连上 Wi-Fi 后一直可用）：除上面这些，还能扫描附近 Wi-Fi、
   改 Wi-Fi 账号密码、改频道表地址、看内存/运行时间。

**没配网 / 想换网络时**：开机没有可用凭据（或从设置页点“手机配网”、网页点“进入配网热点模式”）
会开一个开放热点 **`DesktopClock-Setup`**，手机连上去打开 `http://192.168.4.1` 就能配网，
配网模式 **5 分钟无操作自动重启**，回到正常 STA 模式。

**背景图**：网页"背景图"一栏选一张 JPEG/PNG，浏览器会就地缩放裁剪到 480×320 并转成
RGB565 原始位图再上传（设备端不需要图片解码器），固件把它写进 `wallpaper` 分区
（`partitions.csv` 里 0x310000、384 KB），重启后自动加载。点一下按钮就能换，也可以
"恢复纯色背景"。代价：整屏重绘要多从 PSRAM 读 307 KB 位图，一帧约 +10 ms
（`/api/uibench` 实测 40 ms → 50 ms），只影响翻页这类整屏刷新，时钟跳秒这种局部刷新不变。

接口（都是 JSON）：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 设置页面（单文件内嵌 HTML） |
| GET | `/api/status` | 连接状态、IP/RSSI、堆内存、运行时间、当前音量/亮度/熄屏/频道表地址 |
| GET | `/api/scan` | 扫描附近热点（最多 30 个） |
| POST | `/api/wifi` | `{ssid,pass}` 保存后重启 |
| POST | `/api/settings` | `{volume,brightness,sleep,tvurl}` 立即生效并落盘 |
| POST | `/api/restart` | 保存并重启 |
| POST | `/api/factory` | 清空 NVS 设置并重启 |
| POST | `/api/apmode` | 切到配网热点模式 |
| GET | `/api/bg` | `{present}` 是否存了背景图 |
| POST | `/api/bg` | 请求体是 480×320×2 字节的原始 RGB565；`{"clear":1}` 则恢复纯色 |

背光：GPIO41 由 LEDC PWM 驱动（5 kHz、10 bit），熄屏 = duty 0，系统其余部分照常运行；
触摸任意位置会唤醒（并吞掉这次点击，避免误触控件）。

## 显示与流畅度（实测数据）

屏幕是 QSPI 面板、原生竖屏，横屏靠**软件旋转**实现，所以每一帧都要多做一遍
"旋转 + RGB565 字节交换"。整屏 480×320 刷一次 = 153,600 像素。

用 `/api/uibench`（强制整屏重绘一次并计时）实测，同一块板子：

| 缓冲模式 | 40 MHz | 80 MHz | 内部 RAM（空闲/最低） |
|---|---|---|---|
| **2 × 16 行双缓冲**（最保守） | 46 ms | 42 ms | 50 KB / 30 KB |
| 1 × 24 行单缓冲 | 47 ms | 41 ms | 46 KB / 23 KB |
| 1 × 28 行单缓冲 | 48 ms | **40 ms** | 38 KB / 19 KB |
| 1 × 32 行单缓冲 | 47 ms | 36 ms | 32 KB / 9 KB |

结论：这里的时间主要不是被 QSPI 带宽卡住，而是**每次刷新的固定开销**——
条带越矮，LVGL 每画一条都要重新遍历一遍对象树、面板也要多设几次窗口。
所以"少而高"的条带比"多而矮"的双缓冲更快；把时钟提到 80 MHz 还能再省 ~7 ms
（但 80 MHz 超出模组标称的 40 MHz，**屏幕出现花点/错位就调低**）。

两个旋钮都在网页"屏幕"一栏（改完自动重启生效，存在 NVS 里）：

- **缓冲模式**：`2×16 双缓冲` / `1×24` / `1×28` / `1×32` 单缓冲（默认 `1×28`）
- **总线时钟**：20 / 40 / 60 / 80 MHz（默认 40 MHz，即模组标称值）

单帧的时间构成（1×28 单缓冲、40 MHz）：

| 项目 | 耗时 |
|---|---|
| LVGL 软件渲染 | ~24 ms |
| 软件旋转 + 字节交换 + 提交 DMA | ~9 ms |
| 等 QSPI 把 307 KB 送完（`LCD_QSPI_PCLK_HZ`，默认 80 MHz） | ~8 ms（与上面部分重叠） |
| 只画背景、没有任何控件的整屏重绘 | 24 ms |

诊断接口（都返回 JSON）：

```bash
curl http://<ip>/api/status      # uiframes/uiframeus/uiflushus/uikpx：近 1 秒的帧统计
curl http://<ip>/api/uibench     # 强制整屏重绘一次，返回 ms/areas/flushms
curl "http://<ip>/api/uibench?plain=1"    # 隐藏页面内容，只画背景（管线地板）
curl "http://<ip>/api/uibench?rot=0"      # 临时关掉软件旋转再测（对比旋转开销）
curl "http://<ip>/api/uibench?drawer=1"   # 开一次频道抽屉，返回它占了多少 KB 堆
```

串口每 10 秒打印一行 `ui: N fps-ish, frame X us, flush Y us xZ, N kpx/s`。

其它跟流畅度/内存有关的细节：

- **频道抽屉是虚拟滚动**：只建 8 行控件并复用，滚到哪画哪。原来 86 个频道要建 86 个
  按钮 + 86 个标签 ≈ 25 KB 内部 RAM，现在 ~0.6 KB（86 → 87 个频道也不会涨）。
- 抽屉的遮罩层是不透明的：半透明遮罩会让 LVGL 为整个区域申请 24 KB 的临时图层缓冲。
- 页背景都是透明的，只有屏幕本身画一次底色，避免每页重复填一遍同色矩形。

## 环境搭建踩坑记录（macOS x86_64 + 系统 Python 3.9）

国内网络下 GitHub 基本不可用，以下组合可以完整装好：

1. **ESP-IDF 源码**：`git clone https://gitee.com/EspressifSystems/esp-idf.git -b v5.4.1 --depth 1`。
   注意 gitee 镜像会把 `.gitmodules` 改写成 gitee 地址，而 gitee 上并没有这些子仓库，需要把子模块
   URL 改到 Espressif 的国内镜像组织 `https://jihulab.com/esp-mirror/<org>/<repo>.git`。
   子模块必须 checkout 到 v5.4.1 **钉住的 SHA**（`git ls-tree HEAD <path>` 取），不能用镜像分支 tip
   （二进制库版本漂移会导致构建异常）。
2. **工具链**：`export IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets` 后执行 `./install.sh esp32s3`
   （默认会走 github.com 下载，慢到不可用）。
3. **Python 依赖**：`export PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple`，
   并且 `pip install "cryptography<49"`（新版没有 cp39 wheel，源码构建需要 Rust）。
4. **Python 3.9 的 importlib.metadata 不做名称归一化**：`ruamel.yaml` 0.19+ / `ruamel.yaml.clib`
   的 dist-info 目录名是下划线形式，导致 IDF 的 `check-python-dependencies` 找不到包并刷出一堆
   假的 "requirements are not satisfied"。解决：装 `ruamel.yaml==0.18.6`，并给下划线目录补一个
   带点的别名，例如
   `ln -s ruamel_yaml_clib-0.2.15.dist-info ruamel.yaml.clib-0.2.15.dist-info`。
5. **cmake 必须是 3.x**：`pip install "cmake<4"`（cmake 4 与 IDF 5.4 的 CMake 脚本不兼容）。
6. 若 `setuptools` 装到 81+ 会移除 `pkg_resources`，IDF 需求检查器同样会崩，需 `setuptools<81`。

## 触屏滤波（重要）

这块屏的电容触摸噪声很大：实测**约 40 次/分钟的几百像素鬼点跳变**（ST7123 驱动不写任何阈值配置，
只读寄存器）。`main/main.c` 的 `tp_indev_read_cb()` 做了三层处理：

1. 5 个上报点里取接触面积（strength）最大的那个，避免鬼点抢先
2. **限幅平移**：位移超过 **3000 px/s × 实际间隔**（下限 30px、上限 240px）才视为鬼点，
   按上限沿原方向推进。不用"冻结/丢弃"，那会把滑动速度和惯性抹平，导致切页困难。
   限幅改成按时间计算，是因为 LVGL 任务偶尔会被收音机/TLS 延迟；固定 60px/poll
   在任务被延迟时会把正常快滑变成慢拖
3. 手指抬起立即上报 RELEASED，并在下次触摸时重置参考点

另外：

- 触摸轮询周期从默认 33ms 改为 **20ms（50Hz）**，拖动和点击更跟手
- **LVGL 任务优先级 6**，收音机播放 5、网络 4：TLS 握手或解码突发不会再把 UI/触摸饿住
- `TP_PROBE` / `TP_DEBUG_COORDS` 默认已置 0；排查触摸时再打开

运行时会限频打印 `touch ghost clipped (N total, max_step=...): x,y`，可用于长期观察噪声水平。

## 屏幕/触摸问题排查

固件现在会主动留下几类线索，先看串口（`tools/flash.sh` 后接 `idf.py monitor`，或用
`tools/serial_capture.py`）：

- **开机第一行** `reset reason: ...`：如果上次是 `brownout` / `power-glitch`，是供电问题；
  `panic` / `task-wdt` / `cpu-lockup` 是软件崩溃；`usb` / `power-on` 是正常刷机/上电。
- **每 10 秒** `UI alive: lvgl used ... / frag ... ; int heap ...`：LVGL 池 64KB，正常约用
  38%、碎片 2~4%；如果 `used` 接近 100% 或 `int heap` 很低，才是内存问题。
- **`UI STALL ... ms`**：LVGL 任务超过 5 秒没心跳。此时如果 `radio: frames=...` 还在打印，
  说明芯片没死，是 LVGL 任务被卡住（最可能是显示 flush 等 DMA 完成，或抢锁）；
  如果日志也完全停了，才是整机死机/重启。

常见原因和处理：

1. **供电**：屏幕背光 + 喇叭 + 充电同时工作可到 ~400mA，弱 USB 口/长线会让 5V 掉压，
   表现为屏幕花/卡/触摸乱跳，严重时 brownout 重启。换 5V/2A 充电头 + 短线，或拔掉电池
   只用 USB、降低音量测试。
2. **触摸不灵敏**：先确认没有厚保护膜、手指不太干；软件上可调
   `TP_MAX_SPEED_PX_S`（默认 3000）和 `TP_MIN_STEP`（默认 30）。如果日志里
   `touch ghost clipped` 很频繁，说明环境噪声大，不要盲目提高灵敏度。
3. **真死机**：把 `reset reason` 和最后 30 秒日志发出来。若看到 `UI STALL`，下一步是给
   `esp_lvgl_port` 的 `trans_sem` 加超时（当前是 `portMAX_DELAY`），避免 DMA 回调丢失后
   永久卡住。

## 横屏 480x320 的做法（含三个坑）

面板原生是 320x480 竖屏，横屏通过 **LVGL 软件旋转**实现，面板 MADCTL 保持原生竖屏：

```c
#define LCD_PANEL_H_RES 320        /* 面板原生 */
#define LCD_PANEL_V_RES 480
#define LCD_H_RES       480        /* 逻辑（旋转后） */
#define LCD_V_RES       320
#define LANDSCAPE_ROTATION  LV_DISPLAY_ROTATION_90

disp_cfg.hres = LCD_PANEL_H_RES;  disp_cfg.vres = LCD_PANEL_V_RES;
disp_cfg.flags.sw_rotate = true;
lv_display_set_rotation(disp, LANDSCAPE_ROTATION);
```

**坑 1：不能用 MADCTL.MV 硬件旋转。** 打开 `st77922_vendor_config_t.flags.supports_madctl_mv`
再调 `esp_lcd_panel_swap_xy()` 后，本模块画面会整体错乱（这正是 IDf 驱动把它做成"仅验证过的模组
才可开启"的原因）。只能用软件旋转。

**坑 2：`rounder_cb` 必须跟着转。** ST77922 要求 CASET（面板 X）4 像素对齐，而软件旋转 90° 之后
**面板 X 轴 = 逻辑 Y 轴**。如果还按竖屏去对齐 `area->x1/x2`，每个 16 行刷新块都会错位 →
表现为"元素花屏/错乱"。正确做法：

```c
#if (LANDSCAPE_ROTATION == LV_DISPLAY_ROTATION_90) || (LANDSCAPE_ROTATION == LV_DISPLAY_ROTATION_270)
    area->y1 = (area->y1 >> 2) << 2;
    area->y2 = ((area->y2 >> 2) << 2) + 3;
#endif
```

**坑 3：不要在触摸回调里自己转坐标。** LVGL 9 会对已注册 display 的 indev 自动做旋转换算
（`lv_indev.c` → `lv_display_rotate_point()`：`X = ver_res-1-y, Y = x`），它期望驱动上报
**面板原始坐标**（x:0..319, y:0..479）。如果驱动层再 swap/mirror 一次，就是"双重旋转"，症状是：
- 音量滑块只能拖到中间就拖不动（X 只覆盖 320/480）
- 竖直滑动反而切页（竖直轴被映射到了 LVGL 的 X）
- 屏幕只有一部分区域能点

所以 `TP_LAND_SWAP/MIRROR_*` 全部保持 0，让 LVGL 去转。

**校准方法（可复用）**：`TP_DEBUG_COORDS` 打开后，`boot_btn_task` 每秒心跳会把最近一次按下的
`raw=x,y`（面板原始）和 `mapped=x,y` 打出来，点四个角就能反推出映射关系。注意**不要**在其他任务里
主动调用 `esp_lcd_touch_read_data()` 做探针——那会把 LVGL 需要的触摸点提前消费掉。

## 已知问题 / 待办

- 中文字体是子集：`font_cn_14/16.c`（时钟/天气页）和 `font_radio_14/20.c`
  （收音机/电视/设置页）。改这些页面的文案后跑一次 `tools/gen_radio_font.sh`；
  电视页/设置页里子集之外的汉字（比如新频道名）由 `font_zh_14` 兜底，不会空白，
  但只有 GB2312 一级 3755 字 + 生成时的频道名，二级生僻字仍可能缺
- 蓝牙本期未做：`CONFIG_BT_ENABLED=n` 腾出内部 RAM；以后要用 BLE 时在 sdkconfig 重新打开
- 编译优化已从 Debug 改为 `-O2`（`CONFIG_COMPILER_OPTIMIZATION_PERF=y`），
  软件旋转 + MP3 解码需要这部分 CPU 余量
- 16MB flash 只用 3MB app 分区，其余空闲（未挂 SPIFFS/LittleFS）；收音机缓冲放在 PSRAM
- `TP_PROBE` / `TP_DEBUG_COORDS` 是触摸调试开关，稳定后可置 0 减少日志噪声
- HTTPS 电台依赖 NTP 时间同步；刚开机未对时完成时，证书校验可能失败

## 字体与许可

`main/font_cn_14.c` / `main/font_cn_16.c` 和 `main/font_radio_14.c` /
`main/font_radio_20.c` 都是用 `lv_font_conv` 从 **Noto Sans CJK SC / Noto Sans SC**
生成的子集（只含界面用到的汉字 + ASCII 0x20-0x7E），源字体采用
**SIL Open Font License 1.1**，许可全文见 `LICENSES/OFL-NotoSansCJK.txt`。

兜底字体链（`lv_font.c` 会逐字回退，链可以有多层）：

```
font_cn_16 / font_cn_14 ──┐
font_radio_20 / radio_14 ─┴─► font_zh_14 (GB2312 一级 3755 字, 14px, 4bpp, ~370 KB)
                                   └─► font_zh2_14 (GB2312 二级 3008 字, 14px, 4bpp, ~300 KB)
```

这样界面上出现的任何 GB2312 汉字都有字形。之前天气页"霾"显示成一个方框，
就是因为它是**二级字**，既不在 `font_cn_16` 的子集里、也不在只含一级字的 `font_zh_14` 里，
而 LVGL 的 `LV_USE_FONT_PLACEHOLDER` 会给缺字画一个占位方框（看着像乱码）。
二级字那 300 KB 平时用不到，但天气/城市名是运行时来的，删了迟早再踩。
它保证界面上出现任何"没进子集"的汉字时不会画成空白方块——IPTV 频道名是运行时
从 m3u 拿的，光靠子集永远追不上。

收音机/电视/设置页子集 + 兜底字体一起重新生成（需要 Node.js + lv_font_conv + fonttools）：

```bash
FONT=managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf \
  ./tools/gen_radio_font.sh
```

脚本做的事：扫描 `radio.c`/`radio_ui.c`/`radio_stations.h`/`tv_ui.c`/`media_list.c`/
`settings_ui.c`/`web_config.c` 里的汉字，抓一次 IPTV 频道表的频道名（URL 取自
`main/tv_config.h`，可用 `TV_URL=...` 覆盖），生成 `font_radio_14/20` 和
`font_zh_14`，并把 `.fallback` 链好。

时钟/天气页字体重新生成（需要 Node.js + lv_font_conv + Noto Sans CJK SC 的 OTF）：

```bash
npx lv_font_conv --font NotoSansCJKsc-Regular.otf --size 16 --bpp 4 \
  --format lvgl --no-compress --no-kerning --range 0x20-0x7e \
  --symbols 重庆晴间多云阴雾毛毛雨雪阵雷未知电量连接中失败关闭对时已同步音左右滑动查看天气湿风速更新加载周一二三四五六日度 \
  --lv-font-name font_cn_16 --lv-include lvgl.h -o main/font_cn_16.c
```

> 注意：新增任何汉字都必须重新生成字体（或依赖上面那两级兜底），否则该字会显示成占位方框。
> `font_cn_14/16.c` 是手工用上面命令生成的，生成后要补上兜底链（`tools/gen_radio_font.sh`
> 会自动给 `font_radio_*` 加）：
>
> ```bash
> sed -i '' -e 's/#ifndef FONT_CN_/LV_FONT_DECLARE(font_zh_14);\n\n#ifndef FONT_CN_/' \
>           -e 's/\.fallback = NULL,/.fallback = \&font_zh_14,/' main/font_cn_14.c main/font_cn_16.c
> ```

## 许可证

- 代码：[MIT](LICENSE)
- 字体子集与兜底字体（`main/font_cn_*.c`、`main/font_radio_*.c`、`main/font_zh_14.c`）：
  由 **Noto Sans CJK SC / Noto Sans SC** 生成，[SIL Open Font License 1.1](LICENSES/OFL-NotoSansCJK.txt)
