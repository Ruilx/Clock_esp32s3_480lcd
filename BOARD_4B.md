# Waveshare ESP32-S3-Touch-LCD-4B 板级接线

以下接线来自 4B 原理图的 J2/U5/U8/U9 网络、用户对 J2 的逐线核对，以及本地官方 Arduino 示例。驱动已经按这组资料编写，但颜色、时序和断电保持仍待实板验证。不适用于型号相近的 LCD-4。

## LCD：18 个接口位，实际驱动 16 位

| J2 数据位／原理图网络 | ESP32-S3 GPIO，按低位到高位 |
|---|---|
| D0／B0（J2-16） | 不接 ESP32 |
| D1–D5／B1–B5 | 40, 41, 42, 2, 1 |
| D6–D11／G0–G5 | 21, 8, 18, 45, 38, 39 |
| D12／R0（J2-28） | 不接 ESP32 |
| D13–D17／R1–R5 | 10, 11, 12, 13, 14 |

J2 的命名是三组各 6 位，不代表 ESP32 在发送 RGB666。两组外侧颜色各少接最低一位，实际是 5+6+5。原理图的 R/B 标签与随包 Arduino_GFX 的参数标签相反；代码**沿用示例实际填入 `data_gpio_nums` 的 GPIO 次序**，而不是照网络名重新排序。首轮上板用纯红、纯绿、纯蓝和色阶确认，若只红蓝对调，先核对颜色映射，不改接线或增加数据线。

- RGB 同步：`DE=GPIO17`、`HS=GPIO46`、`VS=GPIO3`、`PCLK=GPIO9`。
- 背光：`BL=GPIO4` 通过 `R37`（10 kΩ）和 `R29`（68 kΩ）注入 AP3032 的 `FB`，中间节点接 `C38`（100 nF）；`CTRL` 由 `R26` 拉到 3.3 V。这不是高电平使能：高电平抬高反馈、压低 LED 电流，低电平点亮。低有效控制已在实板验证。
- 时序从例程取值：PCLK 目标 12 MHz；前肩/脉宽/后肩为水平 10/8/50、垂直 10/8/20。
- `TCA9554`（`0x20`）的 `EX0=CS`、`EX1=SDA`、`EX2=SCL`、`EX7=RST` 用于 ST7701 三线串行初始化，不传输像素。官方表切回标准命令页后发送 `0x21`（INVON）。省略它的上板色卡出现黑白反转，下一版只恢复这一条并复查色卡。保持官方 Gamma 和 `0x3A=0x60`，不写 `MADCTL` 调整 RGB/BGR。见[屏幕颜色自检](diagnostics/lcd_color/README.md)。
- PSRAM 内 1 个全帧缓冲，LCD DMA 用内部 RAM 的 20 行 bounce buffer；暂不使用双全帧。

## I²C、时钟和触摸

- 共享 I²C：`SDA=GPIO47`、`SCL=GPIO48`；板载上拉。
- RTC：`PCF85063AT`，7 位地址 `0x51`。主电池座 → AXP2101 `BAT`；`RTCLDO` → `VCC-RTC` → RTC `VDD`。健康电池及 RTCLDO 正常输出时，这条路径可在拔 USB 后维持 RTC 供电；当前未接电池，不能假定完全断电后还能保时。需要分别测试软件重启和完全断电恢复。
- GT911：共用上述 SDA/SCL；`INT=EX6`、`RST=EX5`。首版不初始化触摸，不改这两个扩展器位。

## 本地来源

- `doc/ESP32-S3-Touch-LCD-4B_SchematicDiagram.pdf`
- `doc/ESP32-S3-Touch-LCD-4B-Demos/Arduino-v3.2.0/examples/03_LVGL_PCF85063_simpleTime/`
- 同包 `libraries/GFX_Library_for_Arduino/src/databus/Arduino_ESP32RGBPanel.cpp` 和 `src/display/Arduino_RGB_Display.h` 的 `st7701_type1_init_operations`。
- 同包 `ESP-IDF-v5.4.2/02_lvgl_demo_v9/sdkconfig` 的 LCD 缓冲配置。
