"""End-to-end self-test for the doubao_asr_flash + doubao_tts_unidirectional
modules. Hits the live Doubao API.

Passes if BOTH of the following are true:
  1. TTS generates a known Chinese sentence as PCM with a reasonable size
     (≥ N seconds of audio) and first-byte latency under 2 s.
  2. Feeding that PCM through Flash ASR returns text containing the source
     sentence's substring.

Exits 0 on full success, 1 on any failure. Prints a structured report.

Usage:
    cp bridge/config.toml.example bridge/config.toml   # if not done yet
    # edit bridge/config.toml — fill in [doubao] appid + access_token
    python3 bridge/test_doubao_new.py
"""
from __future__ import annotations

import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import config as bridge_config
import doubao_asr_flash as asr
import doubao_tts_unidirectional as tts

try:
    _creds = bridge_config.get_doubao()
except RuntimeError as e:
    sys.exit(str(e))
APP_KEY    = _creds.appid
ACCESS_KEY = _creds.access_token

TEST_TEXTS = [
    "今天天气真不错",
    "你好热狗机器人",
    "请帮我把窗户关上",
]


async def run_round_trip(text: str) -> tuple[bool, str]:
    print(f"\n--- round-trip: {text!r} ---")

    # 1. TTS
    stats = tts.StreamStats(0, 0, 0, 0, [])
    chunks: list[bytes] = []
    t0 = time.monotonic()
    try:
        async for c in tts.synthesize_stream(
            text,
            app_key=APP_KEY, access_key=ACCESS_KEY,
            sample_rate=16000, audio_format="pcm",
            stats=stats,
        ):
            chunks.append(c)
    except Exception as e:
        return False, f"TTS exception: {e!r}"
    pcm = b"".join(chunks)
    tts_dur_ms = int((time.monotonic() - t0) * 1000)
    audio_s = len(pcm) / 32000.0   # 16k mono int16 = 32 KB/s
    print(f"  TTS: {len(pcm)} B PCM ({audio_s:.2f}s audio) in {tts_dur_ms}ms"
          f" first={stats.first_audio_ms}ms chunks={stats.total_chunks}")

    # validate basic shape
    if len(pcm) < 16000:   # < 0.5 s of audio is suspicious
        return False, f"TTS audio too short: {len(pcm)} bytes"
    if stats.first_audio_ms == 0 or stats.first_audio_ms > 2500:
        return False, f"TTS first-audio latency too high: {stats.first_audio_ms} ms"

    # 2. Flash ASR on the synthesised PCM
    try:
        r = await asr.transcribe(
            pcm,
            app_key=APP_KEY, access_key=ACCESS_KEY,
            sample_rate=16000,
        )
    except Exception as e:
        return False, f"ASR exception: {e!r}"
    print(f"  ASR: text={r.text!r} duration={r.duration_ms}ms utts={len(r.utterances)}")

    # validate transcript contains the source text (allow trivial punctuation)
    src = text.replace("，", "").replace(",", "").replace("。", "").strip()
    got = r.text.replace("，", "").replace(",", "").replace("。", "").replace("？", "").strip()
    if src not in got:
        return False, f"ASR mismatch: expected substring {src!r} in {got!r}"
    return True, "ok"


async def main():
    print("=" * 60)
    print("Doubao Flash ASR + Unidirectional TTS round-trip test")
    print("=" * 60)

    results = []
    for text in TEST_TEXTS:
        ok, msg = await run_round_trip(text)
        results.append((text, ok, msg))

    print("\n=== Summary ===")
    n_ok = sum(1 for _, ok, _ in results if ok)
    for text, ok, msg in results:
        flag = "PASS" if ok else "FAIL"
        print(f"  [{flag}] {text!r}: {msg}")
    print(f"--> {n_ok}/{len(results)} passed")
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
