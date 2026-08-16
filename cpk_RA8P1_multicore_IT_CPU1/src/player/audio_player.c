/**
 ******************************************************************************
 * @file    audio_player.c
 * @brief   WAV 文件播放 — SD卡读取 → PCM → I2S DMA
 *
 * 播放循环:
 *   SD卡读 1024 采样 → I2S Start TX → 等 DTC 完成信号量 → 读下一块
 *
 * 支持: 16-bit PCM, 16kHz, mono (与录音格式一致)
 ******************************************************************************
 */

#include "audio_player.h"
#include "driver/sd_card/sdhi_driver.h"
#include "driver/audio_codec/i2s_driver.h"
#include "encoder/wav_encoder.h"       /* wav_header_t                    */
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/*
 * 每块 8192 采样 = 16KB = 256ms(立体声@16kHz)。
 * 块越大,预读下一块的时间预算越足 —— 录音任务写 SD 时会持有 FAT 锁
 * 数十 ms,小块(32ms)时预读赶不上播放进度导致 FIFO 下溢/I2S 超时。
 */
#define PCM_BUF_SAMPLES  8192

/* 播放乒乓双缓冲 (2 × 256ms @ 16kHz stereo, SDRAM): 一块播放一块预读 */
static int16_t         g_pcm_buf[2][PCM_BUF_SAMPLES] __attribute__((section(".sdram_noinit")));
static player_state_t  g_state = PLAYER_IDLE;
static volatile bool   g_paused = false;
static volatile uint32_t g_total_ms = 0;
static volatile uint32_t g_elapsed_ms = 0;

/* 当前打开的文件句柄 (NULL=已关闭)。播放任务崩溃时会泄漏, 由
 * audio_player_force_close() 从看门狗任务上下文中强制关闭。 */
static void *g_active_fp = NULL;

/* 卡死自愈: 单次播放超过此时长视为卡死, 下次播放请求可强制夺回 */
#define PLAYER_STUCK_RESET_MS  (60U * 1000U)
static volatile TickType_t g_play_start_tick = 0;

/* ---- WAV 头解析 ---- */

static bool parse_wav_header(void *fp, uint32_t *data_start, uint32_t *data_size,
                             uint16_t *channels, uint32_t *sample_rate)
{
    wav_header_t hdr;
    sd_card_fseek(fp, 0);
    if (sd_card_fread(fp, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        printf("[PLAYER] Failed to read WAV header\r\n");
        return false;
    }

    if (memcmp(hdr.riff_id, "RIFF", 4) != 0 ||
        memcmp(hdr.wave_id, "WAVE", 4) != 0) {
        printf("[PLAYER] Not a WAV file\r\n");
        return false;
    }

    printf("[PLAYER] WAV: %luHz %u-bit %u-ch, PCM=%lu B\r\n",
           (unsigned long)hdr.sample_rate,
           (unsigned)hdr.bits_per_sample,
           (unsigned)hdr.num_channels,
           (unsigned long)hdr.data_size);

    if (hdr.bits_per_sample != 16) {
        printf("[PLAYER] Unsupported bit depth (16-bit only)\r\n");
        return false;
    }
    if (hdr.num_channels == 0 || hdr.num_channels > 2) {
        printf("[PLAYER] Unsupported channel count\r\n");
        return false;
    }
    if (hdr.sample_rate != I2S_SAMPLE_RATE) {
        printf("[PLAYER] WARN: sample rate != %u, pitch will be wrong\r\n",
               (unsigned)I2S_SAMPLE_RATE);
    }

    *data_start  = sizeof(hdr);
    *data_size   = hdr.data_size;
    *channels    = hdr.num_channels;
    *sample_rate = hdr.sample_rate;
    return true;
}

/* ======================================================================== */
/*  Public                                                                   */
/* ======================================================================== */

bool audio_player_init(void)
{
    printf("[PLAYER] Ready\r\n");
    return true;
}

/*
 * 读取下一块 PCM 到指定缓冲区。
 * mono 时读半量样本到缓冲区后半段并原地展开成 L=R 交织(读先于写,安全)。
 * 返回本块 TX 样本数, 0 = 无数据/读错误。
 */
static uint32_t fill_block(void *fp, int16_t *buf, bool mono,
                           uint32_t *p_remaining)
{
    const uint32_t chunk_bytes = mono
        ? (PCM_BUF_SAMPLES / 2) * sizeof(int16_t)
        : PCM_BUF_SAMPLES * sizeof(int16_t);

    uint32_t bytes = (*p_remaining < chunk_bytes) ? *p_remaining : chunk_bytes;
    bytes &= ~1u;                           /* 对齐到 int16 样本 */
    if (bytes == 0) return 0;

    int16_t *dst = mono ? &buf[PCM_BUF_SAMPLES / 2] : buf;
    if (sd_card_fread(fp, dst, bytes) != bytes) {
        printf("[PLAYER] Read error\r\n");
        return 0;
    }
    *p_remaining -= bytes;

    if (mono) {
        uint32_t n = bytes / sizeof(int16_t);
        const int16_t *src = &buf[PCM_BUF_SAMPLES / 2];
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = src[i];
            buf[2 * i]     = s;
            buf[2 * i + 1] = s;
        }
        return n * 2;
    }
    return bytes / sizeof(int16_t);
}

