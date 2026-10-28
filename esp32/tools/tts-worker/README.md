<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Free spoken replies on Cloudflare Workers AI

`worker.js` is a small Cloudflare Worker that speaks Muse replies with
Workers AI's MeloTTS (or a Deepgram Aura model) and answers like OpenAI's
`/v1/audio/speech`, which the firmware's `CONFIG_MUSE_TTS_OPENAI` setting
talks to. Cloudflare's free plan needs no card: its 10,000 daily "neurons"
cover hours of MeloTTS speech a day, and Workers get 100,000 free requests a
day. Long replies are spoken a few sentences at a time and streamed, so they
start playing quickly.

## Deploy it

Either way, you end up with a URL like
`https://muse-tts.<your-subdomain>.workers.dev` and a secret of your choosing.

**From the dashboard** (nothing to install):

1. Sign up at dash.cloudflare.com, then Compute (Workers) > Create > Start
   with Hello World. Name it `muse-tts` and deploy.
2. Edit code: replace everything with `worker.js`, then Deploy.
3. Settings > Bindings > Add > Workers AI, variable name `AI`. That's the
   only binding it needs: no D1 database, KV or R2.
4. Settings > Variables and Secrets > Add, type Secret, name `TTS_SECRET`,
   value a long random string (`openssl rand -hex 24` makes one).

**With wrangler**, from this directory:

```sh
npx wrangler login
npx wrangler deploy
npx wrangler secret put TTS_SECRET
```

Check it from a computer before building:

```sh
curl -sS https://muse-tts.<your-subdomain>.workers.dev/v1/audio/speech \
  -H "Authorization: Bearer <secret>" -H "Content-Type: application/json" \
  -d '{"input":"Bonjour de Muse.","voice":"auto"}' -o hello.wav
```

`hello.wav` should play (MeloTTS answers in WAV, Aura voices in MP3).

## Point the gadget at it

In the build directory's `sdkconfig` (or `idf.py menuconfig`, Muse > Spoken
replies), then rebuild and flash:

```
CONFIG_MUSE_TTS_OPENAI=y
CONFIG_MUSE_TTS_URL="https://muse-tts.<your-subdomain>.workers.dev/v1/audio/speech"
CONFIG_MUSE_TTS_API_KEY="<secret>"
CONFIG_MUSE_TTS_MODEL="melotts"
CONFIG_MUSE_TTS_VOICE="auto"
```

The voice picks the speech:

| Voice | Speech | Free each day |
|---|---|---|
| `auto` (or `auto:fr` to fall back to French) | MeloTTS in English, French or Spanish, picked per sentence from its words | hours |
| `en`, `fr`, `es`, ... | MeloTTS in that language | hours |
| `aura-1:asteria` (or another Aura 1 speaker) | Deepgram Aura 1 | ~7,000 characters |
| `aura-2-en:thalia` (or another Aura 2 speaker) | Deepgram Aura 2 | ~3,600 characters |

The allowance is shared by every Workers AI model on the account, and
Cloudflare sets the prices and the model list, so check
developers.cloudflare.com/workers-ai if speech stops.
