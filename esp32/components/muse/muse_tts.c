/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_settings.h"
#include "sdkconfig.h"

static const char *TAG = "muse_tts";

#define TEXT_MAX 2048            /* Deepgram takes up to 2000 characters */
#define MP3_BUF (32 * 1024)      /* audio fetched, not yet taken; the request waits while it's full */
#define READ_CHUNK 2048
#define TIMEOUT_MS 20000
#define ERROR_BODY 160           /* of a failed request's body, for the log */

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
/* Under s_lock: */
static uint32_t s_id;            /* the request wanted now; a new one drops the last */
static bool s_pending;           /* s_id's text is waiting for the task */
static muse_tts_state_t s_state;
static char *s_text;             /* TEXT_MAX */
static uint8_t *s_mp3;           /* MP3_BUF */
static size_t s_mp3_len;

bool muse_tts_configured(void)
{
#if CONFIG_MUSE_TTS_DEEPGRAM
    return CONFIG_MUSE_TTS_DEEPGRAM_KEY[0] != '\0';
#elif CONFIG_MUSE_TTS_OPENAI
    return CONFIG_MUSE_TTS_URL[0] != '\0';
#else
    return false;
#endif
}

/* The request's URL, its body (to free) and its Authorization header ("" for none). */
static char *request(const char *text, char *url, size_t url_cap, char *auth, size_t auth_cap)
{
    cJSON *body = cJSON_CreateObject();
    auth[0] = '\0';
#if CONFIG_MUSE_TTS_DEEPGRAM
    snprintf(url, url_cap, "https://api.deepgram.com/v1/speak?model=%s&encoding=mp3&bit_rate=48000",
             CONFIG_MUSE_TTS_DEEPGRAM_MODEL);
    snprintf(auth, auth_cap, "Token %s", CONFIG_MUSE_TTS_DEEPGRAM_KEY);
    cJSON_AddStringToObject(body, "text", text);
#elif CONFIG_MUSE_TTS_OPENAI
    strlcpy(url, CONFIG_MUSE_TTS_URL, url_cap);
    if (CONFIG_MUSE_TTS_API_KEY[0]) {
        snprintf(auth, auth_cap, "Bearer %s", CONFIG_MUSE_TTS_API_KEY);
    }
    cJSON_AddStringToObject(body, "model", CONFIG_MUSE_TTS_MODEL);
    if (!strcmp(CONFIG_MUSE_TTS_VOICE, "auto")) {
        /* The service tells each sentence's language; the settings say which when it can't. */
        char voice[16];
        snprintf(voice, sizeof(voice), "auto:%s", muse_settings_speech_lang());
        cJSON_AddStringToObject(body, "voice", voice);
    } else {
        cJSON_AddStringToObject(body, "voice", CONFIG_MUSE_TTS_VOICE);
    }
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "response_format", "mp3");
#else
    (void)text;
    (void)url;
    (void)url_cap;
    (void)auth_cap;
#endif
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return json;
}

/* Adds MP3 for request `id`, waiting while the buffer is full. False once it's been dropped. */
static bool add_mp3(uint32_t id, const uint8_t *data, size_t len)
{
    while (len) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (id != s_id) {
            xSemaphoreGive(s_lock);
            return false;
        }
        size_t n = MP3_BUF - s_mp3_len < len ? MP3_BUF - s_mp3_len : len;
        memcpy(s_mp3 + s_mp3_len, data, n);
        s_mp3_len += n;
        xSemaphoreGive(s_lock);
        data += n;
        len -= n;
        if (len) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return true;
}

