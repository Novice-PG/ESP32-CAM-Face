#include "app_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WEB";

extern char lastEvent[128];
extern char eventType[16];
extern volatile uint32_t streamFps;
extern volatile uint32_t detectFps;

static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-CAM Face</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:Arial,sans-serif;background:#0f0f1a;color:#eee;min-height:100vh}"
".top{display:flex;align-items:center;justify-content:space-between;padding:12px 20px;background:#16213e}"
".top h1{font-size:20px;color:#4ecca3}"
".fps-box{display:flex;gap:16px;font-size:14px}"
".fps-box span{background:#0a0a1a;padding:4px 12px;border-radius:4px}"
".fps-val{color:#e94560;font-weight:bold}"
".main{display:flex;gap:16px;padding:16px;flex-wrap:wrap}"
".left{flex:1;min-width:300px}"
".right{width:320px;min-width:280px}"
".card{background:#16213e;border-radius:10px;padding:14px;margin-bottom:14px}"
".card h2{font-size:15px;color:#4ecca3;margin-bottom:10px;border-bottom:1px solid #233;padding-bottom:6px}"
".stream{width:100%;border-radius:8px;border:2px solid #233;background:#000}"
".btns{display:flex;flex-wrap:wrap;gap:8px}"
".btn{background:#0f3460;color:#fff;border:none;padding:8px 16px;border-radius:5px;cursor:pointer;font-size:13px}"
".btn:hover{background:#e94560}"
".btn.danger{background:#8b0000}"
".btn.danger:hover{background:#ff0000}"
"#status{font-family:monospace;font-size:13px;color:#aaa;margin-top:8px;line-height:1.6}"
".face-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:10px}"
".face-item{background:#0a0a1a;border-radius:8px;overflow:hidden;cursor:pointer;border:2px solid transparent;transition:border-color .2s}"
".face-item:hover{border-color:#4ecca3}"
".face-item.sel{border-color:#e94560}"
".face-item img{width:100%;aspect-ratio:1;object-fit:cover;display:block}"
".face-item .info{padding:6px;font-size:11px;text-align:center}"
".face-detail{display:none;margin-top:10px;text-align:center}"
".face-detail img{max-width:200px;border-radius:8px;border:2px solid #4ecca3}"
".face-detail .meta{margin-top:8px;font-size:13px;color:#aaa}"
".btn-dl{display:inline-block;margin-top:10px;padding:6px 14px;background:#0f3460;color:#fff;border:none;border-radius:5px;cursor:pointer;font-size:13px;text-decoration:none}"
".btn-dl:hover{background:#4ecca3}"
"</style></head><body>"
"<div class='top'>"
"<h1>ESP32-CAM Face</h1>"
"<div class='fps-box'>"
"<span>Stream: <span class='fps-val' id='sFps'>0</span> FPS</span>"
"<span>Detect: <span class='fps-val' id='dFps'>0</span> FPS</span>"
"<span>Faces: <span class='fps-val' id='fCount'>0</span>/<span id='fMax'>0</span></span>"
"<span id='storageBox' style='color:#4ecca3'>SPIFFS</span>"
"</div></div>"
"<div class='main'>"
"<div class='left'>"
"<div class='card'><img id='stream' class='stream'></div>"
"<div class='card'>"
"<h2>Controls</h2>"
"<div class='btns'>"
"<button class='btn' onclick='toggleDetect()'>Toggle Detect</button>"
"<button class='btn danger' onclick='clearDB()'>Clear All Faces</button>"
"</div>"
"<div id='status'>Loading...</div>"
"</div>"
"<div class='card'>"
"<h2>Last Event</h2>"
"<div id='event' style='font-size:13px;color:#aaa'>-</div>"
"</div></div>"
"<div class='right'>"
"<div class='card'>"
"<h2>Face Gallery (<span id='gCount'>0</span>)</h2>"
"<div class='face-grid' id='gallery'></div>"
"<div class='face-detail' id='detail'>"
"<img id='detailImg' src=''>"
"<div class='meta' id='detailMeta'></div>"
"<a id='detailDl' class='btn-dl' href='#' download='face.jpg'>Download Image</a>"
"</div>"
"</div></div></div>"
"<script>"
"var H=location.hostname;"
"document.getElementById('stream').src='http://'+H+':81/';"
"fetch('/stats').then(function(r){return r.json()}).then(initStats).catch(function(){});"
"function initStats(d){"
"document.getElementById('status').textContent='Ready';"
"document.getElementById('sFps').textContent=d.sfps;"
"document.getElementById('dFps').textContent=d.dfps;"
"document.getElementById('fCount').textContent=d.faces;"
"document.getElementById('fMax').textContent=d.max;"
"document.getElementById('storageBox').textContent=d.storage;"
"document.getElementById('event').textContent=d.last+' ['+d.type+']';"
"setInterval(function(){fetch('/stats').then(function(r){return r.json()}).then(function(d){"
"document.getElementById('sFps').textContent=d.sfps;"
"document.getElementById('dFps').textContent=d.dfps;"
"document.getElementById('fCount').textContent=d.faces;"
"document.getElementById('fMax').textContent=d.max;"
"document.getElementById('storageBox').textContent=d.storage;"
"document.getElementById('event').textContent=d.last+' ['+d.type+']'})},1000)}"
"function toggleDetect(){"
"fetch('/toggle_detect').then(function(r){return r.text()}).then(function(t){"
"document.getElementById('status').textContent='Detection: '+t})}"
"function clearDB(){"
"if(!confirm('Delete ALL stored faces?'))return;"
"fetch('/clear_db').then(function(r){return r.text()}).then(function(t){"
"document.getElementById('status').textContent=t;loadGallery()})}"
"function loadGallery(){"
"fetch('/faces_list').then(function(r){return r.json()}).then(function(data){"
"var g=document.getElementById('gallery');g.innerHTML='';"
"document.getElementById('gCount').textContent=data.count;"
"data.faces.forEach(function(f){"
"var d=document.createElement('div');d.className='face-item';"
"d.onclick=function(){selectFace(f.id)};"
"d.innerHTML='<img src=\"/face_image?id='+f.id+'\">'"
"+'<div class=\"info\">#'+f.id+'</div>';g.appendChild(d)})}).catch(function(){})}"
"function selectFace(id){"
"document.getElementById('detail').style.display='block';"
"document.getElementById('detailImg').src='/face_image?id='+id;"
"document.getElementById('detailDl').href='/face_image?id='+id;"
"var items=document.querySelectorAll('.face-item');"
"items.forEach(function(f){f.classList.remove('sel')});"
"event.currentTarget.classList.add('sel');"
"fetch('/stats').then(function(r){return r.json()}).then(function(d){"
"document.getElementById('detailMeta').textContent="
"'ID: #'+id+' | Stored: '+d.faces+'/'+d.max+' | '+d.storage})}"
"loadGallery();setInterval(loadGallery,5000);"
"</script></body></html>";

