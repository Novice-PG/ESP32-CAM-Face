#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include <string.h>
#include <math.h>

static const char *TAG = "MAIN";

volatile bool detectActive = true;

char lastEvent[128] = "";
char eventType[16] = "detect";
static int totalDetected = 0;
volatile uint32_t streamFps = 0;
volatile uint32_t detectFps = 0;

static uint8_t *latest_jpeg = NULL;
static size_t latest_jpeg_len = 0;
static SemaphoreHandle_t jpeg_mutex = NULL;

void web_server_task(void *arg);

bool get_latest_jpeg_copy(uint8_t **out_buf, size_t *out_len) {
    if (!jpeg_mutex || !latest_jpeg || latest_jpeg_len == 0) return false;
    if (xSemaphoreTake(jpeg_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint8_t *copy = (uint8_t *)heap_caps_malloc(latest_jpeg_len, MALLOC_CAP_SPIRAM);
        if (copy) {
            memcpy(copy, latest_jpeg, latest_jpeg_len);
            *out_buf = copy;
            *out_len = latest_jpeg_len;
            xSemaphoreGive(jpeg_mutex);
            return true;
        }
        xSemaphoreGive(jpeg_mutex);
    }
    return false;
}

static void save_face_crop(camera_fb_t *fb, uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh, uint32_t face_id) {
    if (fb->format != PIXFORMAT_RGB565) return;

    int crop_w = fw;
    int crop_h = fh;
    if (crop_w < 4) crop_w = 4;
    if (crop_h < 4) crop_h = 4;
    if (fx + crop_w > fb->width) crop_w = fb->width - fx;
    if (fy + crop_h > fb->height) crop_h = fb->height - fy;

    uint8_t *crop_buf = (uint8_t *)heap_caps_malloc(crop_w * crop_h * 2, MALLOC_CAP_SPIRAM);
    if (!crop_buf) return;

    for (int y = 0; y < crop_h; y++) {
        memcpy(crop_buf + y * crop_w * 2,
               fb->buf + (fy + y) * fb->width * 2 + fx * 2,
               crop_w * 2);
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool ok = fmt2jpg(crop_buf, crop_w * crop_h * 2, crop_w, crop_h,
                      PIXFORMAT_RGB565, 60, &jpg_buf, &jpg_len);

    heap_caps_free(crop_buf);

    if (ok && jpg_buf) {
        bool saved = face_db_save_image(face_id, jpg_buf, jpg_len);
        ESP_LOGI(TAG, "Face crop: id=%lu %dx%d jpg=%lu saved=%d",
                (unsigned long)face_id, crop_w, crop_h, (unsigned long)jpg_len, saved);
        free(jpg_buf);
    } else {
        ESP_LOGW(TAG, "Face crop encode failed: id=%lu %dx%d", (unsigned long)face_id, crop_w, crop_h);
    }
}

static void stream_encoder_task(void *arg) {
    ESP_LOGI(TAG, "Stream encoder on Core %d", xPortGetCoreID());
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        if (frame2jpg(fb, 80, &jpg, &jpg_len) && jpg && jpg_len > 0) {
            if (xSemaphoreTake(jpeg_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
                free(latest_jpeg);
                latest_jpeg = jpg;
                latest_jpeg_len = jpg_len;
                xSemaphoreGive(jpeg_mutex);
            } else {
                free(jpg);
            }
        }
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void face_detect_task(void *arg) {
    ESP_LOGI(TAG, "Face detect task on Core %d", xPortGetCoreID());
    FaceDetectResult result;
    uint32_t detectCount = 0;
    int64_t detectStart = esp_timer_get_time();

    while (1) {
        if (!detectActive) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        detect_faces(fb, &result);

        for (int i = 0; i < result.count; i++) {
            totalDetected++;
            int idx = face_db_find_similar(result.embeddings[i]);
            if (idx >= 0) {
                faceDB[idx].sample_count++;
                if (faceDB[idx].sample_count > MULTI_SAMPLES) {
                    faceDB[idx].sample_count = MULTI_SAMPLES;
                }
                float weight_new = 0.3f;
                float weight_old = 1.0f - weight_new;
                for (int d = 0; d < FEATURE_DIM; d++) {
                    faceDB[idx].features[d] = faceDB[idx].features[d] * weight_old +
                                               result.embeddings[i][d] * weight_new;
                }
                float norm = 0;
                for (int d = 0; d < FEATURE_DIM; d++) norm += faceDB[idx].features[d] * faceDB[idx].features[d];
                norm = sqrtf(norm);
                if (norm > 0.001f) {
                    for (int d = 0; d < FEATURE_DIM; d++) faceDB[idx].features[d] /= norm;
                }
                face_db_save();

                snprintf(lastEvent, sizeof(lastEvent),
                        "Known face #%lu at (%d,%d)",
                        (unsigned long)faceDB[idx].id, result.x[i], result.y[i]);
                strcpy(eventType, "known");
            } else {
                int newIdx = face_db_add(result.embeddings[i],
                                        result.x[i], result.y[i],
                                        result.w[i], result.h[i]);
                if (newIdx >= 0) {
                    save_face_crop(fb, result.x[i], result.y[i],
                                  result.w[i], result.h[i],
                                  faceDB[newIdx].id);
                    snprintf(lastEvent, sizeof(lastEvent),
                            "NEW face #%lu at (%d,%d)",
                            (unsigned long)faceDB[newIdx].id, result.x[i], result.y[i]);
                    strcpy(eventType, "new");
                }
            }
        }
        if (result.count == 0) {
            snprintf(lastEvent, sizeof(lastEvent), "No face detected");
            strcpy(eventType, "detect");
        }

        esp_camera_fb_return(fb);

        detectCount++;
        int64_t now = esp_timer_get_time();
        if (now - detectStart >= 1000000) {
            detectFps = detectCount;
            detectCount = 0;
            detectStart = now;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void wifi_init_sta(void) {
    ESP_LOGI(TAG, "WiFi init STA");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
}

static void wifi_init_ap(void) {
    ESP_LOGI(TAG, "WiFi init AP");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID, .ssid_len = strlen(AP_SSID), .channel = 1,
            .password = AP_PASSWORD, .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    if (strlen(AP_PASSWORD) == 0) wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void event_handler(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-CAM Face Detection System ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    face_db_init();
    jpeg_mutex = xSemaphoreCreateMutex();

    if (!camera_init()) {
        ESP_LOGE(TAG, "Camera init failed!");
        return;
    }

    xTaskCreatePinnedToCore(stream_encoder_task, "jpg_enc", 8192, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(face_detect_task, "detect", TASK_DETECT_STACK, NULL, TASK_DETECT_PRIO, NULL, 1);
    xTaskCreatePinnedToCore(web_server_task, "web", TASK_WEB_SERVER_STACK, NULL, TASK_WEB_SERVER_PRIO, NULL, 0);

    if (strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
        wifi_init_sta();
    } else {
        wifi_init_ap();
    }

    esp_event_handler_instance_t i1, i2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &i1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &i2));

    ESP_LOGI(TAG, "System started! Free heap: %lu", (unsigned long)esp_get_free_heap_size());
}
