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

#include "muse_state.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "muse_text.h"

#define HAPPY_SECS 1.6f

#define AWAKE_BIT BIT0   /* while !s_asleep */
#define NUDGE_BIT BIT1

static volatile muse_mode_t s_mode = MUSE_MODE_BOOT;
static volatile int64_t s_mode_since_us;
static volatile float s_level;
static volatile float s_progress;
static volatile int64_t s_last_poke_us;
static volatile int64_t s_happy_until_us;
static volatile bool s_asleep;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_caption[MUSE_CAPTION_MAX];
static uint32_t s_caption_version;
static SemaphoreHandle_t s_format_lock;
static EventGroupHandle_t s_wake;
static volatile int s_page_cols = 16, s_page_lines = 2;
static muse_power_t s_power = { .battery_pct = -1 };
static volatile bool s_as_if_battery;
static volatile muse_reader_t s_reader;
static volatile bool s_reader_tapped;
static SemaphoreHandle_t s_reply_lock;
static char *s_reply, *s_reply_new;   /* MUSE_REPLY_MAX each, PSRAM if there is some */
static size_t s_reply_cap, s_reply_at;
static uint32_t s_reply_version;

static float secs_since(int64_t us)
{
    return (float)(esp_timer_get_time() - us) / 1e6f;
}

void muse_state_init(void)
{
    static StaticSemaphore_t lock, reply_lock;
    s_format_lock = xSemaphoreCreateMutexStatic(&lock);
    s_reply_lock = xSemaphoreCreateMutexStatic(&reply_lock);
    /* Two copies, swapped on a change, so the voice task never writes what the UI reads. */
    s_reply_cap = MUSE_REPLY_MAX;
    s_reply = heap_caps_calloc(2, s_reply_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_reply) {
        s_reply_cap = 2048;   /* no PSRAM: a shorter reply still scrolls */
        s_reply = calloc(2, s_reply_cap);
    }
    s_reply_new = s_reply ? s_reply + s_reply_cap : NULL;
    static StaticEventGroup_t wake;
    s_wake = xEventGroupCreateStatic(&wake);
    xEventGroupSetBits(s_wake, AWAKE_BIT);
    int64_t now = esp_timer_get_time();
    s_mode_since_us = now;
    s_last_poke_us = now;
}

void muse_state_set_mode(muse_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    /* Shutting down wins over the voice pipeline finishing a turn. */
    if (s_mode == MUSE_MODE_OFF && mode != MUSE_MODE_IDLE) {
        return;
    }
    s_mode_since_us = esp_timer_get_time();
    s_mode = mode;
}

muse_mode_t muse_state_mode(float *secs_in_mode)
{
    if (secs_in_mode) {
        *secs_in_mode = secs_since(s_mode_since_us);
    }
    return s_mode;
}

void muse_state_set_level(float level)
{
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
}

float muse_state_level(void)
{
    return s_level;
}

void muse_state_set_progress(float progress)
{
    s_progress = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
}

float muse_state_progress(void)
{
    return s_progress;
}

void muse_state_set_caption(const char *fmt, ...)
{
    static char buf[sizeof(s_caption)];   /* too big for some callers' stacks */
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    muse_text_to_ascii(buf, sizeof(buf));   /* replies have curly quotes and dashes */

    portENTER_CRITICAL(&s_lock);
    if (strcmp(buf, s_caption) != 0) {
        memcpy(s_caption, buf, sizeof(s_caption));
        s_caption_version++;
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_format_lock);
}

