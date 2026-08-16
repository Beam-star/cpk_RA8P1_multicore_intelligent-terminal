/**
 ******************************************************************************
 * @file    mipi_camera.c
 * @brief   MIPI CSI Camera driver (OV5645) — FreeRTOS port
 * @note    Adapted from Titan OV5640 driver for CPK board OV5645 sensor
 *
 * OV5645 differences from OV5640:
 *   - Built-in 24MHz oscillator (no external XCLK required)
 *   - I2C address: 0x3C
 *   - Chip ID: 0x300A=0x56, 0x300B=0x45
 *   - Similar register layout with minor differences
 ******************************************************************************
 */

#include "mipi_camera.h"
#include "mipi_i2c.h"
#include "ov5645_regs.h"
#include "hal_data.h"
#include "common_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>

/* ======================================================================== */
/*  Internal State                                                            */
/* ======================================================================== */

static SemaphoreHandle_t g_cam_sem = NULL;
static volatile uint8_t *g_cam_buf_stored = NULL;

/* ======================================================================== */
/*  VIN 帧完成中断回调                                                         */
/* ======================================================================== */

void cam_vin_callback(capture_callback_args_t *p_args)
{
    BaseType_t woken = pdFALSE;

    if (p_args->event == VIN_EVENT_NOTIFY) {
        vin_interrupt_status_t status;
        status.mask = (uint32_t)p_args->interrupt_status;

        if (status.bits.frame_complete) {
            g_cam_buf_stored = p_args->p_buffer;
            if (g_cam_sem) {
                xSemaphoreGiveFromISR(g_cam_sem, &woken);
                portYIELD_FROM_ISR(woken);
            }
        }
    }
}

void cam_mipi_csi_callback(mipi_csi_callback_args_t *p_args)
{
    (void)p_args;
}

/* ======================================================================== */
/*  Internal Helpers                                                          */
/* ======================================================================== */

static fsp_err_t write_config_table(const ov5645_reg_t *table)
{
    while (table->reg_num != CONFIG_TABLE_END) {
        if (table->reg_num == REQUEST_WAIT) {
            vTaskDelay(pdMS_TO_TICKS(table->value));
        } else {
            if (!wrSensorReg16_8(table->reg_num, table->value)) {
                printf("[CAM] I2C write failed at 0x%04X\r\n", table->reg_num);
                return FSP_ERR_WRITE_FAILED;
            }
        }
        table++;
    }
    return FSP_SUCCESS;
}

static void ov5645_set_virtual_channel(uint32_t vchannel)
{
    uint8_t tmp;
    rdSensorReg16_8(0x4814, &tmp);
    tmp = (tmp & ~(3 << 6)) | (uint8_t)(vchannel << 6);
    wrSensorReg16_8(0x4814, tmp);
}

static void ov5645_stream_on(void)  { wrSensorReg16_8(0x4202, 0x00); }
static void ov5645_stream_off(void) { wrSensorReg16_8(0x4202, 0x0F); }

static void cam_pin_write(bsp_io_port_pin_t pin, bsp_io_level_t level)
{
    R_IOPORT_PinWrite(&g_ioport_ctrl, pin, level);
}

/* ======================================================================== */
/*  Public API                                                                */
/* ======================================================================== */