bool audio_player_play(const char *path)
{
    if (!path) return false;

    /* 忙守卫:同一时刻只允许一个播放 (rec_ctrl / sound 任务并发调用)。
     * 自愈:若上一次播放卡死超过 PLAYER_STUCK_RESET_MS (等待中的任务
     * 早已超时退出或挂死), 强制复位状态, 避免永久 "Busy" 锁死。 */
    taskENTER_CRITICAL();
    if (g_state != PLAYER_IDLE) {
        TickType_t held = xTaskGetTickCount() - g_play_start_tick;
        if (held < pdMS_TO_TICKS(PLAYER_STUCK_RESET_MS)) {
            taskEXIT_CRITICAL();
            printf("[PLAYER] Busy\r\n");
            return false;
        }
        /* 卡死超时: 夺回播放器 */
        g_state = PLAYER_IDLE;
        taskEXIT_CRITICAL();
        printf("[PLAYER] WARN: stale state (stuck >60s), force reset\r\n");
        taskENTER_CRITICAL();
        if (g_state != PLAYER_IDLE) {   /* 竞争检查 */
            taskEXIT_CRITICAL();
            return false;
        }
    }
    g_state = PLAYER_PLAYING;
    g_paused = false;
    g_play_start_tick = xTaskGetTickCount();
    taskEXIT_CRITICAL();

    printf("[PLAYER] Playing: %s\r\n", path);

    void *fp = sd_card_fopen(path, "r");
    if (!fp) {
        printf("[PLAYER] File not found\r\n");
        g_state = PLAYER_IDLE;
        return false;
    }
    g_active_fp = fp;

    uint32_t data_start, data_size, sample_rate;
    uint16_t channels;
    if (!parse_wav_header(fp, &data_start, &data_size,
                          &channels, &sample_rate)) {
        sd_card_fclose(fp);
        g_active_fp = NULL;
        g_state = PLAYER_IDLE;
        return false;
    }

    sd_card_fseek(fp, data_start);

    /*
     * 乒乓双缓冲播放 (从根本上消除块间 I2S 下溢):
     *   当前块 DMA 播放的同时, 提前从 SD 卡读入下一块到另一缓冲。
     *   SD 读 (~10ms) 远快于块时长 (256ms), 所以 TX_EMPTY 到来时下一块
     *   已就绪, 可立即启动, 块间无 SD 读取间隙 → FIFO 不再排空 → 无下溢。
     *
     * 旧顺序读模型每块边界必有一次下溢 (SD 读间隙 10ms > FIFO 2ms), 靠
     * i2s_start_tx 里 stop/close/open 反复恢复; 长时间播放累积数百次这种
     * 异步重开, 偶发一次竞态就 HardFault。乒乓模型只在 SD 偶发长延迟
     * (>256ms) 时才可能下溢, 触发概率从"每块必现"降到"几乎为零"。
     */
    bool     mono = (channels == 1);
    uint32_t remaining = data_size;
    bool     err = false;

    /* 总时长 (毫秒): data_size 字节 / 字节率 */
    uint32_t bytes_per_sec = sample_rate * 2u * (uint32_t)channels;  /* 16-bit */
    g_total_ms = (bytes_per_sec > 0) ? (uint32_t)((uint64_t)data_size * 1000u / bytes_per_sec) : 0;
    g_elapsed_ms = 0;

    /* 预读第一块到缓冲 0 */
    uint32_t tx_n[2];
    tx_n[0] = fill_block(fp, g_pcm_buf[0], mono, &remaining);
    if (tx_n[0] == 0 && remaining > 0) {
        printf("[PLAYER] Read error\r\n");
        err = true;
    }

    int cur = 0;
    while (tx_n[cur] > 0 && g_state == PLAYER_PLAYING) {
        /* 1. 启动当前块 DMA */
        if (!i2s_start_tx(g_pcm_buf[cur], tx_n[cur])) {
            printf("[PLAYER] I2S TX failed\r\n");
            err = true;
            break;
        }

        /* 2. 当前块播放期间, 预读下一块到另一缓冲 */
        int nxt = 1 - cur;
        tx_n[nxt] = fill_block(fp, g_pcm_buf[nxt], mono, &remaining);
        if (tx_n[nxt] == 0 && remaining > 0) {
            printf("[PLAYER] Read error\r\n");
            err = true;
            break;
        }

        /* 3. 等当前块全部送入 FIFO */
        if (!i2s_wait_complete(500)) {
            printf("[PLAYER] I2S timeout\r\n");
            err = true;
            break;
        }

        /* 更新播放进度 (毫秒) */
        g_elapsed_ms = (uint32_t)((uint64_t)(data_size - remaining) * 1000u / bytes_per_sec);

        /* 暂停: 在块边界生效 */
        while (g_paused && g_state == PLAYER_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (g_state != PLAYER_PLAYING) break;

        /* 4. 切到下一块 (已预读完成, 立即启动 → 无下溢) */
        cur = nxt;
    }

    vTaskDelay(pdMS_TO_TICKS(2));   /* 让 FIFO 中最后 ~1ms 尾音播完 */
    i2s_stop();
    sd_card_fclose(fp);
    g_active_fp = NULL;

    const char *reason;
    if (g_state != PLAYER_PLAYING)      reason = "stopped";
    else if (err && remaining > 0)      reason = "error (mid-stream)";
    else if (err)                       reason = "error (start)";
    else                                reason = "end of file";

    bool ok = (g_state == PLAYER_PLAYING) && !err;
    g_state = PLAYER_IDLE;
    g_total_ms = 0;
    g_elapsed_ms = 0;
    printf("[PLAYER] %s (%s, %lu B left, %uHz %u-ch)\r\n",
           ok ? "Done" : "Stopped", reason,
           (unsigned long)remaining, (unsigned)sample_rate, (unsigned)channels);
    return ok;
}

void audio_player_stop(void)
{
    if (g_state == PLAYER_PLAYING) {
        g_state = PLAYER_STOPPING;
    }
}

void audio_player_pause(void)
{
    if (g_state == PLAYER_PLAYING) {
        g_paused = true;
    }
}

void audio_player_resume(void)
{
    g_paused = false;
}

bool audio_player_get_progress(uint32_t *elapsed_ms, uint32_t *total_ms)
{
    if (g_total_ms == 0) return false;
    *elapsed_ms = g_elapsed_ms;
    *total_ms = g_total_ms;
    return true;
}

player_state_t audio_player_get_state(void) { return g_state; }

void audio_player_force_close(void)
{
    /* 复位 I2S: 若播放任务在 DTC DMA 传输中崩溃, SSI/DTC 可能处于半开状态,
     * 下次播放 write 会返回 FSP_ERR_IN_USE。stop+close+reopen 恢复干净状态。 */
    i2s_stop();

    if (g_active_fp) {
        sd_card_fclose(g_active_fp);
        g_active_fp = NULL;
    }
    g_state      = PLAYER_IDLE;
    g_paused     = false;
    g_total_ms   = 0;
    g_elapsed_ms = 0;
    printf("[PLAYER] Force closed (recovery)\r\n");
}
