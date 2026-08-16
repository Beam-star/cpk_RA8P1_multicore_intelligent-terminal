/**
 ******************************************************************************
 * @file    lvgl_ui_tracking.c
 * @brief   舵机追踪引擎 — 人脸追踪（PID）+ 声源追踪（比例/EMA）
 *
 * 控制回路（25ms 周期）：
 *   - 人脸追踪：error = 画面中心(320) - 人脸框中心X，PID 输出角度修正量，
 *     死区 + 积分限幅 + 每 tick 步长限速，让舵机转得缓慢平稳。
 *   - 声源追踪：取声源定位方位角（0=正前，顺时针），映射到 ±30° 水平角，
 *     EMA 平滑 + 死区 + 步长限速。
 *
 * 方向约定：角度为正 = 右转（pan right）。若实际舵机转向相反，改
 * SERVO_PAN_INVERT。
 ******************************************************************************
 */

#include "lvgl_ui_tracking.h"
#include "driver/scs_servo/scs_servo.h"
#include "lvgl_ui_page3.h"
#include "ai_application/face_detection_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ========================================================================
 * 可调参数（调试时改这里）
 * ======================================================================== */

/* ---- 任务参数 ---- */
#define TRACK_TASK_PRIO        1
#define TRACK_TASK_STACK       1024
#define TRACK_PERIOD_MS        25         /* 控制周期，越小响应越快 */

/* ---- 舵机运动（角度 + 速度）---- */
#define SERVO_LIMIT_DEG        35.0f      /* 【转动角度】左右最大角度（度），改这里 */
#define SERVO_SPEED            150        /* 【转动速度】舵机运行速度，越大越快（0=最快） */
#define SERVO_ACC              40         /* 加速度，越大起步越快 */
#define SERVO_MAX_STEP_DEG     2.0f       /* 每 tick 最大步长（软件限速，越大越跟手） */
#define SERVO_POS_DEADBAND     0.5f       /* 角度变化小于该值不重发写位置（防抖） */
#define SERVO_PAN_INVERT       0          /* 舵机转向相反时置 1 */

/* ---- 人脸 PID ---- */
#define FACE_KP                0.035f     /* 比例：度/像素误差 */
#define FACE_KI                0.002f     /* 积分：度/(像素·秒) */
#define FACE_KD                0.014f     /* 微分：度/(像素/秒) */
#define FACE_DEADBAND_PX       8          /* 误差死区（像素），人脸在中心±该值内不转 */
#define FACE_MAX_INTEGRAL      15.0f      /* 积分限幅，防 windup */

/* ---- 声源追踪 ---- */
#define SOUND_EMA_ALPHA        0.25f      /* 方位角 EMA 平滑系数（越大越跟手） */
#define SOUND_DEADBAND_DEG     3.0f       /* 方位死区（度），声源在正前方±该值内不转 */
#define SOUND_MIN_INTENSITY    20         /* 低于该强度视为无声源 */

/* ======================================================================== */
/*  状态                                                                     */
/* ======================================================================== */

static volatile bool    g_face_on       = false;
static volatile bool    g_sound_on      = false;
static volatile uint8_t g_target_person = 0;   /* 0=person1 */

/* 任务上下文（仅 tracking_task 访问） */
static bool  g_servo_ready     = false;
static float g_angle           = 0.0f;   /* 当前舵机角度（度） */
static float g_last_sent_angle = 0.0f;

static float g_face_integral   = 0.0f;
static float g_face_prev_err   = 0.0f;
static bool  g_face_prev_valid = false;
static bool  g_was_face_active = false;

static float g_sound_smooth    = 0.0f;
static bool  g_sound_valid     = false;

/* ======================================================================== */
/*  工具                                                                     */
/* ======================================================================== */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* 角度(度) → 舵机位置(0..4095)，中位 2048 = 0° */
static uint16_t angle_to_pos(float deg)
{
    int32_t pos = (int32_t)SCS_STS_POS_CENTER + (int32_t)(deg * SCS_STS_POS_PER_DEG);
    if (pos < 0)    pos = 0;
    if (pos > 4095) pos = 4095;
    return (uint16_t)pos;
}

/* 以限速方式朝目标角度逼近，并下发到舵机 */
static void move_toward(float target)
{
    target = clampf(target, -SERVO_LIMIT_DEG, SERVO_LIMIT_DEG);

    float diff = target - g_angle;
    if (diff >  SERVO_MAX_STEP_DEG) diff =  SERVO_MAX_STEP_DEG;
    if (diff < -SERVO_MAX_STEP_DEG) diff = -SERVO_MAX_STEP_DEG;
    g_angle += diff;

    /* 角度变化太小则不重发，减少 UART 流量与舵机抖动 */
    float dd = g_angle - g_last_sent_angle;
    if (dd < 0) dd = -dd;
    if (dd < SERVO_POS_DEADBAND) return;

    g_last_sent_angle = g_angle;
    scs_servo_write_pos(angle_to_pos(g_angle), SERVO_SPEED, SERVO_ACC);
}

/* 找到指定人脸的框中心 X（相机空间 640×480）。 */
static bool find_person_center(uint8_t person_idx, float *cx)
{
    uint32_t count = g_face_detection_count;
    if (count == 0) return false;
    if (count > AI_MAX_DETECTION_NUM) count = AI_MAX_DETECTION_NUM;

    for (uint32_t i = 0; i < count; i++) {
        face_detect_result_t *r = &g_face_detection_results[i];
        if (r->id == (int16_t)person_idx && r->w > 0 && r->h > 0) {
            *cx = (float)r->x + (float)r->w * 0.5f;
            return true;
        }
    }
    return false;
}

