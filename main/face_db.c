#include "app_config.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include <string.h>
#include <math.h>
#include <sys/stat.h>

FaceRecord faceDB[HARD_MAX_FACES];
uint32_t faceCount = 0;
uint32_t nextFaceId = 1;
uint32_t maxFacesAllowed = 50;
storage_type_t storageType = STORAGE_NONE;

static const char *DB_FILE_SPIFFS = "/storage/facedb.dat";
static const char *DB_FILE_SD = SD_MOUNT_POINT "/facedb.dat";
static const char *IMG_DIR_SD = SD_MOUNT_POINT "/faces";
static sdmmc_card_t *sd_card = NULL;

static const char* get_db_path(void) {
    return (storageType == STORAGE_SD) ? DB_FILE_SD : DB_FILE_SPIFFS;
}

static const char* get_img_path(uint32_t face_id, char *buf, size_t buflen) {
    if (storageType == STORAGE_SD) {
        snprintf(buf, buflen, "%s/f%lu.jpg", IMG_DIR_SD, (unsigned long)face_id);
    } else {
        snprintf(buf, buflen, "/storage/f%lu.jpg", (unsigned long)face_id);
    }
    return buf;
}

static void calc_max_faces(void) {
    uint64_t total_bytes = 0;

    if (storageType == STORAGE_SD && sd_card) {
        total_bytes = (uint64_t)sd_card->csd.capacity * sd_card->csd.sector_size;
        maxFacesAllowed = (uint32_t)(total_bytes / 12000);
        if (maxFacesAllowed > HARD_MAX_FACES) maxFacesAllowed = HARD_MAX_FACES;
        if (maxFacesAllowed < 10) maxFacesAllowed = 10;
    } else if (storageType == STORAGE_SPIFFS) {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        total_bytes = total;
        uint64_t free_bytes = total - used;
        maxFacesAllowed = (uint32_t)(free_bytes / 15000);
        if (maxFacesAllowed > HARD_MAX_FACES) maxFacesAllowed = HARD_MAX_FACES;
        if (maxFacesAllowed < 10) maxFacesAllowed = 10;
    } else {
        maxFacesAllowed = 0;
    }

    ESP_LOGI("DB", "Storage: %s, max_faces=%lu",
             storageType == STORAGE_SD ? "SD" : "SPIFFS",
             (unsigned long)maxFacesAllowed);
}

static bool init_sd_card(void) {
    ESP_LOGI("DB", "Trying SD card init...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGW("DB", "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGW("DB", "SD mount failed: %s. Using SPIFFS.", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return false;
    }

    sdmmc_card_print_info(stdout, sd_card);
    storageType = STORAGE_SD;

    mkdir(IMG_DIR_SD, 0755);

    ESP_LOGI("DB", "SD card mounted OK");
    return true;
}

void face_db_init(void) {
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 15,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE("DB", "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI("DB", "SPIFFS: total=%lu used=%lu", (unsigned long)total, (unsigned long)used);
    }

    if (!init_sd_card()) {
        storageType = STORAGE_SPIFFS;
    }

    calc_max_faces();
    face_db_load();
}

void face_db_load(void) {
    const char *path = get_db_path();
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGI("DB", "No database found at %s", path);
        return;
    }

    fread(faceDB, sizeof(FaceRecord), HARD_MAX_FACES, f);
    fclose(f);

    faceCount = 0;
    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (faceDB[i].valid) {
            faceCount++;
            if (faceDB[i].id >= nextFaceId) {
                nextFaceId = faceDB[i].id + 1;
            }
        }
    }
    ESP_LOGI("DB", "Loaded %lu faces from %s (max=%lu)",
             (unsigned long)faceCount, path, (unsigned long)maxFacesAllowed);
}

bool face_db_save(void) {
    const char *path = get_db_path();
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE("DB", "Save failed: %s", path);
        return false;
    }

    fwrite(faceDB, sizeof(FaceRecord), HARD_MAX_FACES, f);
    fclose(f);
    ESP_LOGI("DB", "Saved %lu faces", (unsigned long)faceCount);
    return true;
}

bool face_db_save_image(uint32_t face_id, uint8_t *jpg_buf, size_t jpg_len) {
    char path[128];
    get_img_path(face_id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE("DB", "Save image failed: %s", path);
        return false;
    }
    fwrite(jpg_buf, 1, jpg_len, f);
    fclose(f);
    ESP_LOGI("DB", "Saved image: %s (%lu bytes)", path, (unsigned long)jpg_len);
    return true;
}

bool face_db_delete_image(uint32_t face_id) {
    char path[128];
    get_img_path(face_id, path, sizeof(path));
    return remove(path) == 0;
}

void face_db_delete_oldest(void) {
    int oldestIdx = -1;
    uint32_t oldestTime = UINT32_MAX;
    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (faceDB[i].valid && faceDB[i].timestamp < oldestTime) {
            oldestTime = faceDB[i].timestamp;
            oldestIdx = i;
        }
    }
    if (oldestIdx >= 0) {
        face_db_delete_image(faceDB[oldestIdx].id);
        ESP_LOGI("DB", "Deleted oldest face id=%lu", (unsigned long)faceDB[oldestIdx].id);
        faceDB[oldestIdx].valid = 0;
        faceCount--;
    }
}

float cosine_similarity(float *a, float *b, int dim) {
    float dot = 0, normA = 0, normB = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    if (normA == 0 || normB == 0) return 0;
    return dot / (sqrtf(normA) * sqrtf(normB));
}

int face_db_find_similar(float *features) {
    float bestScore = 0;
    int bestIdx = -1;

    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (!faceDB[i].valid) continue;
        float score = cosine_similarity(features, faceDB[i].features, FEATURE_DIM);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }

    if (bestScore >= SIMILARITY_THRESHOLD) {
        ESP_LOGI("DB", "Match: id=%lu score=%.3f", (unsigned long)faceDB[bestIdx].id, bestScore);
        return bestIdx;
    }
    return -1;
}

int face_db_add(float *features, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (maxFacesAllowed == 0) return -1;

    if (faceCount >= maxFacesAllowed) {
        face_db_delete_oldest();
    }

    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (!faceDB[i].valid) {
            faceDB[i].id = nextFaceId++;
            memcpy(faceDB[i].features, features, sizeof(float) * FEATURE_DIM);
            faceDB[i].timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
            faceDB[i].x = x;
            faceDB[i].y = y;
            faceDB[i].w = w;
            faceDB[i].h = h;
            faceDB[i].valid = 1;
            faceDB[i].sample_count = 1;
            faceCount++;

            if (faceCount % 5 == 0) face_db_save();
            ESP_LOGI("DB", "New face: id=%lu total=%lu max=%lu",
                     (unsigned long)faceDB[i].id, (unsigned long)faceCount,
                     (unsigned long)maxFacesAllowed);
            return i;
        }
    }

    ESP_LOGW("DB", "Database full!");
    return -1;
}

void face_db_clear(void) {
    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (faceDB[i].valid) {
            face_db_delete_image(faceDB[i].id);
            faceDB[i].valid = 0;
        }
    }
    faceCount = 0;
    nextFaceId = 1;

    remove(get_db_path());
    ESP_LOGI("DB", "Database cleared");
}
