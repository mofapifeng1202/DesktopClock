# lvgl_st77922_demo

ESP32-S3 (N16R8) + LCDWiki ES3C35P 3.5" 320x480 IPS（ST77922 QSPI + ST7123 触摸）桌面时钟 / 天气站。
ESP-IDF 5.4.1 + LVGL 9 + esp_lvgl_port。

## 功能

- 时钟页：48 号数字时钟、秒、星期日期、NTP 对时状态、音量滑块
- 天气页：重庆（写死 29.56N/106.55E）当前温度/天气/湿度/风速 + 3 天预报，Open-Meteo 免 key，30 分钟刷新
- 顶部状态栏：电池电压 + 百分比（ADC1_CH7，1:2 分压）、Wi-Fi 信号、对时状态
- 左右滑动切换两页；BOOT 键切背光；RGB LED 指示 Wi-Fi 状态
- ES8311 音频（I2S），开机提示音 + 音量控制

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

首次编译前先配置 Wi-Fi：

```bash
cp main/wifi_app_secret.h.example main/wifi_app_secret.h
# 然后填入你的 2.4GHz Wi-Fi 名称/密码（该文件不入库）
```

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
2. **限幅平移**：单帧位移超过 60px 视为鬼点，只按 60px 沿原方向推进（不要用"冻结/丢弃"，
   那会把滑动速度和惯性抹平，导致切页困难）
3. 手指抬起立即上报 RELEASED，并在下次触摸时重置参考点

运行时会限频打印 `touch ghost clipped (N total): x,y`，可用于长期观察噪声水平。

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

- 中文字体是 `font_cn_14/16.c` 的子集，**新增任何汉字都会显示空白**，需用 `lv_font_conv` 重新生成
- `CONFIG_BT_ENABLED=y` 但代码未用蓝牙，白占 flash/RAM，可关闭
- 优化等级为 Debug，改 Release/Size 可提升帧率并缩小体积（横屏软件旋转有额外 CPU 开销，收益更明显）
- 16MB flash 只用 3MB app 分区，其余空闲（未挂 SPIFFS/LittleFS）
- `TP_PROBE` / `TP_DEBUG_COORDS` 是触摸调试开关，稳定后可置 0 减少日志噪声

## 字体与许可

`main/font_cn_14.c` / `main/font_cn_16.c` 是用 `lv_font_conv` 从 **Noto Sans CJK SC**
生成的子集（只含界面用到的 55 个汉字 + ASCII 0x20-0x7E），源字体采用
**SIL Open Font License 1.1**，许可全文见 `LICENSES/OFL-NotoSansCJK.txt`。

重新生成（需要 Node.js + lv_font_conv + Noto Sans CJK SC 的 OTF）：

```bash
npx lv_font_conv --font NotoSansCJKsc-Regular.otf --size 16 --bpp 4 \
  --format lvgl --no-compress --no-kerning --range 0x20-0x7e \
  --symbols 重庆晴间多云阴雾毛毛雨雪阵雷未知电量连接中失败关闭对时已同步音左右滑动查看天气湿风速更新加载周一二三四五六日度 \
  --lv-font-name font_cn_16 --lv-include lvgl.h -o main/font_cn_16.c
```

> 注意：新增任何汉字都必须重新生成字体，否则该字会显示为空白。
