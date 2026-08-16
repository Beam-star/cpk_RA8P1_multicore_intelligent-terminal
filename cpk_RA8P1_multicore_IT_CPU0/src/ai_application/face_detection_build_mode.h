/**
 ******************************************************************************
 * @file    face_detection_build_mode.h
 * @brief   Build mode switch for AI face detection subsystem
 *
 * Model deployment: Use PCDC flash transfer tool (see PCDC_FLASH_TOOL_GUIDE.md)
 *
 *   python scripts/pcdc_flash_tool.py COM10 erase 0x000000 0x070000
 *   python scripts/pcdc_flash_tool.py COM10 write sub_0000_model_data.c 0x000000
 *   python scripts/pcdc_flash_tool.py COM10 write sub_0000_command_stream.c 0x000670A0
 *
 * At boot, the firmware loads model data from W25Q256 to SDRAM.  The large
 * model data files (sub_0000_model_data.c, sub_0000_io_data.c,
 * sub_0000_command_stream.c, model_io_data.c) are EXCLUDED from the
 * e2studio build to save firmware flash space.
 *
 * FACE_DETECT_FLASH_PROGRAMMER: Always 0.  The old "compile model into
 * firmware then burn to flash" mode has been replaced by PCDC transfer.
 ******************************************************************************
 */
#ifndef FACE_DETECTION_BUILD_MODE_H__
#define FACE_DETECTION_BUILD_MODE_H__

/* Always 0 â€?model deployed via PCDC virtual COM port, not compiled-in */
#define FACE_DETECT_FLASH_PROGRAMMER    0

#endif /* FACE_DETECTION_BUILD_MODE_H__ */