fsp_err_t mipi_camera_init(bool use_test_mode)
{
    fsp_err_t err;
    uint8_t pid_h = 0, pid_l = 0;

    if (g_cam_sem == NULL) {
        g_cam_sem = xSemaphoreCreateBinary();
        if (g_cam_sem == NULL) return FSP_ERR_OUT_OF_MEMORY;
    }

    printf("[CAM] Initializing OV5645 MIPI camera...\r\n");

    /*
     * 上电时序 (ATK-MCOV5645 模组):
     *   PWDN=H: 正常工作 (注意: 此模组 PWDN 极性与其他 OV5645 模组相反!)
     *   RST=L:  复位
     *   XCLK:   必须在 RESETB 释放前运行
     *
     * OV5645 datasheet 时序要求:
     *   t2: DVDD稳定→PWDN释放 (≥5ms)
     *   t3: PWDN释放→RESETB释放 (≥1ms)
     *   t4: RESETB释放→SCCB就绪 (≥20ms)
     */
    cam_pin_write(CAM_PWDN_PIN, BSP_IO_LEVEL_HIGH);  /* P710=H: 正常工作 (此模组极性) */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* XCLK on — 必须在 RESETB 释放前运行 */
    err = R_GPT_Open(&g_timer11_ctrl, &g_timer11_cfg);
    if (err != FSP_SUCCESS) {
        printf("[CAM] GPT11 (XCLK) open failed: %ld\r\n", (long)err);
        return err;
    }
    R_GPT_Enable(&g_timer11_ctrl);
    R_GPT_Start(&g_timer11_ctrl);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* RESET — low pulse ≥1ms, then release */
    cam_pin_write(CAM_RST_PIN, BSP_IO_LEVEL_LOW);
    vTaskDelay(pdMS_TO_TICKS(5));
    cam_pin_write(CAM_RST_PIN, BSP_IO_LEVEL_HIGH);
    vTaskDelay(pdMS_TO_TICKS(30));  /* t4: ≥20ms for SCCB ready */

    /* I2C init (shared IIC0 bus with GT911) */
    if (!mipi_i2c_init()) {
        printf("[CAM] I2C init failed\r\n");
        return FSP_ERR_NOT_OPEN;
    }

    /* 读取芯片 ID */
    for (int retry = 0; retry < 5; retry++) {
        rdSensorReg16_8(OV5645_CHIP_ID_H_REG, &pid_h);
        rdSensorReg16_8(OV5645_CHIP_ID_L_REG, &pid_l);
        printf("[CAM] PID: 0x%02X%02X\r\n", pid_h, pid_l);
        if (pid_h == OV5645_PID_H) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (pid_h != OV5645_PID_H) {
        printf("[CAM] ERROR: OV5645 not found (PID=0x%02X%02X)\r\n", pid_h, pid_l);
        return FSP_ERR_HW_LOCKED;
    }
    printf("[CAM] OV5645 detected (PID=0x%02X%02X)\r\n", pid_h, pid_l);

    /* 5. 写寄存器配置表 */
    err = write_config_table(ov5645_init_table);
    if (err != FSP_SUCCESS) return err;

    /*
     * 6. PLL 时钟配置 — 覆盖寄存器表中的默认值
     *    OV5645 与 OV5640 使用相同的 PLL 参数:
     *    XCLK=24MHz, pre-div=3, mult=140, VCO=1120MHz
     *    sys_div=1, mipi_div=2 → mipi_clk=560MHz (2-lane safe)
     */
    wrSensorReg16_8(0x3035, ((1 << 4) | 2));  /* sys_div=1, mipi_div=2 */
    wrSensorReg16_8(0x3036, 140);              /* PLL multiplier = 140 */
    wrSensorReg16_8(0x3037, ((1 << 4) | 3));   /* root_div=2, pre_div=3 */
    wrSensorReg16_8(0x3108, 0x01);             /* PCLK root=1, sclk2x root=1, sclk root=2 */

    /* 7. 设置 MIPI 虚拟通道 */
    vin_extended_cfg_t *p_vin_ext = (vin_extended_cfg_t *)g_cam_vin_cfg.p_extend;
    ov5645_set_virtual_channel(p_vin_ext->input_ctrl.csi_mode_bits.virtual_channel);

    /* 8. 测试图案 (可选) */
    if (use_test_mode) {
        wrSensorReg16_8(0x5000, 0x00);   /* 禁用 ISP */
        write_config_table(ov5645_test_mode_table);
    }

    /* 10. 关流 → 开 VIN → 配置 CSCE → 开流 */
    ov5645_stream_off();
    vTaskDelay(pdMS_TO_TICKS(5));

    err = R_VIN_Open(&g_cam_vin_ctrl, &g_cam_vin_cfg);
    if (err != FSP_SUCCESS) {
        printf("[CAM] VIN open failed: %d\r\n", (int)err);
        return err;
    }

    R_VIN->DMR &= ~(1UL << 4);
    printf("[CAM] VIN DMR=0x%08lX\r\n", (unsigned long)R_VIN->DMR);

    vTaskDelay(pdMS_TO_TICKS(5));
    ov5645_stream_on();
    {
        uint8_t s4202;
        rdSensorReg16_8(0x4202, &s4202);
        printf("[CAM] Stream on: 0x4202=0x%02X\r\n", s4202);
    }

    printf("[CAM] OV5645 initialized (%dx%d RGB565)\r\n",
           CAM_IMAGE_WIDTH, CAM_IMAGE_HEIGHT);

    return FSP_SUCCESS;
}

void mipi_camera_capture_start(void)
{
    R_VIN_CaptureStart(&g_cam_vin_ctrl, vin_image_buffer_1);
}

bool mipi_camera_wait_frame(uint32_t timeout_ms)
{
    if (g_cam_sem == NULL) return false;
    return (xSemaphoreTake(g_cam_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

uint8_t *mipi_camera_get_frame(void)
{
    return (uint8_t *)g_cam_buf_stored;
}
