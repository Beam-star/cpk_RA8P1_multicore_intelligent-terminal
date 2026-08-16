/**
 ******************************************************************************
 * @file    esp32_uart.h
 * @brief   RA8P1 <-> ESP32 串口通信驱动（UART2 / SCI2, P801/P802, 115200 8N1）
 *
 * 两个功能模式（云端大模型接口已部署在 ESP32 上）：
 *   1. 实时语音转文字（STT）：ESP32 麦克风 -> 云端 -> 返回中文
 *   2. AI 智能体对话（AI）：RA8P1 <-> ESP32 <-> 云端大模型
 *
 * RA8P1 负责：发命令给 ESP32，接收 UTF-8 中文并显示字幕 / 存会议记录。
 * 协议详见本目录下的「数据格式.md」。
 ******************************************************************************
 */

#ifndef ESP32_UART_H_
#define ESP32_UART_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RA8P1 -> ESP32 命令（单字节） ---- */
typedef enum {
    ESP32_CMD_STT_ON   = 0x01,  /* 开始实时语音转文字 */
    ESP32_CMD_STT_OFF  = 0x02,  /* 停止实时语音转文字 */
    ESP32_CMD_AI_ON    = 0x03,  /* 开始 AI 智能体对话   */
    ESP32_CMD_AI_OFF   = 0x04,  /* 停止 AI 智能体对话   */
    ESP32_CMD_PPT_ON   = 0x06,  /* 进入 PPT 模式        */
    ESP32_CMD_PPT_PREV = 0x07,  /* PPT 上一页           */
    ESP32_CMD_PPT_NEXT = 0x08,  /* PPT 下一页           */
    ESP32_CMD_PPT_OFF  = 0x09,  /* 退出 PPT 模式        */
} esp32_cmd_t;

/**
 * 收到一行 UTF-8 中文（null 结尾）时的回调。
 * 在解析任务上下文中调用，需自行做线程同步（如 lv_async_call）。
 */
typedef void (*esp32_line_cb_t)(const char *line);

/** 初始化 UART2 + 启动解析任务。 */
void esp32_uart_init(void);

/** 发送一条命令给 ESP32。 */
bool esp32_uart_send_cmd(esp32_cmd_t cmd);

/** 注册「收到一行字幕」回调。 */
void esp32_uart_set_line_cb(esp32_line_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_UART_H_ */
