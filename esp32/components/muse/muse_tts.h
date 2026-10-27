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

#pragma once

/*
 * Spoken replies: reply text goes to the TTS service set in Kconfig (MUSE_TTS:
 * Deepgram, or an OpenAI-style /v1/audio/speech server such as Kokoro-FastAPI)
 * and the audio it streams back (MP3, or 16-bit WAV) is handed over as it
 * arrives. The request runs on a task of its own, so a slow service never
 * holds up the chat session.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_TTS_BUSY,     /* more audio may come */
    MUSE_TTS_DONE,     /* all of it has been taken */
    MUSE_TTS_FAILED,   /* the request failed (or was dropped); what came has been taken */
} muse_tts_state_t;

/* Whether a TTS service is set up. */
bool muse_tts_configured(void);

/* Starts the task; false if that fails or no service is set up. */
bool muse_tts_start(void);

/* Asks for `text` to be spoken, dropping any request in progress. Returns
 * the request's id, or 0 if there's no service. */
uint32_t muse_tts_speak(const char *text);

/* Takes up to cap bytes of request `id`'s audio into out; *state says whether
 * more may come. Never blocks. */
size_t muse_tts_take(uint32_t id, uint8_t *out, size_t cap, muse_tts_state_t *state);

/* Drops the request in progress. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
