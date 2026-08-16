/**
 ******************************************************************************
 * @file    lvgl_ui_page3.c
 * @brief   UI 第三页 — 声源定位粒子波动雷达 (LVGL 9.3)
 *
 * 实现思路：
 *   - 一个固定圆圈，圆心即麦克风阵列中心。
 *   - 声源方向 angle 映射到圆周上的方位（0 = 正前方/屏幕上方，顺时针）。
 *   - 粒子在声源方位处从圆周边缘生成，向内漂移并逐渐变暗（消散）。
 *   - 粒子颜色用"热力图"渐变（黑→红→黄→白），亮度随强度衰减。
 *
 * 绘制采用 LVGL 自定义 draw 回调（LV_EVENT_DRAW_POST），不创建大量子对象，
 * 内存开销小；粒子状态保存在一个小的静态数组里。
 ******************************************************************************
 */

#include "lvgl_ui_page3.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"
#include "arm_math.h"
#include <string.h>

/* ======================================================================== */
/*  Geometry / constants                                                     */
/* ======================================================================== */

/*
P3_FAN_HALF	    30	扇形半角（度），越大扇形越宽
P3_FADE_STEP	12	每帧衰减，越大消散越快、拖尾越短
P3_DRIFT	    1.5f	每帧向内漂移 px，越大波纹越急
P3_SPAWN_N	    6	每帧生成粒子数，越多扇形越密
P3_CIRCLE_R	    130	圆圈半径
*/

#define P3_W            384
#define P3_H            600

#define P3_TITLE_H      56
#define P3_RADAR_SIZE   280     /* 雷达正方形边长 */
#define P3_CIRCLE_R     130     /* 圆圈半径 */
#define P3_RADAR_Y      84      /* 雷达区域顶部 y */

#define P3_MAX_PARTICLES  96     /* 粒子上限。播放音乐时扬声器持续发声 → 声源持续
                                  * 有粒子，若上限过高，每帧 lv_draw_fill 次数太多，
                                  * 切页动画时 D/AVE 2D 命令缓冲/绘制负载翻倍 → 跑飞 */
#define P3_FADE_STEP      6      /* 每帧强度衰减（越小消散越慢、向内走得越深） */
#define P3_DRIFT          2.2f   /* 每帧向内漂移 (px) */
#define P3_SPAWN_N        6      /* 每帧在扇形内生成的粒子数 */
#define P3_FAN_HALF       30     /* 扇形半角（度） */
#define P3_DEG2RAD        (3.14159265f / 180.0f)

#define C_BG       0x0D1B2A   /* 背景渐变顶部（深蓝） */
#define C_BG_GRAD  0x1B0F2E   /* 背景渐变底部（深紫） */
#define C_TITLE_BG 0x0A1220
#define C_TEXT     0xFFFFFF
#define C_ACCENT   0x4FC3F7

#define UI_FONT (&lv_font_montserrat_16)

/* ======================================================================== */
/*  Particle                                                                 */
/* ======================================================================== */

typedef struct {
    float   angle;      /* 弧度，0 = 正前方(上)，顺时针 */
    float   radius;     /* 距圆心距离 px */
    int16_t intensity;  /* 当前亮度 0..255；<=0 表示空闲槽位 */
} p3_particle_t;

/* 简单 LCG 伪随机数（避免依赖 libc rand()） */
static uint32_t g_rand_state = 12345u;
static uint32_t p3_rand(void)
{
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return g_rand_state;
}

/* ======================================================================== */
/*  Static state                                                             */
/* ======================================================================== */

static lv_obj_t *g_page3 = NULL;
static lv_obj_t *g_radar = NULL;
static lv_obj_t *g_angle_label = NULL;
static lv_obj_t *g_level_label = NULL;

static p3_particle_t g_particles[P3_MAX_PARTICLES];
static lv_timer_t    *g_timer = NULL;

/* 由外部(串口解析任务)或测试模式写入，LVGL 定时器读取 */
static volatile int32_t  g_target_angle = -1;   /* -1 = 无声源 */
static volatile uint32_t g_target_intensity = 0;
static volatile bool     g_test_running = false;

/* 测试模式内部状态 (仅在 LVGL 定时器上下文访问) */
static int32_t g_test_angle = 0;

/* ======================================================================== */
/*  Helpers                                                                  */
/* ======================================================================== */