static int send_all(int fd, const void *buf, int len) {
    int sent = 0;
    const char *p = (const char *)buf;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static void send_response(int fd, const char *type, const char *body, int len) {
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", type, len);
    if (send_all(fd, hdr, hlen) > 0 && len > 0)
        send_all(fd, body, len);
}

static void handle_index(int fd) {
    int len = strlen(PAGE_HTML);
    send_response(fd, "text/html", PAGE_HTML, len);
}

static void handle_stats(int fd) {
    char json[256];
    const char *storage = "SPIFFS";
    if (storageType == STORAGE_SD) storage = "SD";
    else if (storageType == STORAGE_NONE) storage = "None";
    int len = snprintf(json, sizeof(json),
        "{\"faces\":%lu,\"max\":%lu,\"last\":\"%s\",\"type\":\"%s\","
        "\"sfps\":%lu,\"dfps\":%lu,\"storage\":\"%s\"}",
        (unsigned long)faceCount, (unsigned long)maxFacesAllowed,
        lastEvent, eventType,
        (unsigned long)streamFps, (unsigned long)detectFps, storage);
    send_response(fd, "application/json", json, len);
}

static void handle_toggle(int fd) {
    detectActive = !detectActive;
    const char *r = detectActive ? "ON" : "OFF";
    send_response(fd, "text/plain", r, detectActive ? 2 : 3);
}

static void handle_clear(int fd) {
    face_db_clear();
    send_response(fd, "text/plain", "cleared", 7);
}

static void handle_faces_list(int fd) {
    char *json = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!json) json = (char *)malloc(8192);
    if (!json) { send_response(fd, "text/plain", "OOM", 3); return; }

    int pos = 0;
    pos += snprintf(json + pos, 8192 - pos,
        "{\"count\":%lu,\"faces\":[", (unsigned long)faceCount);
    int cnt = 0;
    for (int i = 0; i < HARD_MAX_FACES; i++) {
        if (faceDB[i].valid) {
            if (cnt++) json[pos++] = ',';
            pos += snprintf(json + pos, 8192 - pos,
                "{\"id\":%lu,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"ts\":%lu}",
                (unsigned long)faceDB[i].id,
                faceDB[i].x, faceDB[i].y, faceDB[i].w, faceDB[i].h,
                (unsigned long)faceDB[i].timestamp);
        }
        if (pos > 7900) break;
    }
    pos += snprintf(json + pos, 8192 - pos, "]}");
    send_response(fd, "application/json", json, pos);
    free(json);
}

