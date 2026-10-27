// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Spoken replies for Muse gadgets on Cloudflare's free Workers AI: an
// OpenAI-style POST /v1/audio/speech (what CONFIG_MUSE_TTS_OPENAI speaks to)
// in front of MeloTTS, or a Deepgram Aura model. Aura's MP3 passes through;
// MeloTTS's 44.1 kHz WAV goes out as one WAV stream at 16 kHz mono, the
// gadget's own rate, at a third of the size.
//
// The request's "voice" picks the speech:
//   "auto"                  MeloTTS, in English, French or Spanish as each
//                           part's words say ("auto:fr": French when unsure)
//   "en", "fr", "es", ...   MeloTTS in that language (the default, "en")
//   "aura-1:asteria"        Deepgram Aura 1 with that speaker
//   "aura-2-en:thalia"      Deepgram Aura 2
// MeloTTS's free daily allowance covers hours of speech; Aura's, a few
// thousand characters.
//
// The text is spoken a few sentences at a time and streamed as it goes, the
// first part kept short (about a sentence), so speech starts quickly however
// long the reply is.
// Set the TTS_SECRET secret and the gadget must send it as a Bearer token.

const MAX_INPUT = 4000;   // characters; more is cut off
const FIRST = 120;        // characters in the first part: speech starts once it's ready
const PART = 280;         // characters spoken per request to the model after that
const RATE = 16000;       // what WAV goes out at

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (request.method !== "POST" || url.pathname !== "/v1/audio/speech") {
      return text(404, "POST /v1/audio/speech");
    }
    if (env.TTS_SECRET && request.headers.get("Authorization") !== `Bearer ${env.TTS_SECRET}`) {
      return text(401, "unauthorized");
    }
    let body;
    try {
      body = await request.json();
    } catch {
      return text(400, "expected JSON");
    }
    const input = String(body.input ?? "").trim().slice(0, MAX_INPUT);
    if (!input) {
      return text(400, "no input");
    }
    const voice = String(body.voice || "en");
    const parts = split(input, FIRST, PART);

    // The first part before answering, so a failure is still an HTTP error.
    let first;
    try {
      first = await speak(env, parts[0], voice);
    } catch (e) {
      return text(502, `speech failed: ${e.message || e}`);
    }
    const wav = first.pcm !== undefined;
    const { readable, writable } = new TransformStream();
    const out = writable.getWriter();
    ctx.waitUntil((async () => {
      try {
        if (wav) {
          await out.write(wavHeader(RATE));
        }
        let next = first;
        for (let i = 1; ; i++) {
          await out.write(wav ? next.pcm : next.mp3);
          if (i === parts.length) {
            break;
          }
          next = await speak(env, parts[i], voice);
        }
        await out.close();
      } catch (e) {
        await out.abort(e);   // the gadget plays what it got
      }
    })());
    return new Response(readable, { headers: { "Content-Type": wav ? "audio/wav" : "audio/mpeg" } });
  },
};

function text(status, message) {
  return new Response(message + "\n", { status, headers: { "Content-Type": "text/plain" } });
}

// One part of the text as speech: { mp3 } bytes, or { pcm } at RATE, mono.
async function speak(env, part, voice) {
  let result;
  if (voice.startsWith("aura")) {
    const [model, speaker] = voice.split(":");
    const args = { text: part, encoding: "mp3" };
    if (speaker) {
      args.speaker = speaker;
    }
    result = await env.AI.run(`@cf/deepgram/${model}`, args);
  } else {
    const lang = voice.startsWith("auto") ? detectLang(part, voice.split(":")[1] || "en") : voice;
    result = await env.AI.run("@cf/myshell-ai/melotts", { prompt: part, lang });
  }
  const bytes = await audioBytes(result);
  const wav = parseWav(bytes);
  return wav ? { pcm: toRate(wav, RATE) } : { mp3: bytes };
}

// Common words and letters of the languages "auto" tells apart.
const LANGS = {
  en: { words: "the is are and you to of in that for it with this be on not have was but what your today",
        letters: "" },
  fr: { words: "le les des est et une pour qui vous je pas dans ce avec sur au du il elle nous mais très c'est aujourd'hui ou",
        letters: "èêàâçîïôûùœë" },
  es: { words: "el los las es y una para usted yo con por del está muy hoy pero como más también eso esta",
        letters: "ñáíóú¿¡" },
};

