/**
 ******************************************************************************
 * @file    face_detection_main.cc
 * @brief   Face detection inference + post-processing for v2 anchor-free model
 *
 * v2 model output format (completely different from v1 YOLO anchors):
 *   Output 0 (boxes):  [1, 4, 180] INT8 — ltrb distances for 180 grid points
 *   Output 1 (scores): [1, 1, 180] INT8 — objectness logit per grid point
 *
 * Decoding (done on CPU, matching deploy_v2/scripts/webcam_tflite_test.py):
 *   1. Dequantize:         float_val = (int8_val - zp) × scale
 *   2. Box distances:      dist = softplus(raw_box)  = ln(1 + exp(raw_box))
 *   3. Objectness:         score = sigmoid(raw_score)
 *   4. Grid mapping:       180 = 12×12 (stride 8) + 6×6 (stride 16)
 *   5. Box decode:         center = (grid_cx + 0.5, grid_cy + 0.5)
 *                          ltrb   = distances × stride → xyxy
 *   6. NMS:                same as before (sorted by score, IoU threshold)
 *
 * No anchor boxes — the model directly predicts (left, top, right, bottom)
 * distances from each grid cell center.  No DFL either (reg_max=1).
 ******************************************************************************
 */
#include "face_detection_build_mode.h"

#if !FACE_DETECT_FLASH_PROGRAMMER

#include "face_detection_config.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "bsp_api.h"

extern "C" {
#include "model/wrapper.h"

/* Shared buffer for preprocessed image (C task → C++ inference) */
extern int8_t g_face_detect_input_buffer[AI_INPUT_IMAGE_SIZE];

/* Write detection results to global array */
extern void face_detect_update_result(uint16_t index, int16_t x, int16_t y,
                                       int16_t w, int16_t h, int16_t id);

/* Active detection mode (face vs hand) */
extern detection_mode_t face_detection_get_mode(void);

/* NPU inference in progress flag */
extern volatile bool g_npu_inferencing;
}

/* ---- Activation functions ---- */

static inline float sigmoid(float x)
{
    if (x >= 0) return 1.0f / (1.0f + expf(-x));
    float ex = expf(x);
    return ex / (1.0f + ex);
}

static inline float softplus(float x)
{
    if (x > 20.0f) return x;           /* avoid overflow          */
    if (x < -20.0f) return 0.0f;       /* exp(large negative) ≈ 0 */
    return log1pf(expf(x));
}

/* ---- NMS: simple greedy IoU suppression ---- */

typedef struct {
    float x, y, w, h;    /* top-left + width/height */
    float score;
    bool  suppressed;
} box_t;

static float box_iou(const box_t *a, const box_t *b)
{
    float ax1 = a->x, ay1 = a->y, ax2 = a->x + a->w, ay2 = a->y + a->h;
    float bx1 = b->x, by1 = b->y, bx2 = b->x + b->w, by2 = b->y + b->h;

    float inter_x1 = (ax1 > bx1) ? ax1 : bx1;
    float inter_y1 = (ay1 > by1) ? ay1 : by1;
    float inter_x2 = (ax2 < bx2) ? ax2 : bx2;
    float inter_y2 = (ay2 < by2) ? ay2 : by2;

    float inter_w = inter_x2 - inter_x1;
    float inter_h = inter_y2 - inter_y1;
    if (inter_w <= 0.0f || inter_h <= 0.0f) return 0.0f;

    float inter = inter_w * inter_h;
    float area_a = a->w * a->h;
    float area_b = b->w * b->h;
    return inter / (area_a + area_b - inter + 1e-9f);
}

static int nms(box_t *boxes, int count, float iou_thresh)
{
    /* Sort descending by score (simple bubble — 180 items is small) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (boxes[j].score > boxes[i].score) {
                box_t tmp = boxes[i];
                boxes[i]  = boxes[j];
                boxes[j]  = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (boxes[i].suppressed) continue;
        for (int j = i + 1; j < count; j++) {
            if (boxes[j].suppressed) continue;
            if (box_iou(&boxes[i], &boxes[j]) >= iou_thresh) {
                boxes[j].suppressed = true;
            }
        }
    }

    /* Compact — keep only unsuppressed boxes */
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!boxes[i].suppressed) {
            boxes[kept++] = boxes[i];
        }
    }
    return kept;
}

