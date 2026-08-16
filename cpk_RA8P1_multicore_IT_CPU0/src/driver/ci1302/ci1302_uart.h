/**
 ******************************************************************************
 * @file    ci1302_uart.h
 * @brief   CI1302 离线语音识别模块驱动（UART0 / SCI0, P602=RX P603=TX, 115200 8N1）
 *
 * 协议（见 ASR模块自定义固件/readme.txt 与 串口协议.pdf）：
 *   帧格式：AA 55 <CMD> <DATA> FB（5 字节，无帧号、无校验）
 *   发送协议：语音识别到命令词时，CI1302 通过串口把该帧发给上位机（RA8P1）。
 *   接收协议：RA8P1 通过串口发该帧给 CI1302，CI1302 播放对应播报词。
 *
 * 命令码用 16 位表示：ci1302_cmd_t = (CMD << 8) | DATA。
 ******************************************************************************
 */
#ifndef CI1302_UART_H_
#define CI1302_UART_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 帧定界 ---- */
#define CI1302_HEAD0   0xAA
#define CI1302_HEAD1   0x55
#define CI1302_TAIL    0xFB

/* ---- 命令码（16 位 = CMD<<8 | DATA） ---- */
typedef uint16_t ci1302_cmd_t;

/* 系统命令 */
#define CI1302_CMD_WELCOME          0x0100  /* 欢迎语（上电播报）   */
#define CI1302_CMD_INACTIVATE       0x026F  /* 休息语（进入休眠，RX） */
#define CI1302_CMD_WAKE             0x0300  /* 唤醒词「小萨小萨」     */
#define CI1302_CMD_VOL_UP           0x0400  /* 增大音量               */
#define CI1302_CMD_VOL_DOWN         0x0500  /* 减小音量               */
#define CI1302_CMD_VOL_MAX          0x0600  /* 最大音量               */
#define CI1302_CMD_VOL_MID          0x0700  /* 中等音量               */
#define CI1302_CMD_VOL_MIN          0x0800  /* 最小音量               */
#define CI1302_CMD_BROADCAST_ON     0x0900  /* 开启播报               */
#define CI1302_CMD_BROADCAST_OFF    0x0A00  /* 关闭播报               */

/* 自定义命令（组 0x00 —— 带完整播报词） */
#define CI1302_CMD_REC_START        0x001B  /* 开始录制       */
#define CI1302_CMD_REC_STOP         0x001C  /* 停止录制       */
#define CI1302_CMD_MODE_SWITCH      0x0026  /* 切换模式       */
#define CI1302_CMD_STT_ON           0x0027  /* 开启语音转文字 */
#define CI1302_CMD_STT_OFF          0x0028  /* 关闭语音转文字 */
#define CI1302_CMD_AI_ON            0x0029  /* 开启对话功能   */
#define CI1302_CMD_AI_OFF           0x002A  /* 关闭对话功能   */
#define CI1302_CMD_PPT_ON           0x002B  /* 开启控制 PPT   */
#define CI1302_CMD_PPT_OFF          0x002C  /* 关闭控制 PPT   */
#define CI1302_CMD_PLAY_SONG        0x002D  /* 放一首歌听听   */
#define CI1302_CMD_TRACK_FACE       0x0047  /* 开始追踪人脸   */
#define CI1302_CMD_TRACK_FACE_1     0x0049  /* 追踪一号人脸   */
#define CI1302_CMD_TRACK_FACE_2     0x004A  /* 追踪二号人脸   */
#define CI1302_CMD_TRACK_FACE_3     0x004B  /* 追踪三号人脸   */
#define CI1302_CMD_TRACK_CANCEL     0x004C  /* 取消追踪       */
#define CI1302_CMD_SOUND_TRACK      0x004D  /* 开始声源追踪   */
#define CI1302_CMD_FINGERPRINT_ENROLL   0x004E  /* 录入指纹（待指纹模块） */
#define CI1302_CMD_FINGERPRINT_CHECKIN  0x004F  /* 指纹打卡（待指纹模块） */

/* 自定义命令（组 0xFF —— 语义与组 0x00 重复的别名，播报词较新） */
#define CI1302_CMD_REC_START_V2     0xFF3D  /* 开始录制会议             */
#define CI1302_CMD_REC_STOP_V2      0xFF3E  /* 停止录制会议             */
#define CI1302_CMD_MODE_SWITCH_V2   0xFF3F  /* 切换人脸与手掌识别模式   */
#define CI1302_CMD_STT_ON_V2        0xFF40  /* 开启语音转文字功能       */
#define CI1302_CMD_STT_OFF_V2       0xFF42  /* 关闭语音转文字功能       */
#define CI1302_CMD_AI_ON_V2         0xFF43  /* 开启人工智能对话         */
#define CI1302_CMD_AI_OFF_V2        0xFF44  /* 关闭人工智能对话         */
#define CI1302_CMD_PPT_ON_V2        0xFF49  /* 开启控制 PPT 功能        */
#define CI1302_CMD_PPT_OFF_V2       0xFF58  /* 关闭控制 PPT 功能        */
#define CI1302_CMD_TRACK_FACE_V2    0xFF5E  /* 开始追踪人脸功能         */
#define CI1302_CMD_TRACK_CANCEL_V2  0xFF5F  /* 取消追踪人脸             */
#define CI1302_CMD_SOUND_TRACK_V2   0xFF60  /* 开始声源追踪功能         */
#define CI1302_CMD_FINGERPRINT_ENROLL_OK   0xFF61  /* 录入指纹成功（待指纹模块） */
#define CI1302_CMD_FINGERPRINT_ENROLL_V2   0xFF62  /* 录入指纹功能（待指纹模块） */
#define CI1302_CMD_FINGERPRINT_CHECKIN_V2  0xFF63  /* 指纹打卡功能（待指纹模块） */
#define CI1302_CMD_FINGERPRINT_CHECKIN_OK  0xFF64  /* 指纹打卡成功（待指纹模块） */

/** 收到一条完整命令帧时的回调（解析任务上下文，需自行线程同步）。 */
typedef void (*ci1302_cmd_cb_t)(ci1302_cmd_t cmd);

/** 初始化 UART0 + 启动解析任务。 */
void ci1302_uart_init(void);

/** 注册「收到命令帧」回调。 */
void ci1302_uart_set_cmd_cb(ci1302_cmd_cb_t cb);

/** 发送一条命令帧（接收协议）给 CI1302，触发对应播报。 */
bool ci1302_uart_send(ci1302_cmd_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* CI1302_UART_H_ */
