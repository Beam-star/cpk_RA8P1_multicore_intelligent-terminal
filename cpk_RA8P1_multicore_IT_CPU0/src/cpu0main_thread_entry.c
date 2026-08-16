/**
 ******************************************************************************
 * @file    cpu0main_thread_entry.c
 * @brief   CPU0 (Cortex-M85) main thread �?system startup and mode dispatch
 *
 * Three operating modes, selected by OPERATING_MODE:
 *
 *   MODE_PCDC_ECHO  USB virtual COM port echo test.  No display, no camera,
 *                   no AI.  LED blinks slowly.  PCDC thread handles USB.
 *                   Use this mode to run pcdc_flash_tool.py for W25Q256 ops.
 *
 *   MODE_LVGL_DEMO  Boot animation �?LVGL keypad-encoder music demo.
 *                   GLCDC 1024×600, GT911 touch, W25Q256 FatFS/XIP.
 *
 *   MODE_CAMERA     Camera preview on LCD 1024×600 + optional AI face
 *                   detection (Ethos-U55 NPU).  Double-buffered GLCDC
 *                   output with CPU-rendered bounding boxes.
 ******************************************************************************
 */

#include <cpu0main_thread.h>
#include <stdio.h>
#include <console.h>
#include "perf_counter/perf_counter.h"
#include "common_data.h"       /* GLCDC_CFG_LAYER_2_ENABLE, g_lvgl_port_cfg, g_lvgl_port_ctrl */
#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_log.h"
#include "rpmsg_record_cpu0.h"
#include "sdram.h"
#include "w25q256.h"
#include "w25q256_test.h"
#include "mipi_camera_test.h"
#include "mipi_camera_lcd.h"
#include "rgblcd.h"
#include "gt911.h"
#include "gt911_test.h"
#include "lvgl_ui/lvgl_ui_main.h"
#include "lvgl_ui/lvgl_ui_boot_anim.h"
#include "lvgl_ui/boot_logo.h"
#include "lvgl_ui/myChineseFont.h"
#include "driver/micarray/micarray_driver.h"
#include "driver/esp32/esp32_uart.h"
#include "driver/ci1302/ci1302_uart.h"
#include "driver/scs_servo/scs_servo.h"
#include "driver/zw111/zw111_fingerprint.h"
#include "driver/zw111/fp_name_db.h"
#include "rm_lvgl_port.h"
#include "ai_application/face_detection_build_mode.h"

/* ---- Test function declarations ---- */
extern int  rpmsg_test_run(void);
extern int  rpmsg_nocopy_test_run(void);
extern int  rpmsg_bulk_test_run(void);
extern int  sdram_test_run(void);
extern void rgblcd_test_run(void);
extern void lv_demo_keypad_encoder(void);

/* ======================================================================== */
/*  OPERATING MODE SELECTION �?change this ONE macro                        */
/* ======================================================================== */

#define MODE_PCDC_ECHO    1   /* USB virtual COM port echo, no display        */
#define MODE_LVGL_DEMO    2   /* Boot animation + LVGL music demo             */
#define MODE_CAMERA       3   /* Camera preview + optional face detection     */
#define MODE_RGBLCD_TEST  4   /* Pure hardware LCD test (no LVGL)             */

#define OPERATING_MODE  MODE_CAMERA        /* <-- CHANGE HERE */

/* ======================================================================== */
/*  Global feature flags                                                      */
/* ======================================================================== */

/*
 * CAMERA_ENABLE: master switch for all camera-related functionality.
 *   = 1 → OV5645 init, LCD layer 1 camera preview, boot logo, face DB
 *   = 0 → LVGL UI on layer 2 only (buttons, log, status bar)
 *
 * When disabled, the following are skipped:
 *   - OV5645 MIPI CSI camera init
 *   - Camera preview on GLCDC layer 1
 *   - Boot logo (uses layer 1 from W25Q256)
 *   - Face database / face embedding model (depends on camera input)
 *   - lvgl_port_vpos_cb (camera vsync dependency)
 */
