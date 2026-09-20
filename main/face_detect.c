#include "app_config.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>

typedef struct {
    int x, y, w, h, area;
} Blob;

static void rgb565_to_ycbcr(uint16_t pixel, uint8_t *y, uint8_t *cb, uint8_t *cr) {
    uint8_t r = ((pixel >> 11) & 0x1F) << 3;
    uint8_t g = ((pixel >> 5) & 0x3F) << 2;
    uint8_t b = (pixel & 0x1F) << 3;
    *y  = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    *cb = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *cr = (uint8_t)((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

static uint8_t* create_skin_mask(uint8_t *rgb565, int width, int height) {
    uint8_t *mask = (uint8_t *)heap_caps_malloc(width * height, MALLOC_CAP_SPIRAM);
    if (!mask) mask = (uint8_t *)malloc(width * height);
    if (!mask) return NULL;

    for (int i = 0; i < width * height; i++) {
        uint16_t pixel = ((uint16_t)rgb565[i * 2] << 8) | rgb565[i * 2 + 1];
        uint8_t yv, cb, cr;
        rgb565_to_ycbcr(pixel, &yv, &cb, &cr);
        int isSkin = (cb >= 85 && cb <= 125 && cr >= 135 && cr <= 170 && yv > 80 && yv < 240) ? 1 : 0;
        mask[i] = isSkin * 255;
    }
    return mask;
}

static void morphological_close(uint8_t *mask, int width, int height) {
    uint8_t *tmp = (uint8_t *)malloc(width * height);
    if (!tmp) return;
    memcpy(tmp, mask, width * height);

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int val = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (tmp[(y + dy) * width + (x + dx)] > val) {
                        val = tmp[(y + dy) * width + (x + dx)];
                    }
                }
            }
            mask[y * width + x] = val;
        }
    }
    free(tmp);
}

static void morphological_open(uint8_t *mask, int width, int height) {
    uint8_t *tmp = (uint8_t *)malloc(width * height);
    if (!tmp) return;
    memcpy(tmp, mask, width * height);

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int val = 255;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (tmp[(y + dy) * width + (x + dx)] < val) {
                        val = tmp[(y + dy) * width + (x + dx)];
                    }
                }
            }
            mask[y * width + x] = val;
        }
    }
    free(tmp);
}

static int find_blobs(uint8_t *mask, int width, int height, Blob *blobs, int maxBlobs) {
    uint8_t *visited = (uint8_t *)calloc(width * height, 1);
    if (!visited) return 0;

    int blobCount = 0;

    for (int y = 0; y < height && blobCount < maxBlobs; y++) {
        for (int x = 0; x < width && blobCount < maxBlobs; x++) {
            if (mask[y * width + x] == 0 || visited[y * width + x]) continue;

            int minX = x, maxX = x, minY = y, maxY = y, area = 0;
            int stack[1024 * 2];
            int sp = 0;
            stack[sp++] = x;
            stack[sp++] = y;

            while (sp > 0) {
                int cy = stack[--sp];
                int cx = stack[--sp];
                if (cx < 0 || cx >= width || cy < 0 || cy >= height) continue;
                if (visited[cy * width + cx] || mask[cy * width + cx] == 0) continue;
                visited[cy * width + cx] = 1;
                area++;
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;
                if (sp < 1020) {
                    stack[sp++] = cx - 1; stack[sp++] = cy;
                    stack[sp++] = cx + 1; stack[sp++] = cy;
                    stack[sp++] = cx;     stack[sp++] = cy - 1;
                    stack[sp++] = cx;     stack[sp++] = cy + 1;
                }
            }

            int bw = maxX - minX + 1;
            int bh = maxY - minY + 1;
            if (area > 600 && bw > 28 && bh > 38) {
                float aspect = (float)bw / bh;
                float fillRatio = (float)area / (bw * bh);
                if (aspect > 0.65 && aspect < 1.4 && fillRatio > 0.40) {
                    int cy = (minY + maxY) / 2;
                    int cx = (minX + maxX) / 2;
                    if (cx > width * 0.05 && cx < width * 0.95 &&
                        cy > height * 0.05 && cy < height * 0.95) {
                        blobs[blobCount].x = minX;
                        blobs[blobCount].y = minY;
                        blobs[blobCount].w = bw;
                        blobs[blobCount].h = bh;
                        blobs[blobCount].area = area;
                        blobCount++;
                    }
                }
            }
        }
    }
    free(visited);
    return blobCount;
}

