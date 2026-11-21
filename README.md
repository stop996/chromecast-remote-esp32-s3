# Chromecast Remote for ESP32-S3

用一块ESP32-S3，把 Chromecast with Google TV 的蓝牙遥控器接到 Windows。ESP32 通过一根 USB 数据线提供 **麦克风、键盘、媒体键和调试串口**。

## 功能

- **按住说话。** 按住语音键发送麦克风音频，松开时结束。语音键默认同时按下右 Alt 和右 Ctrl，可在微信输入法中选择同一组「按住说话」快捷键。
- **网页配置快捷键。** 可配对遥控器、录制键盘组合、编辑串口事件名，也可将按键设为系统音量键。普通快捷键每按一次触发一次。
- **麦克风增益可调。** 16 kHz 单声道 USB 音频，增益 1× 至 16×。高增益可能使响声失真。
- **设置保存到设备。** 保存后 ESP32 会关闭配置网页和 Wi-Fi 热点。

## 硬件

| 设备 | 本版本配置 |
| --- | --- |
| 开发板 | 微雪 ESP32-S3-Tiny，4 MB Flash |
| 遥控器 | 以 Chromecast with Google TV 原装 Voice Remote（G9N9N）的按键和 ATVV 语音协议为目标 |
| 电脑 | Windows，通过支持数据传输的 USB 线连接 ESP32 |

替代遥控器能识别哪些按键、能否收音，取决于它的蓝牙接口。部分遥控器会通过红外发送电源、音量或输入源指令，ESP32 只接收蓝牙数据。 [微雪板卡文档](https://docs.waveshare.com/ESP32-S3-Tiny)

## 下载与烧录

打开 [v0.5.0-gain1 发布页](https://github.com/stop996/chromecast-remote-esp32-s3/releases/tag/v0.5.0-gain1)，下载 `Chromecast_Mic_Keyboard_esp32-s3-tiny-v0.5.0-gain1.bin`。合并镜像用于 4 MB Flash 的开发板，从地址 `0x0` 写入。烧录合并镜像会覆盖设备上已有的配对及配置。

按住 **BOOT**，按一下 **RST**，再松开 **BOOT**，即可进入下载模式。以下是 esptool 4.x 的例子，将 `COM7` 换成你的串口。

```powershell
python -m esptool --chip esp32s3 --port COM7 write_flash 0x0 Chromecast_Mic_Keyboard_esp32-s3-tiny-v0.5.0-gain1.bin
```

## 首次配置

1. 连接 ESP32 创建的 **Chromecast-Setup-XXXX** 热点，密码为 `castsetup`。在浏览器打开 `http://192.168.4.1`。
2. 长按遥控器的 **返回 + Home**，等指示灯闪烁后，在网页点 **扫描** 并连接。 [遥控器配对说明](https://support.google.com/chromecast/answer/10109990?hl=en)
3. 在 **USB 键盘组合** 一栏点 **录制**，然后在电脑键盘上按下并松开所需组合。也可以手动输入。语音键的默认组合为 `RAlt+RCtrl`。
4. 选择 **麦克风增益倍率**，点 **保存并关闭 Wi-Fi**。
5. 在 Windows 的 **设置 → 系统 → 声音 → 输入** 中选择 **Chromecast Remote Microphone**。 [Windows USB Audio 类驱动](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-audio-class-system-driver--usbaudio-sys-)

需要重新打开配置页时，短按 **RST** 并松开，等约 1 秒，再按住 **BOOT** 约 1 秒。两个按钮分开按，同时按会进入下载模式。

### 常用默认映射

| 遥控器按键 | USB 输出 |
| --- | --- |
| 确认 | Enter |
| 返回 | Backspace |
| 左 / 右 | 对应方向键 |
| 上 / 下 | Ctrl+`[` / Ctrl+`]` |
| Home | Ctrl+Alt+Shift+T |
| Netflix / 静音 | Ctrl+Alt+Shift+N |
| 音量加 / 减 | 系统音量加 / 减 |
| 语音 | 按住右 Alt + 右 Ctrl，随遥控器按键释放 |

## 从源码编译

用 PlatformIO 打开含 `platformio.ini` 的目录。在 Windows 上建议使用英文路径。构建配置使用 `espressif32 @ 6.10.0` 和 Arduino 框架。

```powershell
pio run -e esp32-s3-tiny-mic
pio device list
pio run -e esp32-s3-tiny-mic -t upload --upload-port COM7
```

PlatformIO 生成的 `firmware.bin` 是应用镜像。用上面的上传命令烧录开发板。

发布固件的 Arduino-ESP32 依赖来自 Espressif，框架版本为 2.0.17。 [框架源码与许可](https://github.com/espressif/arduino-esp32/tree/dcc1105b0cf1322a437b354c336f2abf72b7e512) · [PlatformIO 平台源码](https://github.com/platformio/platform-espressif32/tree/v6.10.0)

## 串口排查与验证

串口使用 **115200 / 8N1**。烧录口和运行后的 CDC 口可能是不同编号，可用 `pio device list` 确认。

| 命令或输出 | 用途 |
| --- | --- |
| `status` | 查看遥控器连接与配置状态 |
| `mic status` | 检查 USB 音频输入状态、蓝牙音频包与已解码样本计数 |
| `hid status` | 检查 USB 键盘就绪状态和已提交的按下、松开报告 |
| `KEY … down/up ASSISTANT` | 查看语音键按下和松开的时间 |
| `RAW` / `HID_MAP` | 查看原始按键报文及 HID 描述符，排查兼容遥控器 |

此版本已通过源码解压重编译、固件镜像校验、网页设置测试及 USB 键盘报告回放。微信输入法的快捷键触发效果仍需用实体设备验证。

## 参考

- [RemoteMapper-ESP32](https://github.com/cuicui-V5/RemoteMapper-ESP32/blob/master/DEVELOPMENT.md) 的 ESP32 蓝牙主机与串口思路
- [InputPlumber 的 18D1:9450 采样](https://github.com/ShadowBlip/InputPlumber/issues/457) 与 [Google Voice over BLE 规范](https://wangefan.github.io/linux_kernel_driver/resources/Google_Voice_over_BLE_spec_v1.0.pdf)
- [vibe-mote 的 G9N9N 音频实现](https://github.com/Tilkmilk/vibe-mote/blob/main/voice.py) 及 [MoteDeck](https://github.com/HZDavy/MoteDeck)、[无线麦 Windows 版](https://github.com/ZSTDJan/windows-remote-mic-app) 的兼容性参考