#define CAMERA_ENABLE  1   /* <-- 1=full camera, 0=LVGL UI only */

/*
 * SERVO_TEST_MODE: 舵机摆动测试开关（调试用）。
 *   = 1 → 跳过相机/LVGL/追踪，只运行 scs_servo_test_run() 让舵机 ±30° 摆动。
 *   = 0 → 正常运行。
 */
#define SERVO_TEST_MODE  0   /* <-- 调试舵机时设为 1 */

/*
 * PCDC mode flag �?read by pcdc_thread_entry() to decide whether to
 * initialize USB or self-delete.  true only in MODE_PCDC_ECHO.
 */
#if OPERATING_MODE == MODE_PCDC_ECHO
bool g_pcdc_echo_mode = true;
#else
bool g_pcdc_echo_mode = false;
#endif


/* ======================================================================== */
/*  Per-mode sub-options                                                     */
/* ======================================================================== */

#if OPERATING_MODE == MODE_CAMERA
    /* AI face detection needs both CAMERA_ENABLE and working flash/NPU */
    #if CAMERA_ENABLE
        #define FACE_DETECTION_ENABLE  1   /* Ethos-U55 NPU face detection */
    #else
        #define FACE_DETECTION_ENABLE  0
    #endif
#elif OPERATING_MODE == MODE_LVGL_DEMO
    /* No sub-options �?boot animation always plays, demo always launches.   */
#elif OPERATING_MODE == MODE_PCDC_ECHO
    /* No sub-options �?PCDC thread handles everything autonomously.        */
#endif


/* ======================================================================== */
/*  Boot animation done callback (LVGL Demo mode only)                       */
/* ======================================================================== */

#if OPERATING_MODE == MODE_LVGL_DEMO
static void boot_anim_done_cb(void)
{
    printf("[MAIN] Boot animation finished ?launching Music Demo\r\n");
    lv_demo_keypad_encoder();
}
#endif


/* ======================================================================== */
/*  Helium / ITCM helpers (unused in production, kept for reference)         */
/* ======================================================================== */

#if !defined(__CODE_AREA) || (__CODE_AREA == CODE_ITCM)
#define CODE_AREA               __attribute__((section(".itcm_code_from_flash")))
#elif __CODE_AREA == CODE_SRAM
#define CODE_AREA               __attribute__((section(".ram_code_from_flash")))
#else
#define CODE_AREA
#endif

CODE_AREA void vector_mac_scalar(const float*,const float*,const float*,float*,uint32_t);
CODE_AREA void vector_mac_helium(const float*,const float*,const float*,float*,uint32_t);

#define ALIGN16 __attribute__((aligned(16)))
#define ARRAY_LEN 2048

/* Helium vector test arrays (commented out �?keep for testing)
//ALIGN16 float a[ARRAY_LEN];
//ALIGN16 float b[ARRAY_LEN];
//ALIGN16 float c[ARRAY_LEN];
//ALIGN16 float y_scalar[ARRAY_LEN];
//ALIGN16 float y_helium[ARRAY_LEN];
*/

int my_var __attribute__((section(".data.my_init_data"))) = 0x1234;

CODE_AREA
static void pinToggleTest(int num)
{
    bsp_io_level_t pin_level;
    int i;
    for (i = 0; i < num; i++) {
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_HIGH);
        R_IOPORT_PinRead(g_ioport.p_ctrl, USER_LED, &pin_level);
        while (pin_level != BSP_IO_LEVEL_HIGH) {
            R_IOPORT_PinRead(g_ioport.p_ctrl, USER_LED, &pin_level);
        }
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_LOW);
        R_IOPORT_PinRead(g_ioport.p_ctrl, USER_LED, &pin_level);
        while (pin_level != BSP_IO_LEVEL_LOW) {
            R_IOPORT_PinRead(g_ioport.p_ctrl, USER_LED, &pin_level);
        }
    }
}


/* ======================================================================== */
/*  Main thread entry                                                        */
/* ======================================================================== */

