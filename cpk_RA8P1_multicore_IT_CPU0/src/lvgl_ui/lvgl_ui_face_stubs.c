/**
 ******************************************************************************
 * @file    lvgl_ui_face_stubs.c
 * @brief   Obsolete — face-embedding enroll/check-in glue.
 *
 * The face-feature check-in pipeline (embedding + face_db) has been replaced
 * by hand detection.  The enroll / check-in / keyboard flow was removed from
 * lvgl_ui_main.c; a mode-toggle button now switches the NPU between the face
 * and hand detectors.
 *
 * This file is retained as an empty translation unit so it can be dropped from
 * the e2studio build (or deleted) without leaving dangling references.
 ******************************************************************************
 */
