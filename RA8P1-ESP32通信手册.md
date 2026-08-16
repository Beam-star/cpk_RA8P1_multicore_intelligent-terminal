# RA8P1 使用手册

本文档说明 RA8P1 触摸 UI 如何通过 UART1 控制 ESP32-S3（`meeting_ppt` 固件）。

## 1. 总览

ESP32 上电后后台持续完成：

- 连接 WiFi
- 连接讯飞 IAT
- 启动并持续运行语音识别
- 监听 UART1

RA8P1 只负责通过 UART1 发送模式命令；ESP32 根据当前模式决定往 UART1 发什么，或者把 PPT 翻页命令通过 WiFi TCP 发给电脑。

## 2. 接线

```text
RA8P1 UART_TX    ->    ESP32 IO18 (U1RXD)
RA8P1 UART_RX    ->    ESP32 IO17 (U1TXD)
RA8P1 GND        ->    ESP32 GND
```

要求：

- 双方都是 3.3V TTL
- 必须共地
- 不要接 5V 或 VOUT1 到 UART 信号脚

## 3. 串口参数

```text
波特率：115200
数据位：8
停止位：1
校验：无
流控：无
```

## 4. 发送协议（RA8P1 -> ESP32）

命令是单字节二进制数据，没有帧头、帧尾或校验。

| 字节 | 含义 | ESP32 动作 |
|---|---|---|
| `0x01` | 进入实时转写 | 把识别结果发回 UART1 |
| `0x02` | 退出实时转写 | 停止发送，进入静默 |
| `0x03` | 进入对话 | 识别后调 DeepSeek，把回答发回 UART1 |
| `0x04` | 退出对话 | 停止发送，进入静默 |
| `0x06` | 进入 PPT 模式 | 等待 `0x07/0x08` |
| `0x07` | PPT 上一页 | WiFi TCP 发 `'0'` 给电脑 |
| `0x08` | PPT 下一页 | WiFi TCP 发 `'1'` 给电脑 |
| `0x09` | 退出 PPT 模式 | 进入静默 |

注意：

- `0x01`、`0x03`、`0x06` 三个主模式互斥，不能同时工作
- 任何模式退出后都回到静默
- 其他字节会被忽略

## 5. 接收协议（ESP32 -> RA8P1）

ESP32 返回的是纯 UTF-8 文本，不带任何前缀，每行以 `\n` 结尾。

上电初始化消息：

```text
WIFI_CONNECTED\n
ASR_READY\n
```

- `WIFI_CONNECTED`：WiFi 连接成功
- `ASR_READY`：讯飞语音识别会话建立成功

实时转写模式下，每识别出一句，返回一行：

```text
今天下午三点开会。\n
```

对话模式下，每句识别完成后返回 DeepSeek 回答：

```text
好的，我帮你记一下。\n
```

PPT 模式下 ESP32 不往 UART1 发语音文本，只把翻页命令发给电脑。

## 6. 模式状态机

```text
上电
  |
  v
静默（发完 WIFI_CONNECTED / ASR_READY 后等待）
  |
  +--0x01--> 实时转写
  |            |
  |            +--0x02--> 静默
  |
  +--0x03--> 对话
  |            |
  |            +--0x04--> 静默
  |
  +--0x06--> PPT
               |
               +--0x07/0x08--> 控制 PPT 左右
               |
               +--0x09--> 静默
```

模式之间可以直接切换，例如当前在实时转写时收到 `0x03`，直接切到对话。

## 7. PC 端怎么跑

电脑和 ESP32 必须连接同一个热点或 WiFi。

进入 PC 目录：

```powershell
cd meeting_ppt\pc
python ppt_server.py
```

服务端会：

- 自动打开同目录下的 `show.pptx` 并开始放映
- 监听 TCP `9000`
- 在 UDP `9001` 广播自己的 IP，供 ESP32 自动发现

启动后终端会打印：

```text
Listening on 0.0.0.0:9000
UDP discovery broadcasting on port 9001
```

ESP32 连上后会打印：

```text
ESP32 connected from 10.43.142.110:xxxxx
```

发现口令默认是：

```text
ra8p1_meeting_2026
```

`ppt_server.py` 和 ESP32 固件里的口令必须一致，否则 ESP32 不会接受该电脑的广播。

## 8. Jetson 桌面模拟器（可选）

如果暂时不用 RA8P1，可以在 Jetson 上运行模拟器：

```bash
python3 /home/jetson/ra8p1_simulator.py
```

界面按钮与 `0x01 ~ 0x09` 一一对应，可以直接模拟 RA8P1 操作。

## 9. 常见问题

**没有收到 `WIFI_CONNECTED`**

- 检查 ESP32 COM5 日志是否出现 `reason=201`，说明找不到 WiFi
- 确认热点名和密码正确

**收到 `WIFI_CONNECTED` 但没收到 `ASR_READY`**

- 检查讯飞 AppID / APIKey / APISecret
- 检查 ESP32 是否已经联网并能访问讯飞

**PPT 命令发不出去**

- 确认电脑 `ppt_server.py` 在运行
- 确认 ESP32 COM5 日志出现 `connected to PC server`
- 确认热点没有开启 AP 隔离
- 确认两边发现口令一致

**UART1 收到乱码**

- 检查波特率是否都是 115200
- 检查 TX/RX 是否接反
- 检查 GND 是否共地