static uint8_t* rgb565_to_grayscale(uint8_t *rgb565, int width, int height) {
    uint8_t *gray = (uint8_t *)heap_caps_malloc(width * height, MALLOC_CAP_SPIRAM);
    if (!gray) gray = (uint8_t *)malloc(width * height);
    if (!gray) return NULL;

    for (int i = 0; i < width * height; i++) {
        uint16_t pixel = ((uint16_t)rgb565[i * 2] << 8) | rgb565[i * 2 + 1];
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        gray[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
    return gray;
}

void extract_features(uint8_t *rgb565, int imgW, int imgH,
                      uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh,
                      float *embedding) {
    memset(embedding, 0, sizeof(float) * FEATURE_DIM);

    uint8_t *gray = rgb565_to_grayscale(rgb565, imgW, imgH);
    if (!gray) return;

    int gridSize = 8;
    int cellW = fw / gridSize;
    int cellH = fh / gridSize;
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;

    int idx = 0;

    for (int gy = 0; gy < gridSize && idx < FEATURE_DIM; gy++) {
        for (int gx = 0; gx < gridSize && idx < FEATURE_DIM; gx++) {
            int startX = fx + gx * cellW;
            int startY = fy + gy * cellH;

            int hist[8] = {0};
            int validPixels = 0;
            for (int dy = 0; dy < cellH; dy += 2) {
                for (int dx = 0; dx < cellW; dx += 2) {
                    int px = startX + dx;
                    int py = startY + dy;
                    if (px >= imgW || py >= imgH) continue;
                    uint8_t val = gray[py * imgW + px];
                    int bin = val * 8 / 256;
                    if (bin > 7) bin = 7;
                    hist[bin]++;
                    validPixels++;
                }
            }

            if (validPixels > 0) {
                float total = (float)validPixels;
                for (int b = 0; b < 2 && idx < FEATURE_DIM; b++) {
                    embedding[idx++] = hist[b] / total;
                }
            }
        }
    }

    if (idx < FEATURE_DIM) {
        int gradBins[16] = {0};
        int gradCount = 0;
        for (int dy = 1; dy < fh - 1; dy += 2) {
            for (int dx = 1; dx < fw - 1; dx += 2) {
                int px = fx + dx;
                int py = fy + dy;
                if (px < 1 || px >= imgW - 1 || py < 1 || py >= imgH - 1) continue;
                int gx = gray[py * imgW + (px + 1)] - gray[py * imgW + (px - 1)];
                int gy2 = gray[(py + 1) * imgW + px] - gray[(py - 1) * imgW + px];
                float mag = sqrtf(gx * gx + gy2 * gy2);
                int bin = (int)(mag / 32.0f);
                if (bin > 15) bin = 15;
                gradBins[bin]++;
                gradCount++;
            }
        }
        if (gradCount > 0) {
            float total = (float)gradCount;
            for (int b = 0; b < 16 && idx < FEATURE_DIM; b++) {
                embedding[idx++] = gradBins[b] / total;
            }
        }
    }

    if (idx < FEATURE_DIM) {
        int avgY = 0, cnt = 0;
        for (int dy = 0; dy < fh; dy += 3) {
            for (int dx = 0; dx < fw; dx += 3) {
                int px = fx + dx;
                int py = fy + dy;
                if (px >= imgW || py >= imgH) continue;
                avgY += gray[py * imgW + px];
                cnt++;
            }
        }
        if (cnt > 0) {
            while (idx < FEATURE_DIM) {
                embedding[idx++] = (float)(avgY / cnt) / 255.0f;
            }
        }
    }

    float norm = 0;
    for (int i = 0; i < FEATURE_DIM; i++) norm += embedding[i] * embedding[i];
    norm = sqrtf(norm);
    if (norm > 0.001f) {
        for (int i = 0; i < FEATURE_DIM; i++) embedding[i] /= norm;
    }

    free(gray);
}

int detect_faces(camera_fb_t *fb, FaceDetectResult *result) {
    result->count = 0;

    uint8_t *skinMask = create_skin_mask(fb->buf, fb->width, fb->height);
    if (!skinMask) return 0;

    morphological_close(skinMask, fb->width, fb->height);
    morphological_open(skinMask, fb->width, fb->height);

    Blob blobs[32];
    int blobCount = find_blobs(skinMask, fb->width, fb->height, blobs, 32);
    free(skinMask);

    int count = 0;
    for (int i = 0; i < blobCount && count < 8; i++) {
        int bx = blobs[i].x, by = blobs[i].y, bw = blobs[i].w, bh = blobs[i].h;

        int eyeTop = by + bh * 1 / 5;
        int eyeBot = by + bh * 2 / 5;
        int mouthTop = by + bh * 3 / 5;
        int mouthBot = by + bh * 5 / 5;
        int faceW2 = bw / 2;

        int eyeSum = 0, mouthSum = 0, totalSum = 0, cnt = 0;
        for (int dy = 0; dy < bh; dy += 2) {
            for (int dx = 0; dx < bw; dx += 2) {
                int px = bx + dx;
                int py = by + dy;
                if (px < 0 || px >= fb->width || py < 0 || py >= fb->height) continue;
                uint16_t pixel = ((uint16_t)fb->buf[py * fb->width * 2 + px * 2] << 8) |
                                 fb->buf[py * fb->width * 2 + px * 2 + 1];
                uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                uint8_t b = (pixel & 0x1F) << 3;
                uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                totalSum += lum;
                cnt++;
                if (py >= eyeTop && py < eyeBot) eyeSum++;
                if (py >= mouthTop && py < mouthBot) mouthSum++;
            }
        }

        if (cnt == 0) continue;

        int symmetryScore = 0;
        int symCnt = 0;
        for (int dy = 0; dy < bh; dy += 3) {
            for (int dx = 0; dx < faceW2; dx += 3) {
                int leftPx = bx + dx;
                int rightPx = bx + bw - 1 - dx;
                int py = by + dy;
                if (leftPx < 0 || rightPx >= fb->width || py < 0 || py >= fb->height) continue;
                uint16_t lp = ((uint16_t)fb->buf[py * fb->width * 2 + leftPx * 2] << 8) |
                               fb->buf[py * fb->width * 2 + leftPx * 2 + 1];
                uint16_t rp = ((uint16_t)fb->buf[py * fb->width * 2 + rightPx * 2] << 8) |
                               fb->buf[py * fb->width * 2 + rightPx * 2 + 1];
                int lr = ((lp >> 11) & 0x1F) << 3;
                int lg = ((lp >> 5) & 0x3F) << 2;
                int lb = (lp & 0x1F) << 3;
                int rr = ((rp >> 11) & 0x1F) << 3;
                int rg = ((rp >> 5) & 0x3F) << 2;
                int rb = (rp & 0x1F) << 3;
                int lGray = (lr * 77 + lg * 150 + lb * 29) >> 8;
                int rGray = (rr * 77 + rg * 150 + rb * 29) >> 8;
                if (abs(lGray - rGray) < 40) symmetryScore++;
                symCnt++;
            }
        }
        float symRatio = (symCnt > 0) ? (float)symmetryScore / symCnt : 0;

        if (symRatio < 0.45) continue;

        result->x[count] = bx;
        result->y[count] = by;
        result->w[count] = bw;
        result->h[count] = bh;

        extract_features(fb->buf, fb->width, fb->height,
                        bx, by, bw, bh,
                        result->embeddings[count]);

        result->recognized[count] = 0;
        result->face_ids[count] = 0;
        count++;
    }

    result->count = count;
    return count;
}