void cpu0main_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    /* ---- Common boot sequence (all modes) ---- */
    R_BSP_SecondaryCoreStart();     /* Launch CPU1                           */
    CONSOLE_Init();                 /* SEGGER RTT console init              */
    perfc_init(false);              /* Performance counter                  */
    rpmsg_core_init();              /* RPMsg-Lite (disables D-Cache)        */
    rpmsg_log_cpu0_init();          /* CPU1 �?CPU0 log relay channel        */
    rpmsg_record_cpu0_init();       /* record-control channel �?CPU1        */

    /* ---- RPMsg tests (commented out �?keep for testing) ----
    //rpmsg_test_run();
    //rpmsg_nocopy_test_run();
    //rpmsg_bulk_test_run();
    */

    SCB_EnableDCache();             /* Re-enable D-Cache for performance    */

    /* ---- Helium vector MAC test (commented out �?keep for testing) ----
    //for (uint32_t i = 0; i < ARRAY_LEN; i++) {
    //    a[i] = 1.0f;
    //    b[i] = 2.0f;
    //    c[i] = 3.0f;
    //}
    //__cycleof__("Scalar_counter") {
    //    vector_mac_scalar(a, b, c, y_scalar, ARRAY_LEN);
    //}
    //__cycleof__("Helium_counter") {
    //    vector_mac_helium(a, b, c, y_helium, ARRAY_LEN);
    //}
    //int32_t error = 0;
    //for (uint32_t i = 0; i < ARRAY_LEN; i++) {
    //    if (y_scalar[i] != y_helium[i]) { error = 1; break; }
    //}
    //printf("cpu0 Result check: %s\r\n", error ? "MISMATCH" : "PASS");
    //
    //R_BSP_PinAccessEnable();
    //__cycleof__("PinToggle") {
    //    pinToggleTest(20000);
    //}
    //R_BSP_PinAccessDisable();
    */

/* ======================================================================== */
/*  MODE 1: PCDC Echo                                                        */
/* ======================================================================== */
#if OPERATING_MODE == MODE_PCDC_ECHO

    printf("\r\n[MAIN] === PCDC Echo Test Mode ===\r\n");
    printf("[MAIN] Connect USB cable, open COM port on PC.\r\n");
    printf("[MAIN] Serial echo is active; use pcdc_flash_tool.py for W25Q256 operations.\r\n");
    printf("[MAIN] To change mode, set OPERATING_MODE in cpu0main_thread_entry.c\r\n\r\n");

    /* PCDC thread (auto-started by FSP) handles USB and flash protocol.
     * Main thread just blinks LED to show we're alive. */
    while (1) {
        R_BSP_PinAccessEnable();
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_HIGH);
        vTaskDelay(500);
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_LOW);
        vTaskDelay(500);
        R_BSP_PinAccessDisable();
    }


/* ======================================================================== */
/*  MODE 2: LVGL Demo (boot animation + keypad encoder music demo)           */
/* ======================================================================== */
#elif OPERATING_MODE == MODE_LVGL_DEMO

    printf("\r\n[MAIN] === LVGL Boot Animation + Music Demo ===\r\n");

    /* 1. Backlight PWM */
    fsp_err_t bl_err = rgblcd_backlight_init();
    if (bl_err != FSP_SUCCESS) {
        printf("[MAIN] Backlight init failed: %ld\r\n", (long)bl_err);
    } else {
        rgblcd_backlight_set(80);
        printf("[MAIN] Backlight OK\r\n");
    }

    /* 2. LVGL + GLCDC port */
    lv_init();
    fsp_err_t port_err = RM_LVGL_PORT_Open(&g_lvgl_port_ctrl, &g_lvgl_port_cfg);
    if (port_err != FSP_SUCCESS) {
        printf("[MAIN] LVGL Port (GLCDC) failed: %ld\r\n", (long)port_err);
    } else {
        printf("[MAIN] LVGL Port (GLCDC) OK\r\n");
    }

    /* 3. GT911 touch controller */
    if (gt911_init()) {
        printf("[MAIN] GT911 touch OK\r\n");
    } else {
        printf("[MAIN] GT911 touch FAILED ?touch disabled\r\n");
    }

    /* 4. Open W25Q256 first (lvgl_ui_assets_load needs it for flash reads) */
    printf("[MAIN] Opening W25Q256...\r\n");
    w25q256_err_t flash_err = w25q256_open();
    printf("[MAIN] w25q256_open: %s\r\n", w25q256_err_str(flash_err));

    /* 5. LVGL UI (touch indev + timer task, loads assets from flash) */
    printf("[MAIN] Starting lvgl_ui_init...\r\n");
    lvgl_ui_init();
    printf("[MAIN] lvgl_ui_init done\r\n");

    /* 6. Play boot animation — callback launches keypad encoder demo */
    boot_anim_start(lv_screen_active(), boot_anim_done_cb);

    printf("[MAIN] Boot sequence complete\r\n");