/* 热力图渐变：黑 -> 红 -> 黄 -> 白 */
static lv_color_t heat_color(uint8_t v)
{
    uint8_t r = 0, g = 0, b = 0;
    if (v < 64) {
        r = (uint8_t)(v * 4);            /* 黑 -> 红 */
    } else if (v < 128) {
        r = 255; g = (uint8_t)((v - 64) * 4);   /* 红 -> 黄 */
    } else if (v < 192) {
        r = 255; g = 255; b = (uint8_t)((v - 128) * 4); /* 黄 -> 白 */
    } else {
        r = 255; g = 255; b = 255;
    }
    return lv_color_make(r, g, b);
}

/* 角度(弧度, 0=上, 顺时针) -> 屏幕坐标 */
static void angle_to_pos(float ang, float radius, int cx, int cy, int *px, int *py)
{
    float s = arm_sin_f32(ang);
    float c = arm_cos_f32(ang);
    *px = cx + (int)(radius * s);
    *py = cy - (int)(radius * c);
}

/* 在声源方位生成若干粒子 */
static void spawn_particles(float ang, int intensity)
{
    int spawned = 0;
    for (int i = 0; i < P3_MAX_PARTICLES && spawned < P3_SPAWN_N; i++) {
        if (g_particles[i].intensity <= 0) {
            /* 扇形扩散：在声源方位 ±P3_FAN_HALF 范围内随机散布，
             * 形成以声源方向为中心的扇形，而非单点串行轨迹 */
            float t = (float)(p3_rand() & 0x7FFF) / 32768.0f;   /* 0..1 */
            float off = (t * 2.0f - 1.0f) * P3_FAN_HALF * P3_DEG2RAD;
            g_particles[i].angle     = ang + off;
            g_particles[i].radius    = (float)P3_CIRCLE_R;
            g_particles[i].intensity = (int16_t)intensity;
            spawned++;
        }
    }
}

/* 粒子老化：向内漂移 + 变暗 */
static void age_particles(void)
{
    for (int i = 0; i < P3_MAX_PARTICLES; i++) {
        p3_particle_t *p = &g_particles[i];
        if (p->intensity <= 0) continue;

        p->intensity -= P3_FADE_STEP;
        p->radius    -= P3_DRIFT;
        if (p->radius < 4.0f) p->radius = 4.0f;
        if (p->intensity < 0) p->intensity = 0;
    }
}

/* ======================================================================== */
/*  Custom draw (LV_EVENT_DRAW_POST)                                         */
/* ======================================================================== */

static void radar_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);

    /* 关键：DRAW 回调用的是屏幕绝对坐标，圆心必须取雷达对象的绝对中心，
     * 否则圆圈会画到屏幕 (P3_RADAR_SIZE/2, P3_RADAR_SIZE/2) 即左上角附近，
     * 超出雷达对象裁剪区，导致左上角部分圆圈被裁掉。 */
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t ca;
    lv_obj_get_coords(obj, &ca);
    const int cx = ca.x1 + (ca.x2 - ca.x1) / 2;
    const int cy = ca.y1 + (ca.y2 - ca.y1) / 2;

    /* ---- 圆圈 ---- */
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color       = lv_color_hex(C_ACCENT);
    arc.width       = 2;
    arc.radius      = P3_CIRCLE_R;
    arc.start_angle = 0;
    arc.end_angle   = 360;
    arc.rounded     = 0;
    arc.opa         = LV_OPA_60;
    arc.center.x    = cx;
    arc.center.y    = cy;
    lv_draw_arc(layer, &arc);

    /* ---- 中心十字 + 四个方位刻度 ---- */
    lv_draw_fill_dsc_t tick;
    lv_draw_fill_dsc_init(&tick);
    tick.color  = lv_color_hex(0x335577);
    tick.opa    = LV_OPA_COVER;
    tick.radius = LV_RADIUS_CIRCLE;
    lv_area_t ta;
    /* 中心点 */
    ta.x1 = cx - 3; ta.y1 = cy - 3; ta.x2 = cx + 3; ta.y2 = cy + 3;
    lv_draw_fill(layer, &tick, &ta);

    /* 四个方位小刻度 */
    const float tick_ang[4] = { 0.0f, 1.5708f, 3.1416f, 4.7124f }; /* 上右下左 */
    for (int i = 0; i < 4; i++) {
        int tx, ty;
        angle_to_pos(tick_ang[i], (float)P3_CIRCLE_R - 8, cx, cy, &tx, &ty);
        ta.x1 = tx - 2; ta.y1 = ty - 2; ta.x2 = tx + 2; ta.y2 = ty + 2;
        lv_draw_fill(layer, &tick, &ta);
    }

    /* ---- 粒子 ---- */
    for (int i = 0; i < P3_MAX_PARTICLES; i++) {
        p3_particle_t *p = &g_particles[i];
        if (p->intensity <= 0) continue;

        int px, py;
        angle_to_pos(p->angle, p->radius, cx, cy, &px, &py);

        int r = 2 + (p->intensity * 3) / 255;   /* 2..5 px */

        lv_draw_fill_dsc_t f;
        lv_draw_fill_dsc_init(&f);
        f.color  = heat_color((uint8_t)p->intensity);
        f.opa    = LV_OPA_COVER;
        f.radius = LV_RADIUS_CIRCLE;
        lv_area_t a;
        a.x1 = px - r; a.y1 = py - r; a.x2 = px + r; a.y2 = py + r;
        lv_draw_fill(layer, &f, &a);
    }
}

