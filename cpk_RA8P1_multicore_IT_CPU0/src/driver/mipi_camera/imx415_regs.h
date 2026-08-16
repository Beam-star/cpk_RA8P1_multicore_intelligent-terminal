/**
 ******************************************************************************
 * @file    imx415_regs.h
 * @brief   Sony IMX415 CMOS image sensor register definitions
 *
 * The IMX415 is an 8.4MP 4K CMOS sensor with MIPI CSI-2 output (2/4-lane).
 * I2C control interface: 7-bit address 0x1a, 16-bit register addresses, 8-bit data.
 * Output format: RAW10 (Bayer pattern).
 *
 * Reference: Linux kernel driver (drivers/media/i2c/imx415.c),
 *            Sony IMX415 datasheet, ATK-MCIMX415 module spec.
 ******************************************************************************
 */

#ifndef IMX415_REGS_H_
#define IMX415_REGS_H_

#include <stdint.h>

/* ---- I2C Slave Address ---- */
#define IMX415_I2C_ADDR             0x1A    /* 7-bit address */

/* ---- Sensor Identification ---- */
#define IMX415_REG_CHIP_ID          0x3132  /* Sensor info register (16-bit read) */
#define IMX415_CHIP_ID              0x050C  /* Expected chip ID (varies by revision) */

/* ---- System Control (page 0x30) ---- */
#define IMX415_SYS_MODE             0x3000  /* System mode: standby/operating */
#define IMX415_SYS_MODE_STANDBY     0x07    /* Standby mode (low power) */
#define IMX415_SYS_MODE_OPERATING   0x08    /* Operating mode */

#define IMX415_XMSTA                0x3002  /* Master start: 0x00 = start output */
#define IMX415_XMSTA_START          0x00    /* Start MIPI output */
#define IMX415_XMSTA_STOP           0x01    /* Stop MIPI output */

#define IMX415_XMASTER              0x3003  /* Master/slave select: 0=master, 1=slave */

/* ---- Output Configuration ---- */
#define IMX415_WINMODE              0x3008  /* Window mode: 0=all-pixel readout */
#define IMX415_ADDMODE              0x3009  /* Flip mode: bit0=V-flip, bit1=H-mirror */
#define IMX415_REVERSE              0x300A  /* Reverse mode */
#define IMX415_ADBIT                0x300B  /* AD bit mode: RAW10/12 selection */
#define IMX415_ADBIT_RAW10          0x00    /* RAW 10-bit mode */
#define IMX415_MDBIT                0x300C  /* MDBIT */
#define IMX415_OUTSEL               0x300D  /* Output select: 0x22 = XVS/VSYNC, XHS/LOW */
#define IMX415_DRV                  0x300E  /* Drive strength */

/* ---- Image Window (ROI) ----
 *   X_ADDR_START (0x3018-9): Horizontal ROI start [15:0]
 *   Y_ADDR_START (0x301A-B): Vertical ROI start   [15:0]
 *   X_ADDR_END   (0x301C-D): Horizontal ROI end   [15:0]
 *   Y_ADDR_END   (0x301E-F): Vertical ROI end     [15:0]
 *   X_OUT_SIZE   (0x3020-1): Output horizontal size [15:0]
 *   Y_OUT_SIZE   (0x3022-3): Output vertical size   [15:0]
 *
 * IMX415 full pixel array: 3864 x 2192 effective
 * For 640x480 output: center-crop a 1280x960 ROI, then set OUT_SIZE to 640x480
 * (the sensor supports digital scaling/binning within the ROI)
 */
#define IMX415_X_ADDR_START         0x3018
#define IMX415_Y_ADDR_START         0x301A
#define IMX415_X_ADDR_END           0x301C
#define IMX415_Y_ADDR_END           0x301E
#define IMX415_X_OUT_SIZE           0x3020
#define IMX415_Y_OUT_SIZE           0x3022

/* ---- Frame Timing ----
 *   VMAX (0x3024-6): Total lines per frame (incl. vertical blanking), 20-bit
 *   HMAX (0x3028-9): Total clocks per line (incl. horizontal blanking), 16-bit
 *
 * Frame rate = INCK_freq / (HMAX * VMAX)
 * where INCK is the internal pixel clock after PLL dividers.
 */
#define IMX415_VMAX                 0x3024
#define IMX415_HMAX                 0x3028

/* ---- Clock / PLL Configuration (INCKSEL) ----
 * Controls the internal PLL and clock dividers.
 * INCK_freq after dividers determines pixel rate.
 * Values depend on XCLK input frequency (24/27/37.125/72/74.25 MHz).
 */
#define IMX415_INCKSEL1             0x3115
#define IMX415_INCKSEL2             0x3116
#define IMX415_INCKSEL3             0x3118
#define IMX415_INCKSEL4             0x311A
#define IMX415_INCKSEL5             0x311E
#define IMX415_INCKSEL6             0x3200
#define IMX415_INCKSEL7             0x4074

/* ---- MIPI CSI-2 Configuration ---- */
#define IMX415_LANEMODE             0x4001  /* Lane mode: 1=2-lane, 3=4-lane */
#define IMX415_LANEMODE_2LANE       0x01    /* 2-lane MIPI */
#define IMX415_LANEMODE_4LANE       0x03    /* 4-lane MIPI */

/* ---- MIPI D-PHY Timing Registers ----
 * Values depend on link frequency.
 *
 * 720 Mbps/lane (2-lane):
 *   TCLKPOST=0x6F, TCLKPREPARE=0x2F, TCLKTRAIL=0x2F, TLPX=0x27
 * 1440 Mbps/lane (2-lane):
 *   TCLKPOST=0x9F, TCLKPREPARE=0x57, TCLKTRAIL=0x57, TLPX=0x4F
 */
#define IMX415_TCLKPOST             0x4002
#define IMX415_TCLKPREPARE          0x4003
#define IMX415_TCLKTRAIL            0x4004
#define IMX415_TLPX                 0x4005

/* ---- CSI-2 Clock Lane Timing ---- */
#define IMX415_TXCLKESC_FREQ        0x400A  /* TX clock escape frequency */

#define IMX415_BCWAIT_TIME          0x400C  /* CSI clock lane Tbwait time */
#define IMX415_CPWAIT_TIME          0x400E  /* CSI clock lane Tprepare wait time */

/* ========================================================================
 *  IMX415 Register Configuration Entry
 *
 *  Same structure format as OV5640 for compatibility with existing
 *  config table write functions.
 * ======================================================================== */

/* Config table sentinel values */
#define IMX415_TABLE_END            0xFFFF
#define IMX415_REQUEST_WAIT         0xAAAA

#endif /* IMX415_REGS_H_ */