/* ======================================================================== */
/*  MODE 3: Camera preview + optional face detection                         */
/* ======================================================================== */
#elif OPERATING_MODE == MODE_CAMERA

    printf("\r\n[MAIN] === Camera Mode ===\r\n");

#if SERVO_TEST_MODE
    /* 舵机摆动测试：跳过相机/LVGL/追踪，只测舵机 ±30° 摆动 */
    printf("[MAIN] === Servo Test Mode (skip camera) ===\r\n");
    scs_servo_test_run();
    while (1) {
        vTaskDelay(1000);
    }
#endif

    /* ----------------------------------------------------------------
     * 1.  Display + backlight
     *
     *   DUAL-LAYER  (GLCDC_CFG_LAYER_2_ENABLE = true):
     *     Layer 1 = camera (640×480 left-aligned), layer 2 = LVGL UI
     *     (384×600 right panel).  RM_LVGL_PORT_Open() owns GLCDC and
     *     routes LVGL flush() to layer 2.  The camera task writes
     *     layer 1 directly via R_GLCDC_BufferChange().
     *
     *     This code path also registers lvgl_port_vpos_cb (in mipi_camera_lcd.c)
     *     as an additional VPOS callback so the camera task's vsync-poll
     *     (g_frame_count) continues to work while RM_LVGL_PORT consumes
     *     the primary line-detect callback.
     *
     *   LEGACY single-layer  (GLCDC_CFG_LAYER_2_ENABLE = false):
     *     Same as before �?rgblcd_init() opens GLCDC directly, camera
     *     is centered, no LVGL display driver is attached (lv_init() +
     *     lvgl_ui_init() still run but LVGL has nothing to flush to).
     *     The boot logo is still shown on layer 1.
     * ---------------------------------------------------------------- */
