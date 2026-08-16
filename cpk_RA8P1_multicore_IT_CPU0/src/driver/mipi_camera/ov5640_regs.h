/**
 ******************************************************************************
 * @file    ov5640_regs.h
 * @brief   OV5640 寄存器地址定义
 *
 * 寄存器地址均为 16-bit, 通过 I2C (SCCB) 总线访问:
 *   写: [START] [0x3C+W] [addr_hi] [addr_lo] [data] [STOP]
 *   读: [START] [0x3C+W] [addr_hi] [addr_lo] [RESTART] [0x3C+R] [data] [STOP]
 *
 * 参考来源: OpenMV 项目、OV5640 datasheet、Renesas mipicsi_camera 示例
 ******************************************************************************
 */

#ifndef OV5640_REGS_H_
#define OV5640_REGS_H_

/* ---- 系统控制寄存器 ---- */
#define SYSTEM_RESET_00          0x3000   /* 软复位控制 0 */
#define SYSTEM_RESET_01          0x3001   /* 软复位控制 1 */
#define SYSTEM_RESET_02          0x3002   /* 软复位控制 2 (JPEG编解码器使能) */
#define CLOCK_ENABLE_00          0x3004   /* 时钟使能 0 */
#define CLOCK_ENABLE_02          0x3006   /* 时钟使能 2 (JPEG时钟) */
#define SYSTEM_CTROL0            0x3008   /* 系统控制: bit[7]=软复位, bit[6]=电源休眠 */
#define SCCB_SYSTEM_CTRL_1       0x3103   /* SCCB 系统控制 (时钟源选择) */
#define SYSTEM_ROOT_DIVIDER      0x3108   /* 系统根分频器 (PCLK/SCLK/sclk2x 分频) */

/* ---- 芯片 ID (只读, 用于识别 OV5640) ---- */
#define REG_CHIP_ID_H            0x300A   /* 高字节: 固定 0x56 */
#define REG_CHIP_ID_L            0x300B   /* 低字节: REV1A=0x40, REV2A=0x41, REV2C=0x4C */

/* ---- MIPI 控制 ---- */
#define MIPI_CTRL_00             0x300E   /* MIPI 通道控制: bit[2]=MIPI使能, bit[5:4]=lane数 */
#define MIPI_SC_CTRL             0x302E   /* MIPI SC 控制 */

/* ---- PLL 时钟 (控制 MIPI 时钟和系统时钟) ----
 * PLL 计算: base_pll = 24MHz / (root_div × pre_div) × multiplier
 * 系统时钟 = base_pll / (sys_clock_div × sclk2x_root_div × sclk_root_div)
 * MIPI 时钟 = base_pll / (mipi_clock_div × pclk_root_div)
 */
#define SC_PLL_CONTRL0           0x3034   /* bit[3:0]: 8/10/12-bit MIPI 模式选择 */
#define SC_PLL_CONTRL1           0x3035   /* bit[7:4]=系统时钟分频, bit[3:0]=MIPI时钟分频 */
#define SC_PLL_CONTRL2           0x3036   /* bit[7:0]=PLL 倍频系数 (4~252, 偶数) */
#define SC_PLL_CONTRL3           0x3037   /* bit[4]=PLL根分频(0=旁路,1=÷2), bit[3:0]=预分频(1,2,3,4,6,8) */

/* ---- Auto Focus ---- */
#define AF_CMD_MAIN              0x3022
#define AF_CMD_ACK               0x3023
#define AF_FW_STATUS             0x3029
#define MCU_FIRMWARE_BASE        0x8000

/* ---- AWB (Auto White Balance) ---- */
#define AWB_R_GAIN_H             0x3400
#define AWB_R_GAIN_L             0x3401
#define AWB_G_GAIN_H             0x3402
#define AWB_G_GAIN_L             0x3403
#define AWB_B_GAIN_H             0x3404
#define AWB_B_GAIN_L             0x3405
#define AWB_MANUAL_CONTROL       0x3406

/* ---- AEC/AGC (Auto Exposure/Gain) ---- */
#define AEC_PK_EXPOSURE_0        0x3500
#define AEC_PK_EXPOSURE_1        0x3501
#define AEC_PK_EXPOSURE_2        0x3502
#define AEC_PK_MANUAL            0x3503
#define AEC_PK_REAL_GAIN_H       0x350A
#define AEC_PK_REAL_GAIN_L       0x350B

/* ---- 时序 / 分辨率控制 ----
 *
 * OV5640 图像处理管线:
 *   全幅传感器 (2624×1964)
 *     → 窗口裁剪 (0x3800~0x3807: X/Y 起止地址)
 *     → ISP 缩放 (0x3810~0x3813: 偏移, 0x3814~0x3815: 子采样步长)
 *     → 输出尺寸 (0x3808~0x380B: DVPHO/DVPVO)
 *     → 总时序 (0x380C~0x380F: HTS/VTS)
 */
