# SCS 舵机驱动（FEETECH STS 磁编码串行舵机）

从 `FTServo_stm32HAL/SCSLib` 移植到 Renesas FSP 的 **FT-SCS 协议** 驱动，用于
会议终端的人脸追踪 / 声源追踪（单自由度水平舵机，左右各 30°）。

## 硬件接线与 FSP 配置

驱动使用 **UART4 / SCI4（`g_uart4`）**，引脚 **P414 = RX，P415 = TX，1Mbps 8N1**，
已在 e2studio FSP 中配置好（`ra_gen/hal_data.c` 里生成 `g_uart4`，channel=4）。

> 注意：UART4 的 `p_callback` 已在 FSP 中设为 `UART4_Callback`（`ra_gen/hal_data.c`
> 里 `g_uart4_cfg.p_callback = UART4_Callback`），该函数由本驱动在 `scs_servo.c`
> 中定义（RXI 中断投递 RX 字节到环形缓冲）。若重新生成 FSP，确认回调仍是
> `UART4_Callback`。

舵机参数（`scs_servo.h` 顶部 `#define`，按需修改）：
- `SCS_SERVO_ID` = 1（出厂默认站号）。
- 协议为**异步双工**（TTL，TX/RX 分离）；若舵机为单线半双工，把 TX/RX 在
  接线端并到一起即可（驱动本身按全双工收发）。

## 使用

- `scs_servo_init()` —— 打开 UART4 + 使能扭矩 + 回中位。舵机未接时各操作会
  超时返回 false，不会卡死启动。
- `scs_servo_write_pos(pos, speed, acc)` —— 写绝对位置（0..4095，中位 2048=0°）。
- `scs_servo_read_pos()` / `scs_servo_torque()` / `scs_servo_is_moving()`。

位置换算：`1 单位 = 0.087°`，即 `30° ≈ 345 单位`；中位 2048 对应 0°。
速度单位 0.732 RPM（0 = 最快，越小越慢），加速度单位 8.7°/s²。

上层追踪引擎见 `src/lvgl_ui/lvgl_ui_tracking.c`（人脸 PID + 声源比例/EMA）。
