#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_camera.h"
#include "esp_log.h"

#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
#define AP_SSID             "ESP32-CAM"
#define AP_PASSWORD         "12345678"

#define FEATURE_DIM         128
#define SIMILARITY_THRESHOLD 0.70
#define MULTI_SAMPLES       3
#define HARD_MAX_FACES      100

#define CAMERA_FRAME_SIZE   FRAMESIZE_QVGA
#define CAMERA_PIXEL_FORMAT PIXFORMAT_RGB565
#define CAMERA_XCLK_FREQ    20000000
#define CAMERA_FB_COUNT     2

#define PWDN_GPIO_NUM       32
#define RESET_GPIO_NUM      -1
#define XCLK_GPIO_NUM       0
#define SIOD_GPIO_NUM       26
#define SIOC_GPIO_NUM       27
#define Y9_GPIO_NUM         35
#define Y8_GPIO_NUM         34
#define Y7_GPIO_NUM         39
#define Y6_GPIO_NUM         36
#define Y5_GPIO_NUM         21
#define Y4_GPIO_NUM         19
#define Y3_GPIO_NUM         18
#define Y2_GPIO_NUM         5
#define VSYNC_GPIO_NUM      25
#define HREF_GPIO_NUM       23
#define PCLK_GPIO_NUM       22

#define SD_MOSI             15
#define SD_MISO             2
#define SD_CLK              14
#define SD_CS               13
#define SD_MOUNT_POINT      "/sdcard"

#define TASK_CAMERA_STACK       4096
#define TASK_CAMERA_PRIO        6
#define TASK_DETECT_STACK       16384
#define TASK_DETECT_PRIO        3
#define TASK_WEB_SERVER_STACK   8192
#define TASK_WEB_SERVER_PRIO    5

typedef struct {
    uint32_t id;
    float features[FEATURE_DIM];
    uint32_t timestamp;
    uint16_t x, y, w, h;
    uint8_t valid;
    uint8_t sample_count;
} FaceRecord;

typedef struct {
    int count;
    uint16_t x[8];
    uint16_t y[8];
    uint16_t w[8];
    uint16_t h[8];
    float embeddings[8][FEATURE_DIM];
    int recognized[8];
    int face_ids[8];
} FaceDetectResult;

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint16_t width;
    uint16_t height;
} JpegFrame;

typedef enum {
    STORAGE_NONE,
    STORAGE_SPIFFS,
    STORAGE_SD
} storage_type_t;

extern FaceRecord faceDB[HARD_MAX_FACES];
extern uint32_t faceCount;
extern uint32_t nextFaceId;
extern volatile bool detectActive;
extern QueueHandle_t streamQueue;
extern uint32_t maxFacesAllowed;
extern storage_type_t storageType;

bool camera_init(void);
int detect_faces(camera_fb_t *fb, FaceDetectResult *result);
void extract_features(uint8_t *rgb565, int imgW, int imgH,
                      uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh,
                      float *embedding);
void face_db_init(void);
void face_db_load(void);
bool face_db_save(void);
bool face_db_save_image(uint32_t face_id, uint8_t *jpg_buf, size_t jpg_len);
bool face_db_delete_image(uint32_t face_id);
int face_db_find_similar(float *features);
int face_db_add(float *features, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void face_db_clear(void);
void web_server_task(void *arg);
bool get_latest_jpeg_copy(uint8_t **out_buf, size_t *out_len);