/* ======================================================================== */
/*  Update timer                                                             */
/* ======================================================================== */

static void update_labels(void)
{
    if (!g_angle_label || !g_level_label) return;

    char a[24], b[24];
    if (g_target_angle < 0) {
        lv_label_set_text(g_angle_label, "Dir: ---");
        lv_label_set_text(g_level_label, "Level: ---");
    } else {
        lv_snprintf(a, sizeof(a), "Dir: %d deg", (int)g_target_angle);
        lv_snprintf(b, sizeof(b), "Level: %u", (unsigned)g_target_intensity);
        lv_label_set_text(g_angle_label, a);
        lv_label_set_text(g_level_label, b);
    }
}

static void update_cb(lv_timer_t *t)
{
    (void)t;

    /* 页面不可见（含切页动画期间）时暂停粒子更新：避免后台无谓重绘，
     * 并减轻切页动画时新旧两页同时渲染的负载（连续音乐下粒子会一直满屏）。 */
    if (lv_screen_active() != g_page3) {
        return;
    }

    /* 测试模式：模拟缓慢旋转 + 脉动声源 */
    if (g_test_running) {
        g_test_angle += 1;          /* 慢速：约 33°/s，一整圈约 11s */
        if (g_test_angle >= 360) g_test_angle -= 360;
        float s = arm_sin_f32((float)g_test_angle * 3.14159265f / 180.0f);
        int32_t intensity = 180 + (int32_t)(70.0f * s);   /* 110..250 脉动 */
        g_target_angle     = g_test_angle;
        g_target_intensity = (uint32_t)intensity;
    }

    /* 在声源方位生成粒子 */
    if (g_target_angle >= 0 && g_target_intensity > 0) {
        float ang = (float)g_target_angle * 3.14159265f / 180.0f;
        spawn_particles(ang, (int)g_target_intensity);
    }

    /* 老化 + 重绘 */
    age_particles();
    update_labels();
    if (g_radar) lv_obj_invalidate(g_radar);
}

/* ======================================================================== */
/*  Page construction                                                        */
/* ======================================================================== */