/* Runs request `id` for `text`. True if any audio came: a cut-off stream still plays what it got. */
static bool fetch(uint32_t id, const char *text)
{
    static char url[256], auth[160];
    static uint8_t chunk[READ_CHUNK];
    char *body = request(text, url, sizeof(url), auth, sizeof(auth));
    if (!body) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = READ_CHUNK,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        return false;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "audio/mpeg, audio/wav");
    if (auth[0]) {
        esp_http_client_set_header(c, "Authorization", auth);
    }
    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    int len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK && esp_http_client_write(c, body, len) != len) {
        err = ESP_FAIL;
    }
    free(body);
    int status = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
    }
    if (err != ESP_OK || status == 0) {
        ESP_LOGW(TAG, "can't reach the TTS service (%s)", esp_err_to_name(err));
    } else if (status != 200) {
        int n = esp_http_client_read(c, (char *)chunk, ERROR_BODY);
        ESP_LOGW(TAG, "TTS service: HTTP %d %.*s", status, n > 0 ? n : 0, (const char *)chunk);
    } else {
        int n;
        while ((n = esp_http_client_read(c, (char *)chunk, sizeof(chunk))) > 0) {
            if (!add_mp3(id, chunk, n)) {
                break;   /* dropped */
            }
            if (!got) {
                ESP_LOGI(TAG, "first audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            got += n;
        }
        if (n < 0 || !esp_http_client_is_complete_data_received(c)) {
            ESP_LOGW(TAG, "TTS stream cut off after %u bytes", (unsigned)got);
        }
        ESP_LOGI(TAG, "%u bytes of speech for %u characters in %.2fs", (unsigned)got, (unsigned)strlen(text),
                 (esp_timer_get_time() - t0) / 1e6);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return got > 0;
}

static void tts_task(void *arg)
{
    (void)arg;
    static char text[TEXT_MAX];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool pending = s_pending;
        uint32_t id = s_id;
        if (pending) {
            strlcpy(text, s_text, sizeof(text));
            s_pending = false;
        }
        xSemaphoreGive(s_lock);
        if (!pending) {
            continue;
        }
        bool ok = fetch(id, text);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (id == s_id) {
            s_state = ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED;
        }
        xSemaphoreGive(s_lock);
    }
}

bool muse_tts_start(void)
{
    if (!muse_tts_configured()) {
        return false;
    }
    s_lock = xSemaphoreCreateMutex();
    s_text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3 = heap_caps_malloc(MP3_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* TLS runs here; its stack can be in PSRAM like the chat session's. */
    if (!s_lock || !s_text || !s_mp3 ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 12 * 1024, NULL, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        return false;
    }
#if CONFIG_MUSE_TTS_DEEPGRAM
    ESP_LOGI(TAG, "replies spoken by Deepgram (%s)", CONFIG_MUSE_TTS_DEEPGRAM_MODEL);
#elif CONFIG_MUSE_TTS_OPENAI
    ESP_LOGI(TAG, "replies spoken by %s (%s, %s)", CONFIG_MUSE_TTS_URL, CONFIG_MUSE_TTS_MODEL, CONFIG_MUSE_TTS_VOICE);
#endif
    return true;
}

uint32_t muse_tts_speak(const char *text)
{
    if (!s_task) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t id = ++s_id ? s_id : ++s_id;   /* never 0 */
    strlcpy(s_text, text, TEXT_MAX);
    s_pending = true;
    s_state = MUSE_TTS_BUSY;
    s_mp3_len = 0;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return id;
}

size_t muse_tts_take(uint32_t id, uint8_t *out, size_t cap, muse_tts_state_t *state)
{
    size_t n = 0;
    *state = MUSE_TTS_FAILED;
    if (!s_task) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (id == s_id) {
        n = s_mp3_len < cap ? s_mp3_len : cap;
        memcpy(out, s_mp3, n);
        memmove(s_mp3, s_mp3 + n, s_mp3_len - n);
        s_mp3_len -= n;
        *state = s_mp3_len || s_state == MUSE_TTS_BUSY ? MUSE_TTS_BUSY : s_state;
    }
    xSemaphoreGive(s_lock);
    return n;
}

void muse_tts_cancel(void)
{
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_id++;
    s_pending = false;
    s_mp3_len = 0;
    xSemaphoreGive(s_lock);
}