/* ======================================================================== */
/*  Temporal tracker — personN labels in first-appearance order              */
/* ======================================================================== */
/*  Each frame the NMS output is confidence-sorted, so naively labelling by
 *  array index makes "person1" jump between faces as confidence fluctuates.
 *  This tracker matches detections to persistent tracks by IoU so each face
 *  keeps its position in first-appearance order, then emits them as a
 *  contiguous run (person1, person2, … up to the number of faces on screen).
 *  When a face leaves, the remaining faces renumber down to fill the gap.   */

#define TRACK_IOU_THRESHOLD  0.3f   /* min IoU to match a detection to a track */
#define TRACK_MAX_MISSED     10     /* drop a track after N consecutive misses */

typedef struct {
    float x, y, w, h;               /* camera-space box (top-left + size) */
    uint16_t id;                    /* monotonic creation order — used only to
                                       sort into first-appearance order       */
    uint16_t missed;
    bool active;
} track_t;

static track_t    g_tracks[AI_MAX_DETECTION_NUM];
static uint32_t   g_active_tracks = 0;
static uint32_t   g_next_id       = 0;
static detection_mode_t g_last_track_mode = DETECTION_MODE_FACE;

static float track_iou(const track_t *t, float x, float y, float w, float h)
{
    float ax1 = t->x, ay1 = t->y, ax2 = t->x + t->w, ay2 = t->y + t->h;
    float bx1 = x,     by1 = y,     bx2 = x + w,     by2 = y + h;

    float ix1 = (ax1 > bx1) ? ax1 : bx1;
    float iy1 = (ay1 > by1) ? ay1 : by1;
    float ix2 = (ax2 < bx2) ? ax2 : bx2;
    float iy2 = (ay2 < by2) ? ay2 : by2;
    float iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;

    float inter   = iw * ih;
    float area_a  = t->w * t->h;
    float area_b  = w * h;
    return inter / (area_a + area_b - inter + 1e-9f);
}

static void track_reset(void)
{
    g_active_tracks = 0;
    g_next_id = 0;
}

/* Match N camera-space boxes to persistent tracks.  Fills out_x/out_y/out_w/
 * out_h/out_id with tracked boxes sorted in ascending id order (first-
 * appearance order) and returns the output count. */
static uint32_t track_update(const float *inx, const float *iny,
                             const float *inw, const float *inh, uint32_t n,
                             float *outx, float *outy, float *outw, float *outh,
                             uint16_t *outid)
{
    bool matched[AI_MAX_DETECTION_NUM] = {false};

    /* 1. Greedy-match each detection to the best-IoU free track */
    for (uint32_t d = 0; d < n; d++) {
        float best_iou = TRACK_IOU_THRESHOLD;
        int   best     = -1;
        for (uint32_t t = 0; t < g_active_tracks; t++) {
            if (matched[t]) continue;
            float iou = track_iou(&g_tracks[t], inx[d], iny[d], inw[d], inh[d]);
            if (iou > best_iou) { best_iou = iou; best = (int)t; }
        }

        if (best >= 0) {
            g_tracks[best].x = inx[d];
            g_tracks[best].y = iny[d];
            g_tracks[best].w = inw[d];
            g_tracks[best].h = inh[d];
            g_tracks[best].missed = 0;
            matched[best] = true;
        } else if (g_active_tracks < AI_MAX_DETECTION_NUM) {
            /* New face → new track, next id in first-appearance order */
            track_t *nt = &g_tracks[g_active_tracks];
            nt->x = inx[d]; nt->y = iny[d]; nt->w = inw[d]; nt->h = inh[d];
            nt->id = (uint16_t)g_next_id++;
            nt->missed = 0;
            nt->active = true;
            matched[g_active_tracks] = true;   /* seen this frame → don't age */
            g_active_tracks++;
        }
    }

    /* 2. Age unmatched tracks; drop stale ones and compact */
    uint32_t w = 0;
    for (uint32_t t = 0; t < g_active_tracks; t++) {
        if (!matched[t] && ++g_tracks[t].missed > TRACK_MAX_MISSED) {
            g_tracks[t].active = false;
        }
        if (g_tracks[t].active) {
            g_tracks[w++] = g_tracks[t];
        }
    }
    g_active_tracks = w;

    /* 3. Sort by id ascending → first-appearance order */
    for (uint32_t i = 0; i < w; i++) {
        for (uint32_t j = i + 1; j < w; j++) {
            if (g_tracks[j].id < g_tracks[i].id) {
                track_t tmp = g_tracks[i];
                g_tracks[i] = g_tracks[j];
                g_tracks[j] = tmp;
            }
        }
    }

    /* 4. Emit — contiguous ids (person1, person2, …) in first-appearance order.
     * Tracks are already sorted by creation order above, so the array position
     * IS the current on-screen order; re-number from 0 each frame. */
    if (w > AI_MAX_DETECTION_NUM) w = AI_MAX_DETECTION_NUM;
    for (uint32_t i = 0; i < w; i++) {
        outx[i]   = g_tracks[i].x;
        outy[i]   = g_tracks[i].y;
        outw[i]   = g_tracks[i].w;
        outh[i]   = g_tracks[i].h;
        outid[i]  = (uint16_t)i;
    }
    return w;
}