static void reset_pid(void)
{
    g_face_integral   = 0.0f;
    g_face_prev_err   = 0.0f;
    g_face_prev_valid = false;
}

/* ======================================================================== */
/*  人脸追踪（PID）                                                          */
/* ======================================================================== */

static void face_track_tick(void)
{
    float cx;
    if (!find_person_center(g_target_person, &cx)) {
        reset_pid();      /* 目标人脸丢失：保持当前位置不动 */
        return;
    }

    /* error > 0 = 人脸在画面左侧 → 应左转（角度减小，默认方向） */
    float err = 320.0f - cx;

    if (err >  FACE_DEADBAND_PX) err -= FACE_DEADBAND_PX;
    else if (err < -FACE_DEADBAND_PX) err += FACE_DEADBAND_PX;
    else {
        reset_pid();      /* 死区内：保持 */
        return;
    }

    float dt = (float)TRACK_PERIOD_MS * 0.001f;

    g_face_integral += err * dt;
    g_face_integral  = clampf(g_face_integral, -FACE_MAX_INTEGRAL, FACE_MAX_INTEGRAL);

    float d = 0.0f;
    if (g_face_prev_valid) d = (err - g_face_prev_err) / dt;
    g_face_prev_err   = err;
    g_face_prev_valid = true;

    float pid = FACE_KP * err + FACE_KI * g_face_integral + FACE_KD * d;

    /* err>0(偏左) → 左转 → 角度减小；SERVO_PAN_INVERT 翻转该方向 */
    float step = (SERVO_PAN_INVERT ? +1.0f : -1.0f) * pid;
    move_toward(g_angle + step);
}

/* ======================================================================== */
/*  声源追踪（比例 + EMA）                                                   */
/* ======================================================================== */

static void sound_track_tick(void)
{
    int16_t az;
    uint8_t intensity;

    if (!lvgl_ui_page3_get_direction(&az, &intensity) ||
        intensity < SOUND_MIN_INTENSITY) {
        g_sound_valid = false;
        return;          /* 无声源：保持 */
    }

    /* 方位角 0..360（0=正前，顺时针）→ 有符号误差 [-180,180]，正=右 */
    float err = (float)az;
    if (err > 180.0f) err -= 360.0f;

    /* 限幅到 ±30° */
    err = clampf(err, -SERVO_LIMIT_DEG, SERVO_LIMIT_DEG);

    if (!g_sound_valid) {
        g_sound_smooth = err;
        g_sound_valid  = true;
    } else {
        g_sound_smooth += SOUND_EMA_ALPHA * (err - g_sound_smooth);
    }

    /* 死区：接近正前方时不再微调，避免抖动 */
    if (g_sound_smooth > -SOUND_DEADBAND_DEG && g_sound_smooth < SOUND_DEADBAND_DEG) {
        move_toward(0.0f);
    } else {
        move_toward(g_sound_smooth);
    }
}

/* ======================================================================== */
/*  主循环                                                                   */
/* ======================================================================== */

static void tracking_tick(void)
{
    bool face_mode    = (face_detection_get_mode() == DETECTION_MODE_FACE);
    bool face_active  = g_face_on && face_mode;
    bool sound_active = g_sound_on;

    /* 人脸追踪由 Face 模式切入 Hand 时已在上层自动关闭，这里再兜底一次 */
    if (g_face_on && !face_mode) {
        face_active = false;
    }

    /* 追踪使能边沿：进入时复位 PID */
    if (face_active != g_was_face_active) {
        g_was_face_active = face_active;
        reset_pid();
    }

    if (!g_servo_ready) {
        return;          /* 舵机未初始化：只更新状态，不驱动 */
    }

    if (face_active) {
        face_track_tick();
    } else if (sound_active) {
        sound_track_tick();
    } else {
        /* 无追踪：缓慢回中位 */
        reset_pid();
        move_toward(0.0f);
    }
}

static void tracking_task(void *pv)
{
    (void)pv;

    /* 舵机初始化放任务内：阻塞读带超时，舵机未接也不会卡死启动 */
    g_servo_ready = scs_servo_init();
    g_angle       = 0.0f;
    g_last_sent_angle = g_angle;

    while (1) {
        tracking_tick();
        vTaskDelay(pdMS_TO_TICKS(TRACK_PERIOD_MS));
    }
}

/* ======================================================================== */
/*  对外 API                                                                 */
/* ======================================================================== */

void lvgl_ui_tracking_init(void)
{
    if (xTaskCreate(tracking_task, "servo_track", TRACK_TASK_STACK, NULL,
                    TRACK_TASK_PRIO, NULL) != pdPASS) {
        printf("[TRACK] tracking task create failed\r\n");
        return;
    }
    printf("[TRACK] tracking task started\r\n");
}

void lvgl_ui_tracking_face_set_on(bool on)
{
    g_face_on = on;
}

bool lvgl_ui_tracking_face_is_on(void)
{
    return g_face_on;
}

void lvgl_ui_tracking_sound_set_on(bool on)
{
    g_sound_on = on;
}

bool lvgl_ui_tracking_sound_is_on(void)
{
    return g_sound_on;
}

void lvgl_ui_tracking_set_target_person(uint8_t idx)
{
    if (idx > 2) idx = 2;
    g_target_person = idx;
}

uint8_t lvgl_ui_tracking_get_target_person(void)
{
    return g_target_person;
}