#if GLCDC_CFG_LAYER_2_ENABLE
    /* ---- Dual-layer: backlight only; RM_LVGL_PORT opens GLCDC ---- */
    fsp_err_t lcd_err = rgblcd_backlight_init();
    if (lcd_err != FSP_SUCCESS) {
        printf("[MAIN] Backlight init failed: %ld\r\n", (long)lcd_err);
    } else {
        rgblcd_backlight_set(80);
        printf("[MAIN] Backlight OK (dual-layer mode)\r\n");

        /* LVGL init before opening the port */
        lv_init();
        printf("[MAIN] LVGL memory init OK\r\n");

        /* Open LVGL port (vpos callback only needed when camera is active) */
    #if CAMERA_ENABLE
        rm_lvgl_port_cfg_t local_lvgl_cfg = g_lvgl_port_cfg;
        local_lvgl_cfg.p_callback = lvgl_port_vpos_cb;
        fsp_err_t lvgl_err = RM_LVGL_PORT_Open(&g_lvgl_port_ctrl, &local_lvgl_cfg);
    #else
        fsp_err_t lvgl_err = RM_LVGL_PORT_Open(&g_lvgl_port_ctrl, &g_lvgl_port_cfg);
    #endif
        if (lvgl_err != FSP_SUCCESS) {
            printf("[MAIN] RM_LVGL_PORT_Open failed: %ld\r\n", (long)lvgl_err);
        } else {
            printf("[MAIN] RM_LVGL_PORT_Open OK (LVGL on layer 2)\r\n");

            /* Touch and flash */
            printf("[MAIN] Initializing GT911 touch...\r\n");
            if (gt911_init()) {
                printf("[MAIN] GT911 touch OK\r\n");
            } else {
                printf("[MAIN] GT911 touch FAILED — touch disabled\r\n");
            }

            w25q256_open();
            printf("[MAIN] W25Q256 opened\r\n");
            {
                /* 一次性 JEDEC ID 验证 (不在 w25q256_open 里做, 以免破坏后续
                 * PCDC write 路径的 OSPI 控制器状态) */
                static bool s_jedec_done = false;
                if (!s_jedec_done) {
                    s_jedec_done = true;
                    w25q256_jedec_id_t id;
                    if (w25q256_read_jedec_id(&id) == W25Q256_OK)
                        printf("[W25Q256] JEDEC ID: %02X %02X %02X\r\n",
                               (unsigned)id.manufacturer, (unsigned)id.memory_type,
                               (unsigned)id.capacity);
                    else
                        printf("[W25Q256] JEDEC ID read FAILED\r\n");
                }
            }

            /* 从 W25Q256 加载中文字库到 SDRAM（供 LVGL 字幕显示用） */
            myChineseFont_load();
            printf("[MAIN] Chinese font loaded\r\n");

    #if CAMERA_ENABLE
            /* Camera path: boot logo on layer1, then LVGL UI + camera.
             * The detection task loads its own models (face + hand) from
             * W25Q256 when started — no face DB / embedding model here. */
            boot_logo_show_layer1(2000);

            lvgl_ui_init();

            /* 声源定位：依据 MICARRAY_TEST_MODE 选择测试(模拟)或真实(UART 热力图)模式 */
            micarray_init();

            /* ESP32 语音转文字 / AI 对话（UART2，字幕回调已在 lvgl_ui_init 内注册） */
            esp32_uart_init();

            /* CI1302 离线语音识别声控模块（UART0 / SCI0，命令回调已在 lvgl_ui_init 内注册） */
            ci1302_uart_init();

            /* ZW111 指纹模块（UART6 / SCI6，完成回调已在 lvgl_ui_init 内注册） */
            zw111_fingerprint_init();

            /* 指纹 ID ↔ 名字映射库（W25Q256 @0xBD0000，录入保存/打卡查询/清库擦除） */
            fp_name_db_init();

            mipi_camera_lcd_start(false, (bool)FACE_DETECTION_ENABLE);

        #if FACE_DETECTION_ENABLE
            printf("[MAIN] Face detection: ENABLED\r\n");
        #else
            printf("[MAIN] Face detection: disabled\r\n");
        #endif
    #else
            /* No camera: clear both layers to black so they don't show SDRAM garbage.
             * Layer 1 (fb_background): camera preview area, 1024×600
             * Layer 2 (fb_foreground): LVGL panel, 384×600 — MUST be cleared
             *   because D/AVE 2D only renders dirty areas; un-cleared SDRAM
             *   random data would bleed through as visual artefacts. */
//            printf("[MAIN] Camera DISABLED — clearing framebuffers...\r\n");
//            const size_t sz0 = (size_t)DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * DISPLAY_VSIZE_INPUT0;
//            const size_t sz1 = (size_t)DISPLAY_BUFFER_STRIDE_BYTES_INPUT1 * DISPLAY_VSIZE_INPUT1;
//            memset(fb_background[0], 0x00, sz0);
//            memset(fb_background[1], 0x00, sz0);
//            memset(fb_foreground[0], 0x00, sz1);
//            memset(fb_foreground[1], 0x00, sz1);
//            SCB_CleanDCache_by_Addr((volatile void *)fb_background[0], (int32_t)sz0);
//            SCB_CleanDCache_by_Addr((volatile void *)fb_background[1], (int32_t)sz0);
//            SCB_CleanDCache_by_Addr((volatile void *)fb_foreground[0], (int32_t)sz1);
//            SCB_CleanDCache_by_Addr((volatile void *)fb_foreground[1], (int32_t)sz1);
            lvgl_ui_init();
    #endif
        }
    }