static void handle_face_image(int fd, const char *query) {
    const char *p = strstr(query, "id=");
    if (!p) { send_response(fd, "text/plain", "Bad", 3); return; }
    char val[16] = {0};
    p += 3;
    int i = 0;
    while (*p && *p != '&' && i < 15) val[i++] = *p++;

    char path[128];
    if (storageType == STORAGE_SD) {
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/faces/f%s.jpg", val);
    } else {
        snprintf(path, sizeof(path), "/storage/f%s.jpg", val);
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "/storage/f%s.jpg", val);
        f = fopen(path, "rb");
    }
    if (!f) { send_response(fd, "text/plain", "Not found", 9); return; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: max-age=3600\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", fsize);
    send_all(fd, hdr, hlen);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, n) < 0) break;
    }
    fclose(f);
}

static void handle_client(int fd) {
    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) { close(fd); return; }
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }

    char *sp = strchr(buf, ' ');
    if (!sp) { close(fd); return; }
    *sp = 0;
    char *path = sp + 1;
    sp = strchr(path, ' ');
    if (sp) *sp = 0;
    char *query = strchr(path, '?');
    if (query) { *query = 0; query++; } else { query = ""; }

    if (strcmp(path, "/") == 0) handle_index(fd);
    else if (strcmp(path, "/stats") == 0) handle_stats(fd);
    else if (strcmp(path, "/toggle_detect") == 0) handle_toggle(fd);
    else if (strcmp(path, "/clear_db") == 0) handle_clear(fd);
    else if (strcmp(path, "/faces_list") == 0) handle_faces_list(fd);
    else if (strcmp(path, "/face_image") == 0) handle_face_image(fd, query);
    else {
        const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send_all(fd, r, strlen(r));
    }
    close(fd);
}

static void handle_client_task(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    handle_client(fd);
    vTaskDelete(NULL);
}

static void mjpeg_task(void *arg) {
    ESP_LOGI(TAG, "MJPEG task on Core %d", xPortGetCoreID());
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(81),
        .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 2);
    ESP_LOGI(TAG, "MJPEG :81");

    while (1) {
        struct sockaddr_in ca; socklen_t cla = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr *)&ca, &cla);
        if (cfd < 0) continue;

        struct timeval sndtv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv));

        const char *hdr = "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n";
        if (send_all(cfd, hdr, strlen(hdr)) < 0) { close(cfd); continue; }

        uint32_t fc = 0;
        int64_t t0 = esp_timer_get_time();

        while (1) {
            uint8_t *jpg = NULL;
            size_t jlen = 0;

            if (get_latest_jpeg_copy(&jpg, &jlen)) {
                char part[96];
                int pl = snprintf(part, sizeof(part),
                    "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                    (unsigned)jlen);
                if (send_all(cfd, part, pl) < 0 || send_all(cfd, jpg, jlen) < 0) {
                    free(jpg); break;
                }
                free(jpg);
                fc++;
            } else {
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }

            int64_t now = esp_timer_get_time();
            if (now - t0 >= 1000000) { streamFps = fc; fc = 0; t0 = now; }
        }
        close(cfd);
    }
}

void web_server_task(void *arg) {
    ESP_LOGI(TAG, "Web server on Core %d", xPortGetCoreID());
    xTaskCreatePinnedToCore(mjpeg_task, "mjpeg", 8192, NULL, 5, NULL, 0);

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 5);
    ESP_LOGI(TAG, "HTTP :80");

    while (1) {
        struct sockaddr_in ca; socklen_t cla = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr *)&ca, &cla);
        if (cfd < 0) continue;
        int *fdp = malloc(sizeof(int));
        if (fdp) { *fdp = cfd; xTaskCreatePinnedToCore(handle_client_task, "http_conn", 8192, fdp, 5, NULL, 1); }
        else { close(cfd); }
    }
}