/* ======================================================================== */
/*  Main inference entry point                                               */
/* ======================================================================== */

extern "C" int face_detect_run_inference(void)
{
    /* ---- Step 1: Copy preprocessed image to NPU arena ---- */
    memcpy(mera_input_ptr(), g_face_detect_input_buffer, AI_INPUT_IMAGE_SIZE);

    /* ---- Diagnostic: verify input is changing ---- */
    {
        static int diag_cnt = 0;
        if (diag_cnt < 3) {
            int sum = 0;
            for (int i = 0; i < 16; i++) sum += g_face_detect_input_buffer[i];
            printf("[AI] frame#%d input[0..3]=%d %d %d %d sum16=%d\r\n",
                   diag_cnt,
                   g_face_detect_input_buffer[0], g_face_detect_input_buffer[1],
                   g_face_detect_input_buffer[2], g_face_detect_input_buffer[3], sum);
            diag_cnt++;
        }
    }

    /* ---- Step 2: Run NPU inference ---- */
    mera_invoke();

    /* ---- Step 3: Get output tensor pointers ---- */
    /* Output0: box distances [1, 4, 180] INT8  → 720 bytes */
    /* Output1: scores          [1, 1, 180] INT8  → 180 bytes */
    int8_t *raw_box   = (int8_t *)mera_output1_ptr();
    int8_t *raw_score = (int8_t *)mera_output2_ptr();

    /* ---- Mode-aware output dequantization + AI→camera mapping ---- */
    detection_mode_t mode = face_detection_get_mode();
    const float box_scale  = (mode == DETECTION_MODE_HAND) ? HAND_OUTPUT_BOX_SCALE   : AI_OUTPUT_BOX_SCALE;
    const int   box_zp     = (mode == DETECTION_MODE_HAND) ? HAND_OUTPUT_BOX_ZERO_POINT  : AI_OUTPUT_BOX_ZERO_POINT;
    const float score_scale= (mode == DETECTION_MODE_HAND) ? HAND_OUTPUT_SCORE_SCALE : AI_OUTPUT_SCORE_SCALE;
    const int   score_zp   = (mode == DETECTION_MODE_HAND) ? HAND_OUTPUT_SCORE_ZERO_POINT: AI_OUTPUT_SCORE_ZERO_POINT;

    /* ---- Diagnostic: verify NPU output changes between inferences ---- */
    {
        static int diag2_cnt = 0;
        static int8_t prev_score[4] = {0};
        if (diag2_cnt < 4) {
            printf("[AI] infer#%d box[0..3]=%d %d %d %d score[0..3]=%d %d %d %d\r\n",
                   diag2_cnt,
                   raw_box[0], raw_box[1], raw_box[2], raw_box[3],
                   raw_score[0], raw_score[1], raw_score[2], raw_score[3]);
            if (diag2_cnt > 0) {
                int changed = (raw_score[0] != prev_score[0] ||
                               raw_score[1] != prev_score[1]);
                printf("[AI]   -> score changed=%d\r\n", changed);
            }
            for (int i = 0; i < 4; i++) prev_score[i] = raw_score[i];
            diag2_cnt++;
        }
    }

    /* ---- Step 4: Dequantize and decode ---- */
    /* Grid layout: 12×12 (stride=8, indices 0–143) + 6×6 (stride=16, indices 144–179) */
    static const int   grids[]    = { 12, 6 };
    static const int   strides[]  = { 8,  16 };
    static const int   offsets[]  = { 0,  144 };   /* start index in 180-element array */

    /* Collect candidates above threshold.
     * static — 180 × 24 B ≈ 4.3 KB, too large for 2 KB FreeRTOS task stack. */
    static box_t candidates[AI_OUTPUT_GRID_POINTS];
    int          cand_count = 0;

    for (int scale = 0; scale < 2; scale++) {
        int grid   = grids[scale];
        int stride = strides[scale];
        int start  = offsets[scale];

        for (int gy = 0; gy < grid; gy++) {
            for (int gx = 0; gx < grid; gx++) {
                int idx = start + gy * grid + gx;

                /* 4a. Score: dequantize + sigmoid */
                float score_raw = ((float)raw_score[idx] - (float)score_zp)
                                * score_scale;
                float score = sigmoid(score_raw);

                if (score < AI_DETECTION_THRESHOLD) continue;

                /* 4b. Box distances: dequantize + softplus */
                /* Layout: channels (l, t, r, b) are consecutive, then grid points */
                int bo = idx;   /* box offset for this grid point */
                float dl = softplus(((float)raw_box[bo + 0 * AI_OUTPUT_GRID_POINTS]
                                  - (float)box_zp) * box_scale);
                float dt = softplus(((float)raw_box[bo + 1 * AI_OUTPUT_GRID_POINTS]
                                  - (float)box_zp) * box_scale);
                float dr = softplus(((float)raw_box[bo + 2 * AI_OUTPUT_GRID_POINTS]
                                  - (float)box_zp) * box_scale);
                float db = softplus(((float)raw_box[bo + 3 * AI_OUTPUT_GRID_POINTS]
                                  - (float)box_zp) * box_scale);

                /* 4c. Decode to xyxy (in 96×96 coordinate space).
                 *
                 * l/t/r/b are in GRID-CELL units (not pixels), so the
                 * offset arithmetic must happen BEFORE the stride multiply.
                 * Reference: deploy_v2/DECODE.md §3.3:
                 *   x1 = ((grid_x + 0.5) - l) * stride
                 *   y1 = ((grid_y + 0.5) - t) * stride
                 *   x2 = ((grid_x + 0.5) + r) * stride
                 *   y2 = ((grid_y + 0.5) + b) * stride */
                float x1 = (((float)gx + 0.5f) - dl) * (float)stride;
                float y1 = (((float)gy + 0.5f) - dt) * (float)stride;
                float x2 = (((float)gx + 0.5f) + dr) * (float)stride;
                float y2 = (((float)gy + 0.5f) + db) * (float)stride;

                /* Clamp to image bounds */
                if (x1 < 0.0f) x1 = 0.0f;
                if (y1 < 0.0f) y1 = 0.0f;
                if (x2 > (float)AI_INPUT_IMAGE_WIDTH)  x2 = (float)AI_INPUT_IMAGE_WIDTH;
                if (y2 > (float)AI_INPUT_IMAGE_HEIGHT) y2 = (float)AI_INPUT_IMAGE_HEIGHT;

                float w = x2 - x1;
                float h = y2 - y1;
                if (w <= 0.0f || h <= 0.0f) continue;

                /* 4d. Store candidate */
                if (cand_count < AI_OUTPUT_GRID_POINTS) {
                    candidates[cand_count].x          = x1;
                    candidates[cand_count].y          = y1;
                    candidates[cand_count].w          = w;
                    candidates[cand_count].h          = h;
                    candidates[cand_count].score      = score;
                    candidates[cand_count].suppressed = false;
                    cand_count++;
                }
            }
        }
    }

    /* ---- Step 5: NMS ---- */
    int kept = nms(candidates, cand_count, AI_NMS_THRESHOLD);

    /* ---- Diagnostic (first 3 inferences only) ---- */
    {
        static int diag_count = 0;
        if (diag_count < 3) {
            diag_count++;
            /* Raw score tensor: min/max/mean of first 10 values */
            int  s_min = 127, s_max = -128;
            int  s_sum = 0;
            for (int i = 0; i < 10; i++) {
                int v = (int)raw_score[i];
                if (v < s_min) s_min = v;
                if (v > s_max) s_max = v;
                s_sum += v;
            }
            /* Raw box tensor: first 2 grid points (8 values) */
            printf("[FACE_DET v2] diag#%d  score_raw[0..9]: min=%d max=%d avg=%.1f  "
                   "box_raw[0..7]: %d %d %d %d %d %d %d %d  "
                   "cand=%d kept=%d best_score=%.4f\r\n",
                   diag_count, s_min, s_max, s_sum / 10.0f,
                   raw_box[0], raw_box[1], raw_box[2], raw_box[3],
                   raw_box[4], raw_box[5], raw_box[6], raw_box[7],
                   cand_count, kept,
                   kept > 0 ? candidates[0].score : 0.0f);
        }
    }

    /* ---- Step 6: Map AI → camera, track (face), write results ---- */
    for (uint16_t i = 0; i < AI_MAX_DETECTION_NUM; i++) {
        face_detect_update_result(i, 0, 0, 0, 0, 0);
    }

    /* Reset tracker on mode change so ids restart per mode. */
    if (g_last_track_mode != mode) {
        track_reset();
        g_last_track_mode = mode;
    }

    /* Precompute the AI→camera mapping for the active model. */
    float map_scale, map_off_x, map_off_y;
    if (mode == DETECTION_MODE_HAND) {
        /* Letterbox: cam = (ai - offset) / ratio */
        float inv_ratio = 1.0f / HAND_LETTERBOX_RATIO;
        map_scale = inv_ratio;
        map_off_x = -(float)HAND_LETTERBOX_OFFSET_X * inv_ratio;
        map_off_y = -(float)HAND_LETTERBOX_OFFSET_Y * inv_ratio;
    } else {
        /* Centre-crop: crop 480×480, scale 480/96, x-offset (640-480)/2 */
        map_scale = (float)CAMERA_INPUT_HEIGHT / (float)AI_INPUT_IMAGE_HEIGHT;
        map_off_x = (float)(CAMERA_INPUT_WIDTH - CAMERA_INPUT_HEIGHT) / 2.0f;
        map_off_y = 0.0f;
    }

    /* Map NMS candidates (AI space) → camera space. */
    float camx[AI_MAX_DETECTION_NUM], camy[AI_MAX_DETECTION_NUM];
    float camw[AI_MAX_DETECTION_NUM], camh[AI_MAX_DETECTION_NUM];
    uint32_t cam_count = 0;
    for (int i = 0; i < kept && cam_count < AI_MAX_DETECTION_NUM; i++) {
        float cx = candidates[i].x * map_scale + map_off_x;
        float cy = candidates[i].y * map_scale + map_off_y;
        float cw = candidates[i].w * map_scale;
        float ch = candidates[i].h * map_scale;

        /* Clamp to camera frame bounds */
        if (cx < 0.0f) { cw += cx; cx = 0.0f; }
        if (cy < 0.0f) { ch += cy; cy = 0.0f; }
        if (cx + cw > (float)CAMERA_INPUT_WIDTH)  cw = (float)CAMERA_INPUT_WIDTH  - cx;
        if (cy + ch > (float)CAMERA_INPUT_HEIGHT) ch = (float)CAMERA_INPUT_HEIGHT - cy;
        if (cw <= 0.0f || ch <= 0.0f) continue;

        camx[cam_count] = cx; camy[cam_count] = cy;
        camw[cam_count] = cw; camh[cam_count] = ch;
        cam_count++;
    }

    if (mode == DETECTION_MODE_HAND) {
        /* Hand mode: no tracking; label is simply "hand" (id=0). */
        for (uint32_t i = 0; i < cam_count; i++) {
            face_detect_update_result((uint16_t)i,
                (int16_t)(camx[i] + 0.5f), (int16_t)(camy[i] + 0.5f),
                (int16_t)(camw[i] + 0.5f), (int16_t)(camh[i] + 0.5f), 0);
        }
    } else {
        /* Face mode: track to keep stable personN labels in first-appearance order. */
        float ox[AI_MAX_DETECTION_NUM], oy[AI_MAX_DETECTION_NUM];
        float ow[AI_MAX_DETECTION_NUM], oh[AI_MAX_DETECTION_NUM];
        uint16_t oid[AI_MAX_DETECTION_NUM];
        uint32_t out_count = track_update(camx, camy, camw, camh, cam_count,
                                          ox, oy, ow, oh, oid);
        for (uint32_t i = 0; i < out_count; i++) {
            face_detect_update_result((uint16_t)i,
                (int16_t)(ox[i] + 0.5f), (int16_t)(oy[i] + 0.5f),
                (int16_t)(ow[i] + 0.5f), (int16_t)(oh[i] + 0.5f),
                (int16_t)oid[i]);
        }
    }

    return 0;
}

#endif /* !FACE_DETECT_FLASH_PROGRAMMER */
