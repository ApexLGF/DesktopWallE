"""
Hermes platform adapter for the 热狗 (Hotdog) desk-robot.

This plugin makes the StackChan device appear to the Hermes gateway as
just another messaging channel — like Telegram, Feishu or IRC — so that
session memory, multi-turn context, mem0 hooks, skills and the agent
loop all work without any bridge-side scaffolding.

Topology (Shape A, decided 2026-05-16):

    StackChan (ws ocsc.v2)
        │
        ▼
    bridge daemon  (127.0.0.1:8766 — runs WS + Doubao ASR + Doubao TTS)
        │   ▲
        │   │ HTTP POST /say   (agent reply →  TTS → speaker)
        │   │ HTTP POST /show_image
        │   │ HTTP POST /listen / /capture  (follow-up mic, photo …)
        │   │
        ▼   │
    POST /inbox  (this adapter's local HTTP listener)
        │
        ▼
    Hermes gateway (agent runs in-process, session per device_id)

Why this design:

The previous bridge ran its own `voice_loop` that spawned `hermes -z` per
turn AND let the `hotdog` skill call `/say` directly — two TTS paths could
race, the 35 s subprocess timeout killed the agent mid-tool-call, and
session continuity was hand-rolled. By moving the agent into the gateway
and reducing bridge to a thin protocol-router we get session continuity,
proper cancellation, and a single authoritative TTS pipeline for free.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import time
from typing import Any, Dict, Optional

import aiohttp
from aiohttp import web

logger = logging.getLogger(__name__)

# Lazy-imported at top — these are stable Hermes APIs.
from gateway.platforms.base import (
    BasePlatformAdapter,
    MessageEvent,
    MessageType,
    SendResult,
)
from gateway.config import PlatformConfig, Platform


# --------------------------------------------------------------------------- #
# Config defaults                                                             #
# --------------------------------------------------------------------------- #
DEFAULT_BRIDGE_URL       = "http://127.0.0.1:8766"
DEFAULT_INBOX_HOST       = "127.0.0.1"
DEFAULT_INBOX_PORT       = 8770
DEFAULT_FOLLOWUP_SECONDS = 5.0

# Plugin platform name. The Platform enum has a `_missing_` hook that
# auto-creates a pseudo-member for bundled plugin platforms — see
# gateway/config.py.
PLATFORM_NAME = "hotdog"


def _read_cfg(cfg: PlatformConfig, key: str, default: Any) -> Any:
    """Read a key from extra/yaml or env-var. Adapters typically check both."""
    extra = getattr(cfg, "extra", None) or {}
    if key in extra and extra[key] not in (None, ""):
        return extra[key]
    env_key = f"HOTDOG_{key.upper()}"
    return os.environ.get(env_key) or default


# --------------------------------------------------------------------------- #
# Adapter                                                                     #
# --------------------------------------------------------------------------- #
class HotdogAdapter(BasePlatformAdapter):
    """One adapter instance covers all 热狗 devices the bridge knows about.

    Each device_id received from /inbox becomes the chat_id, so the gateway
    naturally maintains a separate session per physical robot.
    """

    def __init__(self, config: PlatformConfig) -> None:
        super().__init__(config, Platform(PLATFORM_NAME))
        self.bridge_url = _read_cfg(config, "bridge_url", DEFAULT_BRIDGE_URL).rstrip("/")
        self.inbox_host = _read_cfg(config, "inbox_host", DEFAULT_INBOX_HOST)
        self.inbox_port = int(_read_cfg(config, "inbox_port", DEFAULT_INBOX_PORT))
        self.followup_s = float(_read_cfg(config, "followup_seconds", DEFAULT_FOLLOWUP_SECONDS))
        self._http_session: Optional[aiohttp.ClientSession] = None
        self._inbox_runner: Optional[web.AppRunner] = None

    @property
    def name(self) -> str:
        return "Hotdog"

    # ── Connection lifecycle ────────────────────────────────────────────── #
    async def connect(self) -> bool:
        # Reusable HTTP client for talking to bridge.
        # Session-wide ceiling — per-request timeouts can be lower but
        # not higher. Long replies (a multi-paragraph poem, a 100 s
        # dance sequence) need /say to block for ~60-90 s; pick 240 s
        # so we never trip this when bumping the per-call timeout below.
        self._http_session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=240),
            headers={"X-Hermes-Plugin": "hotdog-platform"},
        )

        # Probe bridge so we fail fast if the daemon isn't up.
        try:
            async with self._http_session.get(f"{self.bridge_url}/status",
                                              timeout=aiohttp.ClientTimeout(total=4)) as r:
                data = await r.json()
                logger.info("Hotdog: bridge OK, %d device(s) online",
                            len(data.get("devices", [])))
        except Exception as e:
            logger.warning("Hotdog: bridge not reachable at %s yet (%s); will retry on next inbound",
                           self.bridge_url, e)

        # Start local inbox HTTP listener so the bridge can push new ASR
        # finals at us.
        app = web.Application()
        app.router.add_post("/inbox", self._handle_inbox)
        app.router.add_get("/healthz", lambda _r: web.json_response({"ok": True}))
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, self.inbox_host, self.inbox_port)
        try:
            await site.start()
        except Exception as e:
            logger.error("Hotdog: failed to bind inbox %s:%d — %s",
                         self.inbox_host, self.inbox_port, e)
            self._set_fatal_error("inbox_bind_failed", str(e), retryable=False)
            await runner.cleanup()
            return False
        self._inbox_runner = runner

        self._mark_connected()
        logger.info("Hotdog: ready — inbox on http://%s:%d, bridge %s",
                    self.inbox_host, self.inbox_port, self.bridge_url)
        return True

    async def disconnect(self) -> None:
        self._mark_disconnected()
        if self._inbox_runner is not None:
            try:
                await self._inbox_runner.cleanup()
            except Exception:
                pass
            self._inbox_runner = None
        if self._http_session is not None:
            try:
                await self._http_session.close()
            except Exception:
                pass
            self._http_session = None

    # ── Inbound from bridge ─────────────────────────────────────────────── #
    async def _handle_inbox(self, request: web.Request) -> web.Response:
        """POST /inbox  body {device_id, text, [user_name], [user_id]}.

        Bridge calls this when an ASR final transcript is ready.
        """
        try:
            body = await request.json()
        except Exception:
            return web.json_response({"ok": False, "error": "bad_json"}, status=400)

        device_id = str(body.get("device_id") or "").strip()
        text      = (body.get("text") or "").strip()
        if not device_id or not text:
            return web.json_response({"ok": False, "error": "device_id and text required"},
                                     status=400)
        user_id   = str(body.get("user_id") or device_id)
        user_name = str(body.get("user_name") or "热狗用户")

        # Hotdog is a *voice* channel — there is no way for the user to
        # respond to a "Dangerous command requires approval" prompt
        # mid-turn. Pre-approve any dangerous-command checks for this
        # device's agent session so the agent can run `terminal` calls
        # (e.g. `curl wttr.in/xian` for weather) without deadlocking on
        # an approval prompt that gets silently filtered downstream.
        try:
            from tools.approval import enable_session_yolo  # type: ignore
            session_key = f"agent:main:hotdog:dm:{device_id}"
            enable_session_yolo(session_key)
        except Exception:
            logger.debug("Hotdog: enable_session_yolo unavailable; "
                         "dangerous-command approvals may stall this channel")

        source = self.build_source(
            chat_id=device_id,
            chat_name=device_id,
            chat_type="dm",
            user_id=user_id,
            user_name=user_name,
        )
        event = MessageEvent(
            text=text,
            message_type=MessageType.TEXT,
            source=source,
            message_id=str(int(time.time() * 1000)),
            timestamp=__import__("datetime").datetime.now(),
        )
        try:
            await self.handle_message(event)
        except Exception:
            logger.exception("Hotdog: handle_message failed for %s", device_id)
            return web.json_response({"ok": False, "error": "handler_failed"}, status=500)
        return web.json_response({"ok": True, "device": device_id})

    # ── Outbound to device ──────────────────────────────────────────────── #
    # Substrings that mark a message as a gateway-internal notice the user
    # doesn't need to hear out loud. Kept tight on purpose: bare emoji
    # markers ("⚠️", "📢") were eating real agent output (e.g. the
    # `⚠️ Dangerous command requires approval` banner that the gateway
    # emits when the agent tries a sensitive tool — suppressing it caused
    # voice_loop to time out waiting for a reply that the user never got
    # to hear). Match on the actual *text* of each notice instead.
    _SYSTEM_MSG_MARKERS = (
        "Response formatting failed",
        "No home channel is set",
        "A home channel is where",
        "📬 No home channel",
    )

    @classmethod
    def _is_system_notice(cls, content: str) -> bool:
        stripped = content.lstrip("(（ \t")
        return any(m in stripped[:120] for m in cls._SYSTEM_MSG_MARKERS)

    async def send(
        self,
        chat_id: str,
        content: str,
        reply_to: Optional[str] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> SendResult:
        """Speak the agent's text reply on the named device, then arm a
        follow-up mic window so the user can chain another turn without
        re-uttering the wake-word."""
        if not content or not content.strip():
            return SendResult(success=True, message_id="empty")

        # Suppress gateway-internal notices so we don't burn a 30 s TTS
        # slot reading "📬 No home channel …" out loud.
        if self._is_system_notice(content):
            logger.info("Hotdog: suppressing gateway notice (%d chars): %s",
                        len(content), content[:80].replace("\n", " "))
            return SendResult(success=True, message_id="skipped_notice")

        # Strip simple markdown so TTS doesn't read "**bold**" literally.
        clean = self._strip_markdown(content.strip())

        # `/say` queues the reply into the bridge's TTS pipeline and
        # returns immediately when `nonblocking` is set. The bridge's
        # voice_loop is what owns the conversation rhythm: after the
        # speaker drains it opens its own follow-up mic and re-POSTs to
        # /inbox if the user speaks. That keeps the adapter stateless
        # and means a long reply (60 s of TTS) never trips an HTTP
        # timeout here, and the user can wait any length of time before
        # speaking the next turn — same session picks up because
        # chat_id == device_id.
        ok, err = await self._post_json(
            "/say", {"device": chat_id, "text": clean, "nonblocking": True}
        )
        if not ok:
            return SendResult(success=False, error=err or "say_failed")
        return SendResult(success=True, message_id=str(int(time.time() * 1000)))

    async def send_typing(self, chat_id: str, metadata=None) -> None:
        """No-op. Earlier this redrew a "thinking" face on the LCD, but the
        gateway calls send_typing as a periodic heartbeat (every few seconds
        while the agent runs) and each redraw pinned the device CPU long
        enough to stall the speaker DMA — long replies played choppy.

        If we want a thinking indicator later, do it once at the first call
        of a session and not on every heartbeat."""
        return None

    async def send_image(
        self,
        chat_id: str,
        image_url: str,
        caption: Optional[str] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> SendResult:
        ok, err = await self._post_json(
            "/show_image", None,
            params={"device": chat_id, "url": image_url},
        )
        if not ok:
            return SendResult(success=False, error=err or "image_failed")
        if caption:
            # Show caption text on the LCD AND speak it.
            await self.send(chat_id, caption)
        return SendResult(success=True, message_id=str(int(time.time() * 1000)))

    async def send_image_file(
        self,
        chat_id: str,
        path: str,
        caption: Optional[str] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> SendResult:
        ok, err = await self._post_json(
            "/show_image", None,
            params={"device": chat_id, "path": path},
        )
        if not ok:
            return SendResult(success=False, error=err or "image_failed")
        if caption:
            await self.send(chat_id, caption)
        return SendResult(success=True, message_id=str(int(time.time() * 1000)))

    async def get_chat_info(self, chat_id: str) -> Dict[str, Any]:
        return {"name": f"热狗 {chat_id}", "type": "dm", "chat_id": chat_id}

    # ── Helpers ─────────────────────────────────────────────────────────── #
    # Follow-up listen used to live here. It moved to the bridge so the
    # adapter stays stateless — bridge's voice_loop now waits for
    # tts.done after /say lands and opens its own follow-up mic.

    async def _post_json(
        self,
        path: str,
        body: Optional[dict],
        params: Optional[dict] = None,
        timeout: float = 30.0,
    ) -> tuple[bool, Optional[str]]:
        if self._http_session is None:
            return False, "http session not initialised"
        url = f"{self.bridge_url}{path}"
        try:
            async with self._http_session.post(
                url, json=body, params=params,
                timeout=aiohttp.ClientTimeout(total=timeout),
            ) as resp:
                if resp.status >= 400:
                    txt = (await resp.text())[:200]
                    return False, f"HTTP {resp.status}: {txt}"
                # We don't currently need the body, but consume it so the
                # connection can be re-used.
                with contextlib.suppress(Exception):
                    await resp.read()
                return True, None
        except asyncio.TimeoutError:
            return False, f"timeout calling {path}"
        except Exception as e:
            return False, f"{type(e).__name__}: {e}"

    @staticmethod
    def _strip_markdown(text: str) -> str:
        """Remove common markdown that would make TTS sound robotic."""
        text = re.sub(r"\*\*(.+?)\*\*", r"\1", text)
        text = re.sub(r"__(.+?)__",     r"\1", text)
        text = re.sub(r"\*(.+?)\*",     r"\1", text)
        text = re.sub(r"_(.+?)_",       r"\1", text)
        text = re.sub(r"`([^`]+?)`",    r"\1", text)
        text = re.sub(r"```.*?```",     "",    text, flags=re.S)
        # Strip bullet markers at line starts.
        text = re.sub(r"(?m)^\s*[-*]\s+", "", text)
        return text.strip()


# --------------------------------------------------------------------------- #
# Registration                                                                #
# --------------------------------------------------------------------------- #
def check_requirements() -> bool:
    # aiohttp ships with Hermes already (gateway uses it).
    return True


def validate_config(cfg: PlatformConfig) -> bool:
    """Always OK — adapter falls back to sane defaults for every field."""
    return True


def is_connected(adapter: HotdogAdapter) -> bool:
    return bool(getattr(adapter, "_running", False))


def register(ctx) -> None:
    """Plugin entry point: called by Hermes' plugin discovery."""
    ctx.register_platform(
        name=PLATFORM_NAME,
        label="Hotdog",
        adapter_factory=lambda cfg: HotdogAdapter(cfg),
        check_fn=check_requirements,
        validate_config=validate_config,
        is_connected=is_connected,
        required_env=[],
        install_hint="No extra packages needed (uses gateway's aiohttp).",
        emoji="🌭",
        # TTS chunks are roughly capped by Doubao; keep replies tight.
        max_message_length=2000,
        # The device is on the local LAN behind a trusted WiFi; no point
        # gating by user. Operators who want stricter ACLs can flip
        # HOTDOG_ALLOW_ALL_USERS=false and list specific device_ids.
        allowed_users_env="HOTDOG_ALLOWED_USERS",
        allow_all_env="HOTDOG_ALLOW_ALL_USERS",
        # Wire up cron / "home channel" delivery so the gateway stops
        # warning "📬 No home channel is set for Hotdog" on every restart.
        cron_deliver_env_var="HOTDOG_HOME_CHANNEL",
        # Hint to the LLM about how its replies will be consumed: spoken
        # aloud through a small speaker, so plain conversational Chinese
        # works best. Skill-tool calls (move_head / set_led / etc.) remain
        # available via the `hotdog` skill the device exposes.
        platform_hint=(
            "你正在通过桌面机器人「热狗」与用户对话。回复会被合成成中文语音"
            "通过小喇叭朗读出来，所以请用自然简短的口语化中文回答，不要"
            "使用 markdown、代码块或长列表。回复总长度尽量在 80 字以内；"
            "需要详细说明时分多句话说。"
            "想让机器人做表情、动作、亮 LED、显示图片可以调用 hotdog skill"
            "里的对应工具（不要再调 /say 端点，文字回复会自动朗读）。"
        ),
    )