#define TIMING_HS_H              0x3800   /* 水平起始地址 高字节 */
#define TIMING_HS_L              0x3801   /* 水平起始地址 低字节 */
#define TIMING_VS_H              0x3802   /* 垂直起始地址 高字节 */
#define TIMING_VS_L              0x3803   /* 垂直起始地址 低字节 */
#define TIMING_HW_H              0x3804   /* 水平结束地址 高字节 */
#define TIMING_HW_L              0x3805   /* 水平结束地址 低字节 */
#define TIMING_VH_H              0x3806   /* 垂直结束地址 高字节 */
#define TIMING_VH_L              0x3807   /* 垂直结束地址 低字节 */
#define TIMING_DVPHO_H           0x3808   /* 输出水平尺寸 高字节 (如 0x02=640>>8) */
#define TIMING_DVPHO_L           0x3809   /* 输出水平尺寸 低字节 (如 0x80=640&0xFF) */
#define TIMING_DVPVO_H           0x380A   /* 输出垂直尺寸 高字节 (如 0x01=480>>8) */
#define TIMING_DVPVO_L           0x380B   /* 输出垂直尺寸 低字节 (如 0xE0=480&0xFF) */
#define TIMING_HTS_H             0x380C   /* 水平总尺寸 高字节 (含 blanking, 影响帧率) */
#define TIMING_HTS_L             0x380D   /* 水平总尺寸 低字节 */
#define TIMING_VTS_H             0x380E   /* 垂直总尺寸 高字节 (含 blanking, 影响帧率) */
#define TIMING_VTS_L             0x380F   /* 垂直总尺寸 低字节 */
#define TIMING_HOFFSET_H         0x3810   /* ISP 水平偏移 高字节 */
#define TIMING_HOFFSET_L         0x3811   /* ISP 水平偏移 低字节 */
#define TIMING_VOFFSET_H         0x3812   /* ISP 垂直偏移 高字节 */
#define TIMING_VOFFSET_L         0x3813   /* ISP 垂直偏移 低字节 */
#define TIMING_X_INC             0x3814   /* 水平子采样步长: bit[7:4]=奇数, bit[3:0]=偶数 */
#define TIMING_Y_INC             0x3815   /* 垂直子采样步长: bit[7:4]=奇数, bit[3:0]=偶数 */
#define TIMING_TC_REG_20         0x3820   /* bit[2]=ISP垂直翻转, bit[1]=传感器垂直翻转 */
#define TIMING_TC_REG_21         0x3821   /* bit[2]=ISP水平镜像, bit[1]=传感器水平镜像 */

/* ---- ISP / 输出格式 ----
 *
 * FORMAT_CONTROL (0x4300) 和 FORMAT_CONTROL_MUX (0x501F) 配合决定输出格式:
 *   YUV422 YUYV:  0x4300=0x30, 0x501F=0x00
 *   RGB565:       0x4300=0x6F, 0x501F=0x01
 *   Bayer RAW:    0x4300=0x00, 0x501F=0x01
 *   本项目使用 YUV422 (由 VIN 模块转换为 RGB565)
 */
#define FORMAT_CONTROL           0x4300   /* 输出格式控制 (YUV/RGB/RAW 选择) */
#define FORMAT_CONTROL_MUX       0x501F   /* ISP 格式多路选择 */
#define ISP_CONTROL_00           0x5300   /* ISP 控制寄存器 */
#define PRE_ISP_TEST             0x503D   /* bit[7]=测试图案使能, bit[2:0]=图案类型 */

/* ---- BLC (Black Level Correction) ---- */
#define BLC_CTRL_00              0x4000
#define BLACK_LEVEL_00_H         0x402C
#define BLACK_LEVEL_00_L         0x402D
#define BLACK_LEVEL_01_H         0x402E
#define BLACK_LEVEL_01_L         0x402F
#define BLACK_LEVEL_10_H         0x4030
#define BLACK_LEVEL_10_L         0x4031
#define BLACK_LEVEL_11_H         0x4032
#define BLACK_LEVEL_11_L         0x4033

/* ---- VFIFO (Video FIFO) ---- */
#define VFIFO_HSIZE_H            0x4602
#define VFIFO_HSIZE_L            0x4603
#define VFIFO_VSIZE_H            0x4604
#define VFIFO_VSIZE_L            0x4605

/* ---- JPEG ---- */
#define JPEG_CTRL07              0x4407

/* ---- MIPI Timing ---- */
#define MIPI_CTRL_00_REG         0x4800
#define MIPI_TIMING_REG          0x4837

#endif /* OV5640_REGS_H_ */