void lvgl_ui_page3_init(void)
{
    g_page3 = lv_obj_create(NULL);
    lv_obj_set_size(g_page3, P3_W, P3_H);
    ui_apply_bg_gradient(g_page3);
    lv_obj_set_style_border_width(g_page3, 0, 0);

    /* ---- 标题栏 ---- */
    lv_obj_t *tbar = lv_obj_create(g_page3);
    lv_obj_set_size(tbar, P3_W, P3_TITLE_H);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_radius(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(tbar);
    lv_label_set_text(t, "Sound Localization");
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(t, &bangers_28, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);

    /* ---- 雷达绘制区 ---- */
    g_radar = lv_obj_create(g_page3);
    lv_obj_set_size(g_radar, P3_RADAR_SIZE, P3_RADAR_SIZE);
    lv_obj_set_pos(g_radar, (P3_W - P3_RADAR_SIZE) / 2, P3_RADAR_Y);
    lv_obj_set_style_bg_color(g_radar, lv_color_hex(0x05050C), 0);
    lv_obj_set_style_bg_opa(g_radar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_radar, 0, 0);
    lv_obj_set_style_radius(g_radar, 0, 0);
    lv_obj_clear_flag(g_radar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_radar, radar_draw_cb, LV_EVENT_DRAW_POST, NULL);

    /* ---- 状态标签 ---- */
    g_angle_label = lv_label_create(g_page3);
    lv_label_set_text(g_angle_label, "Dir: ---");
    lv_obj_set_style_text_color(g_angle_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_angle_label, UI_FONT, 0);
    lv_obj_align(g_angle_label, LV_ALIGN_TOP_LEFT, 16, P3_RADAR_Y + P3_RADAR_SIZE + 16);

    g_level_label = lv_label_create(g_page3);
    lv_label_set_text(g_level_label, "Level: ---");
    lv_obj_set_style_text_color(g_level_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_level_label, UI_FONT, 0);
    lv_obj_align(g_level_label, LV_ALIGN_TOP_RIGHT, -16, P3_RADAR_Y + P3_RADAR_SIZE + 16);

    lv_obj_t *hint = lv_label_create(g_page3);
    lv_label_set_text(hint, "0 = front (top), clockwise");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, UI_FONT, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, P3_RADAR_Y + P3_RADAR_SIZE + 48);

    /* ---- 粒子数组清零 ---- */
    memset(g_particles, 0, sizeof(g_particles));

    /* ---- 动画定时器 ---- */
    g_timer = lv_timer_create(update_cb, 30, NULL);
}

lv_obj_t *lvgl_ui_page3_get_screen(void)
{
    return g_page3;
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

void lvgl_ui_page3_set_direction(int16_t angle_deg, uint8_t intensity)
{
    /* 归一化到 0..359 */
    int32_t a = angle_deg;
    while (a < 0) a += 360;
    a %= 360;
    g_target_angle     = a;
    g_target_intensity = intensity;
}

/* 无 libm 依赖的 atan2(y,x)，返回 [-PI, PI] 弧度 */
static float p3_atan2(float y, float x)
{
    float ax = (x < 0) ? -x : x;
    float ay = (y < 0) ? -y : y;
    float a;
    if (ax < 1e-6f) {
        a = (y > 0) ? 1.5707963f : -1.5707963f;
    } else if (ay < ax) {
        float r = ay / ax;
        float r2 = r * r;
        a = r * (0.9998660f + r2 * (-0.3302995f + r2 * (0.1801410f
                + r2 * (-0.0851330f + r2 * 0.0208351f))));
    } else {
        float r = ax / ay;
        float r2 = r * r;
        a = 1.5707963f - r * (0.9998660f + r2 * (-0.3302995f + r2 * (0.1801410f
                + r2 * (-0.0851330f + r2 * 0.0208351f))));
    }
    if (x < 0) a = 3.14159265f - a;
    if (y < 0) a = -a;
    return a;
}

/* 简单峰值定位：热力图 16x16 -> 方向 + 强度。
 * 注意：栅格到物理角度的精确映射尚未标定，这里按"圆心=阵列中心、
 * 峰值相对中心的方向=声源方位"做近似映射。 */
void lvgl_ui_page3_set_heatmap(const uint8_t heatmap[16 * 16])
{
    int mx = 0, my = 0;
    uint8_t peak = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t v = heatmap[y * 16 + x];
            if (v > peak) { peak = v; mx = x; my = y; }
        }
    }
    if (peak == 0) {
        g_target_angle = -1;
        g_target_intensity = 0;
        return;
    }

    float dx = (float)(mx - 7.5f);
    float dy = (float)(my - 7.5f);
    /* atan2(dx, -dy): dy 向下 -> 后方(180), -dy 向上 -> 前方(0) */
    float ang = p3_atan2(dx, -dy) * 180.0f / 3.14159265f;
    while (ang < 0) ang += 360.0f;
    lvgl_ui_page3_set_direction((int16_t)ang, peak);
}

bool lvgl_ui_page3_get_direction(int16_t *angle_deg, uint8_t *intensity)
{
    if (g_target_angle < 0) {
        return false;
    }
    *angle_deg = (int16_t)g_target_angle;
    *intensity = (uint8_t)g_target_intensity;
    return true;
}

void lvgl_ui_page3_test_start(void)
{
    g_test_angle = 0;
    g_test_running = true;
}

void lvgl_ui_page3_test_stop(void)
{
    g_test_running = false;
    g_target_angle = -1;
    g_target_intensity = 0;
}

bool lvgl_ui_page3_test_is_running(void)
{
    return g_test_running;
}
