#include <cpu1main_thread.h>
#include "rpmsg_core.h"
#include "rpmsg_log.h"
#include "rpmsg_record_cpu1.h"
#include "driver/sd_card/sdhi_driver.h"
#include "driver/pdm_audio/pdm_audio.h"
#include "driver/audio_codec/es8156.h"
#include "driver/audio_codec/i2s_driver.h"
#include "recorder/av_recorder.h"
#include "recorder/video_recorder.h"
#include "player/audio_player.h"
#include "player/video_player.h"

#define AV_RECORD_ENABLE  1   /* PDM→WAV 音频录制 */

/* 喂 WDT: R_BSP_SoftwareDelay 内部调 WDT_Refresh */
#define WDT_FEED()  R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS)

/* New Thread entry function */
void cpu1main_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

    vTaskDelay(pdMS_TO_TICKS(250));
    //WDT_FEED();

    rpmsg_core_init();
    rpmsg_log_cpu1_init();
    //WDT_FEED();

#if AV_RECORD_ENABLE
    /* ---- SD 卡 + FAT 文件系统 ---- */
    if (sd_card_init()) {
        rpmsg_log_cpu1_printf("[MAIN] SD card OK\r\n");
    } else {
        rpmsg_log_cpu1_printf("[MAIN] SD card FAILED — recording disabled\r\n");
    }
    //WDT_FEED();

    /* ---- PDM 麦克风 (3ch 16kHz, Mic0→WAV) ---- */
    if (pdm_audio_init()) {
        rpmsg_log_cpu1_printf("[MAIN] PDM OK\r\n");
    } else {
        rpmsg_log_cpu1_printf("[MAIN] PDM FAILED\r\n");
    }
    //WDT_FEED();

    /* ---- 音频播放链 (MCLK → ES8156 → I2S → 播放器) ----
     * 非致命:任一步失败只禁用播放,不影响录音。
     * 顺序很重要:MCLK 必须先于 codec/SSI (参考 Titan_Mini_wavplayer)。 */
    bool audio_ok = i2s_mclk_start();
    //WDT_FEED();
    if (audio_ok) audio_ok = es8156_init();
    //WDT_FEED();
    if (audio_ok) audio_ok = i2s_init(true);
    //WDT_FEED();
    if (audio_ok) {
        audio_player_init();
        rpmsg_log_cpu1_printf("[MAIN] Audio playback OK\r\n");
    } else {
        rpmsg_log_cpu1_printf("[MAIN] Playback DISABLED (codec/I2S init failed)\r\n");
    }

    /* ---- 音频录制器 ---- */
    av_recorder_init();
    //WDT_FEED();

    /* ---- 视频录制器 (MJPEG 编码) ---- */
    video_recorder_init();
    //WDT_FEED();

    /* ---- 视频回放器 (MJPEG 解码) ---- */
    video_player_init();
    //WDT_FEED();

    /* ---- RPMsg 录制控制 + 播放通道 ---- */
    if (rpmsg_record_cpu1_init()) {
        rpmsg_log_cpu1_printf("[MAIN] Record channel ready\r\n");
    } else {
        rpmsg_log_cpu1_printf("[MAIN] Record channel FAILED\r\n");
    }
    //WDT_FEED();
#endif

    while (1) {
        //WDT_FEED();
        vTaskDelay(1000);
    }
}