bool muse_state_caption(char *out, size_t out_len, uint32_t *version)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (*version != s_caption_version) {
        strlcpy(out, s_caption, out_len);
        *version = s_caption_version;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void muse_state_set_page(int cols, int lines)
{
    s_page_cols = cols;
    s_page_lines = lines;
}

void muse_state_page(int *cols, int *lines)
{
    *cols = s_page_cols;
    *lines = s_page_lines;
}

/* `text` with stand-ins into out, and where byte `at` of it lands there. */
static size_t shown_reply(const char *text, size_t at, char *out, size_t cap, size_t *out_at)
{
    const char *p = text;
    size_t o = 0;
    *out_at = 0;
    while (*p) {
        if ((size_t)(p - text) <= at) {
            *out_at = o;
        }
        size_t len;
        char sub[4];
        int n = muse_text_ascii(p, &len, sub);
        const char *put = n < 0 ? p : sub;
        size_t put_len = n < 0 ? len : (size_t)n;
        if (o + put_len >= cap) {
            break;
        }
        memcpy(out + o, put, put_len);
        o += put_len;
        p += len;
    }
    out[o] = '\0';
    if ((size_t)(p - text) <= at) {
        *out_at = o;
    }
    return o;
}

void muse_state_set_reply(const char *text, size_t at)
{
    if (!s_reply) {
        return;
    }
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    size_t shown_at;
    shown_reply(text, at, s_reply_new, s_reply_cap, &shown_at);
    if (strcmp(s_reply_new, s_reply) != 0) {
        char *t = s_reply;
        s_reply = s_reply_new;
        s_reply_new = t;
        s_reply_version++;
    }
    s_reply_at = shown_at;
    xSemaphoreGive(s_reply_lock);
}

bool muse_state_reply(char *out, size_t cap, size_t *at, uint32_t *version)
{
    if (!s_reply) {
        *at = 0;
        return false;
    }
    bool changed = false;
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    if (*version != s_reply_version) {
        strlcpy(out, s_reply, cap);
        *version = s_reply_version;
        changed = true;
    }
    *at = s_reply_at;
    xSemaphoreGive(s_reply_lock);
    return changed;
}

void muse_state_set_reader(muse_reader_t reader)
{
    s_reader = reader;
}

muse_reader_t muse_state_reader(void)
{
    return s_reader;
}

void muse_state_reader_tap(void)
{
    s_reader_tapped = true;
}

bool muse_state_reader_take_tap(void)
{
    bool tapped = s_reader_tapped;
    s_reader_tapped = false;
    return tapped;
}

void muse_state_set_power(const muse_power_t *power)
{
    bool was = muse_state_on_battery();
    portENTER_CRITICAL(&s_lock);
    s_power = *power;
    portEXIT_CRITICAL(&s_lock);
    if (muse_state_on_battery() != was) {
        muse_state_nudge();
    }
}

muse_power_t muse_state_power(void)
{
    portENTER_CRITICAL(&s_lock);
    muse_power_t p = s_power;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

bool muse_state_on_battery(void)
{
    muse_power_t p = muse_state_power();
    return s_as_if_battery || (!p.usb && p.battery_pct >= 0);
}

void muse_state_set_as_if_battery(bool on)
{
    if (on != s_as_if_battery) {
        s_as_if_battery = on;
        muse_state_nudge();
    }
}

void muse_state_poke(void)
{
    s_last_poke_us = esp_timer_get_time();
}

float muse_state_idle_secs(void)
{
    return secs_since(s_last_poke_us);
}

void muse_state_set_asleep(bool asleep)
{
    if (!asleep) {
        muse_state_poke();
    }
    s_asleep = asleep;
    if (asleep) {
        xEventGroupClearBits(s_wake, AWAKE_BIT);
    } else {
        xEventGroupSetBits(s_wake, AWAKE_BIT);
    }
}

bool muse_state_asleep(void)
{
    return s_asleep;
}

void muse_state_wait_awake(uint32_t timeout_ms)
{
    xEventGroupWaitBits(s_wake, AWAKE_BIT | NUDGE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_wake, NUDGE_BIT);
}

void muse_state_nudge(void)
{
    xEventGroupSetBits(s_wake, NUDGE_BIT);
}

void muse_state_make_happy(void)
{
    s_happy_until_us = esp_timer_get_time() + (int64_t)(HAPPY_SECS * 1e6f);
    muse_state_poke();
}

float muse_state_happiness(void)
{
    float left = (float)(s_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. */
    return left > 0.4f ? 1.0f : left / 0.4f;
}