#else
    /* ---- Single-layer: LVGL on Layer 1 (full screen 1024x600) ---- */
    fsp_err_t bl_err = rgblcd_backlight_init();
    if (bl_err != FSP_SUCCESS) {
        printf("[MAIN] Backlight init failed: %ld\r\n", (long)bl_err);
    } else {
        rgblcd_backlight_set(80);
        printf("[MAIN] Backlight OK (single-layer)\r\n");

        /* LVGL memory init before opening port */
        lv_init();
        printf("[MAIN] LVGL mem init OK\r\n");

        /* Open LVGL port: inherits Layer 1, fb_background, 1024x600 */
        fsp_err_t lvgl_err = RM_LVGL_PORT_Open(&g_lvgl_port_ctrl, &g_lvgl_port_cfg);
        if (lvgl_err != FSP_SUCCESS) {
            printf("[MAIN] RM_LVGL_PORT_Open failed: %ld\r\n", (long)lvgl_err);
        } else {
            printf("[MAIN] GLCDC opened (single-layer, LVGL on Layer1)\r\n");

            /* GT911 touch */
            printf("[MAIN] Initializing GT911 touch...\r\n");
            if (gt911_init()) {
                printf("[MAIN] GT911 touch OK\r\n");
            } else {
                printf("[MAIN] GT911 touch FAILED — touch disabled\r\n");
            }

            w25q256_open();
            printf("[MAIN] W25Q256 opened\r\n");

        #if CAMERA_ENABLE
            boot_logo_show_layer1(2000);
        #endif

            lvgl_ui_init();
            printf("[MAIN] LVGL UI init OK\r\n");

        #if CAMERA_ENABLE
            mipi_camera_lcd_start(false, (bool)FACE_DETECTION_ENABLE);
            #if FACE_DETECTION_ENABLE
            printf("[MAIN] Face detection: ENABLED\r\n");
            #else
            printf("[MAIN] Face detection: disabled\r\n");
            #endif
        #else
            printf("[MAIN] Camera DISABLED — LVGL UI only (Layer1 full-screen)\r\n");
        #endif
        }
    }
#endif /* GLCDC_CFG_LAYER_2_ENABLE */

/* ======================================================================== */
/*  MODE 4: Pure RGBLCD hardware test (no LVGL, no camera)                    */
/* ======================================================================== */
#elif OPERATING_MODE == MODE_RGBLCD_TEST

    printf("\r\n[MAIN] === RGBLCD Single-Layer Test Mode ===\r\n");

    /* Backlight + direct RGB test (no LVGL, no Layer2) */
    fsp_err_t bl_err = rgblcd_backlight_init();
    if (bl_err != FSP_SUCCESS)
        printf("[MAIN] Backlight failed: %ld\r\n", (long)bl_err);
    else
        rgblcd_test_run();

    printf("[MAIN] Test done. If no tripling -> Layer1 HW is fine.\r\n");

    /* 5. Idle */
    while (1) {
        vTaskDelay(1000);
    }

#endif /* OPERATING_MODE */


    /* ---- Standalone tests (commented out �?uncomment to run) ---- */
    //sdram_test_run();
    //w25q256_test_run();
    //w25q256_test_speed();
    //rgblcd_test_run();
    //mipi_camera_test_start_ex(false, false);

    printf("[MAIN] my custom section data: %d\r\n", my_var);

    /* ---- Idle loop: heartbeat LED ---- */
    while (1) {
        R_BSP_PinAccessEnable();
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_HIGH);
        vTaskDelay(1000);
        R_IOPORT_PinWrite(g_ioport.p_ctrl, USER_LED, BSP_IO_LEVEL_LOW);
        vTaskDelay(1000);
        R_BSP_PinAccessDisable();
    }
}
