# 硬件说明（ES3C35P）

板卡：**LCDWiki ES3C35P** —— 3.5 寸 IPS + ESP32-S3 的一体化显示模块，非 Espressif 官方开发板。
主控芯片为**正品 Espressif ESP32-S3R8**（QFN56，rev v0.2，内嵌 8MB 八线 PSRAM），
外部 16MB QSPI NOR Flash（Zbit ZB25VQ128）。设备标识（MAC / eFuse 唯一 ID）已在此文档中脱敏。

## 1. 主要器件

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3R8 | 双核 LX7 @240MHz，8MB OPI PSRAM，Wi-Fi 2.4G + BLE5 |
| 外部 Flash | Zbit ZB25VQ128 | 16MB，Quad，3.3V |
| 显示屏 | ST77922 | 3.5" IPS，320×480（原生竖屏），65K RGB565，QSPI |
| 触摸 | ST7123 | 电容屏控制器，I²C，5 点，与面板一体化 |
| 音频 | ES8311 + SC8002B | I²C 控制 + I²S 数据 + 板载功放，接 1.25mm 喇叭座 |
| 充电 | TP4054 + SL2305 | 单节锂电 CC/CV 充电 + P-MOS 电源通路切换 |
| 存储扩展 | microSD | 4-bit SDIO |
| 指示灯 | WS2812 | 单灯，GPIO40 |

## 2. 引脚映射（固件实际使用）

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD CS / PCLK | 10 / 12 | QSPI |
| LCD D0 / D1 / D2 / D3 | 11 / 13 / 14 / 9 | QSPI 四线 |
| LCD RST | — | 接板级 EN，非 GPIO（驱动传 `-1`） |
| LCD BL | 41 | 高电平点亮，支持 PWM 调光 |
| TP SDA / SCL | 38 / 39 | I²C0，100kHz |
| TP RST / INT | 48 / 47 | 复位需 300ms 稳定期，否则 I²C NACK |
| Audio EN | 1 | 低电平使能 |
| I²S MCLK / BCLK / WS | 17 / 18 / 21 | |
| I²S DOUT / DIN | 15 / 16 | |
| RGB LED | 40 | WS2812 |
| BOOT 键 | 0 | 固件里用于切换背光 |
| 电池 ADC | 8 | ADC1_CH7，BAT+ 经 100K/100K 分压，读数 ×2 |

## 3. 供电与充电

```
USB-C 5V ──┬── TP4054 ── BAT+ ── 1.25mm 2P 电池座 (JP1)
           │     (CC/CV 4.2V，R12=3.3k → ~290mA)
           └── SL2305 (P-MOS) ── 系统 3.3V LDO
                 插电时切断电池放电，拔线时由电池供电
BAT+ ── 100K/100K 分压 ── GPIO8 (BAT_ADC)
```

- 有过充保护（恒压 4.2V 截止），**没有过放保护**——需使用自带保护板的电芯
- 线性充电，充电时芯片发热属正常现象
- 显示 + 喇叭 + 充电同时工作总电流可达 ~400mA，接近 500mA 供电上限，充电会很慢

## 4. 显示：横屏是用软件旋转实现的

面板原生 320×480 竖屏。虽然 ST77922 支持 MADCTL.MV 硬件旋转，但**本模块开启后画面会整体错乱**
（ESP-IDF 驱动把 MV 做成"仅验证过的模组可开启"的开关就是这个原因）。因此：

- 面板保持原生竖屏，LVGL 用 `disp_cfg.flags.sw_rotate = true` + `lv_display_set_rotation(90)`
- **陷阱 1**：`rounder_cb` 的 4 像素对齐要跟着转。ST77922 要求 CASET（面板 X）4 对齐，
  旋转 90° 后面板 X = 逻辑 Y，所以要对齐 `y1/y2` 而不是 `x1/x2`，否则每个刷新块错位 → 元件花屏
- **陷阱 2**：不要在触摸回调里自己旋转坐标。LVGL 9 的 `lv_display_rotate_point()` 已按
  `X = ver_res-1-y, Y = x` 自动换算，驱动只需上报面板原始坐标（x:0..319, y:0..479）。
  双重旋转的症状：只有一部分屏幕能点、滑块卡在中点、竖直滑动反而切页

## 5. 触摸噪声与滤波

实测这块屏的电容触摸噪声明显：**约 40 次/分钟、幅度几百像素的鬼点跳变**
（例如 `(292,258) → (5,354)`）。ST7123 驱动不写任何阈值/消抖配置，全部噪声直通 LVGL。

固件 `tp_indev_read_cb()` 的处理：
1. 5 个上报点中取**接触面积最大**的那个（真实手指优于鬼点）
2. **限幅平移**：单帧位移超过 60px 时只按 60px 沿原方向推进
   （不要用"冻结/丢弃"，那会抹平滑动速度与惯性，导致切页困难）
3. 抬手立即上报 RELEASED，下次按下重新取基准

## 6. eFuse 摘要（已脱敏）

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) revision v0.2 |
| Wafer / Package | minor 2 / 0 |
| PSRAM | 8MB，vendor AP_3v3，85℃ |
| Flash 类型 | 4 data lines（quad） |
| ADC 校准 | 六档 init code 齐全，TEMP_CALIB 存在 |
| 安全启动 / Flash 加密 | 未启用 |
| JTAG / USB-Serial-JTAG | 均未锁死，可正常调试 |

## 7. 构建环境

- ESP-IDF **5.4.1**，工具链 esp32s3
- 组件：esp_lcd_st77922 2.1.1、esp_lcd_touch_st7123 1.0.2、esp_lvgl_port 2.9.x、LVGL 9.2.x
- 国内网络下的安装要点（gitee 源码 + jihulab 子模块镜像、`IDF_GITHUB_ASSETS` 工具链镜像、
  Python 3.9 的 `ruamel.yaml` dist-info 问题、`cmake<4`）见仓库根目录 `README.md`