// The language of `text` among LANGS, or `fallback` when it can't tell.
function detectLang(text, fallback) {
  const lower = text.toLowerCase();
  const words = lower.split(/[^a-zà-ÿœ']+/);
  let best = fallback, bestScore = 0, tie = false;
  for (const [lang, { words: list, letters }] of Object.entries(LANGS)) {
    const set = new Set(list.split(" "));
    let score = words.reduce((n, w) => n + (set.has(w) ? 1 : 0), 0);
    for (const c of lower) {
      if (letters.includes(c)) {
        score += 2;
      }
    }
    if (score > bestScore) {
      best = lang;
      bestScore = score;
      tie = false;
    } else if (score === bestScore && score > 0) {
      tie = true;
    }
  }
  return bestScore && !tie ? best : fallback;
}

// Workers AI hands back audio as a stream, raw bytes, or base64 in an object.
async function audioBytes(result) {
  if (result instanceof ReadableStream || result instanceof Response) {
    return new Uint8Array(await new Response(result.body ?? result).arrayBuffer());
  }
  if (result instanceof ArrayBuffer) {
    return new Uint8Array(result);
  }
  if (result instanceof Uint8Array) {
    return result;
  }
  if (result && typeof result.audio === "string") {
    return fromBase64(result.audio);
  }
  throw new Error("no audio in the model's answer");
}

function fromBase64(text) {
  if (Uint8Array.fromBase64) {
    return Uint8Array.fromBase64(text);
  }
  const bin = atob(text);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    out[i] = bin.charCodeAt(i);
  }
  return out;
}

// A 16-bit PCM WAV's { rate, channels, samples }, or null if it isn't one.
function parseWav(b) {
  const tag = (o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
  if (b.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") {
    return null;
  }
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let fmt = null;
  for (let o = 12; o + 8 <= b.length; ) {
    const size = v.getUint32(o + 4, true);
    if (tag(o) === "fmt ") {
      fmt = { format: v.getUint16(o + 8, true), channels: v.getUint16(o + 10, true),
              rate: v.getUint32(o + 12, true), bits: v.getUint16(o + 22, true) };
    } else if (tag(o) === "data") {
      if (!fmt || fmt.format !== 1 || fmt.bits !== 16) {
        throw new Error("unsupported WAV");
      }
      const end = Math.min(b.length, o + 8 + size) & ~1;
      const samples = new Int16Array(b.slice(o + 8, end).buffer);   // a copy: aligned
      return { rate: fmt.rate, channels: fmt.channels, samples };
    }
    o += 8 + size + (size & 1);
  }
  throw new Error("WAV without data");
}

// Mono at `rate`, each output sample the average of the input it covers
// (which also keeps what the lower rate can't carry from aliasing back in).
function toRate(wav, rate) {
  const { samples, channels } = wav;
  const frames = Math.floor(samples.length / channels);
  if (wav.rate === rate && channels === 1) {
    return new Uint8Array(samples.buffer);
  }
  const step = wav.rate / rate;
  const n = Math.floor(frames / step);
  const out = new Int16Array(n);
  for (let i = 0, from = 0; i < n; i++) {
    const to = Math.min(frames, Math.floor((i + 1) * step));
    let sum = 0;
    for (let k = from * channels; k < to * channels; k++) {
      sum += samples[k];
    }
    out[i] = Math.round(sum / ((to - from) * channels || 1));
    from = to;
  }
  return new Uint8Array(out.buffer);
}

// A WAV header for a stream of unknown length.
function wavHeader(rate) {
  const b = new Uint8Array(44);
  const v = new DataView(b.buffer);
  const put = (o, t) => [...t].forEach((c, i) => (b[o + i] = c.charCodeAt(0)));
  put(0, "RIFF");
  v.setUint32(4, 0xffffffff, true);
  put(8, "WAVE");
  put(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);          // PCM
  v.setUint16(22, 1, true);          // mono
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * 2, true);
  v.setUint16(32, 2, true);
  v.setUint16(34, 16, true);
  put(36, "data");
  v.setUint32(40, 0xffffffff, true);
  return b;
}

// Splits text into parts of up to `first` characters for the first and `max`
// after it, at sentence ends where it can, else at spaces.
function split(input, first, max) {
  const sentences = input.match(/[^.!?\n]+[.!?]*[\])"']*\s*|\n+/g) || [input];
  const parts = [];
  let cur = "";
  const limit = () => (parts.length ? max : first);
  const push = () => {
    if (cur.trim()) {
      parts.push(cur.trim());
    }
    cur = "";
  };
  for (let s of sentences) {
    if (cur && cur.length + s.length > limit()) {
      push();
    }
    while (s.length > limit()) {
      // A sentence too long for one part: break it at the last space that fits.
      const at = s.lastIndexOf(" ", limit());
      const cut = at > 0 ? at : limit();
      cur = s.slice(0, cut);
      push();
      s = s.slice(cut);
    }
    cur += s;
  }
  push();
  return parts.length ? parts : [input.slice(0, first)];
}
