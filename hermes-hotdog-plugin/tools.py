"""
Hotdog platform tools — agent-facing API for the StackChan desk robot.

These get registered into the global tool registry by adapter.py's
``register(ctx)`` function. The agent on the ``hotdog`` platform sees them
as first-class tools (``hotdog_face``, ``hotdog_action``, …). Each handler
talks to the bridge daemon (HTTP at 127.0.0.1:8766) which owns the actual
WebSocket connection to the device.

The whole point: the agent never reaches for ``bash`` / ``curl`` / a ``sc``
skill script. Hardware control is exposed as native tools, voice replies
flow back through the platform's ``send()`` like Telegram or Feishu.
"""

from __future__ import annotations

import json
import logging
from typing import Any, Optional

import aiohttp

# Imported at runtime when adapter.py is loaded inside the Hermes gateway;
# the gateway puts hermes-agent's own ``tools`` package on sys.path.
from tools.registry import tool_error, tool_result  # type: ignore

logger = logging.getLogger(__name__)

# Set by ``register_all`` at plugin-load time so handlers know where to
# reach the bridge daemon. Defaults assume same-host (the standard
# deployment — bridge + Hermes gateway run on the same Mac mini).
BRIDGE_URL = "http://127.0.0.1:8766"


def set_bridge_url(url: str) -> None:
    """Override the bridge URL — called by adapter.register() with the
    HOTDOG_BRIDGE_URL the user configured (or the default)."""
    global BRIDGE_URL
    BRIDGE_URL = (url or BRIDGE_URL).rstrip("/")


async def _post_bridge(
    path: str,
    body: Optional[dict] = None,
    params: Optional[dict] = None,
    timeout: float = 10.0,
) -> dict:
    """POST to a bridge endpoint and return parsed JSON.

    Raises ``RuntimeError`` on HTTP >= 400 or transport failure so the
    caller can wrap into a ``tool_error``. We don't share a session across
    handlers because every call is a brief localhost RPC — pooling buys
    nothing here, and a per-call session sidesteps the "session bound to
    a closed loop" issue when Hermes restarts the gateway loop.
    """
    url = f"{BRIDGE_URL}{path}"
    try:
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=timeout)
        ) as sess:
            async with sess.post(url, json=body, params=params) as resp:
                txt = await resp.text()
                if resp.status >= 400:
                    raise RuntimeError(f"HTTP {resp.status}: {txt[:200]}")
                return json.loads(txt) if txt else {}
    except aiohttp.ClientError as e:
        raise RuntimeError(f"{path}: {type(e).__name__}: {e}")


# ---------------------------------------------------------------------------
# hotdog_face — render an expression on the LCD
# ---------------------------------------------------------------------------
HOTDOG_FACE_SCHEMA = {
    "name": "hotdog_face",
    "description": (
        "Render a StackChan robot face on the 320×240 LCD. Use to react "
        "emotionally (happy/sad/love/surprised), to signal internal state "
        "(thinking/sleep/talking), or as a moment of personality "
        "(wink_l/cat/embarrassed). The face renders directly to the device "
        "framebuffer — fast, no JPEG round-trip."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "expression": {
                "type": "string",
                "enum": [
                    "neutral", "happy", "smile", "love", "sad", "angry",
                    "surprised", "thinking", "sleep", "wink_l", "wink_r",
                    "stare", "dead", "embarrassed", "cat", "speak", "talking",
                ],
                "description": "Which expression to render.",
            },
            "eye_rgb":   {"type": "string", "description": "Eye color hex, e.g. '0x00ff00'. Optional."},
            "mouth_rgb": {"type": "string", "description": "Mouth color hex. Optional."},
            "bg_rgb":    {"type": "string", "description": "Background color hex. Optional."},
        },
        "required": ["expression"],
    },
}


async def _handle_hotdog_face(args: dict, **kw) -> str:
    expression = args.get("expression")
    if not expression:
        return tool_error("expression is required")
    params: dict[str, Any] = {"expression": expression}
    for k in ("eye_rgb", "mouth_rgb", "bg_rgb"):
        v = args.get(k)
        if v:
            params[k] = v
    try:
        await _post_bridge("/rpc", {"method": "face", "params": params})
    except Exception as e:
        return tool_error(str(e))
    return tool_result({"ok": True, "expression": expression})


# ---------------------------------------------------------------------------
# hotdog_action — predefined head-motion gesture
# ---------------------------------------------------------------------------
HOTDOG_ACTION_SCHEMA = {
    "name": "hotdog_action",
    "description": (
        "Run a predefined head-motion gesture. Each gesture takes 0.5–3 s "
        "and blocks until done. Use for emotional emphasis: nod to agree, "
        "shake to disagree, surprised when shocked, dance to celebrate, "
        "bow to greet, peek to investigate."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "enum": [
                    "home", "nod", "shake", "yes", "no",
                    "look_up", "look_down", "look_left", "look_right", "look_around",
                    "dance", "surprised", "sleep", "wake", "panic", "peek", "bow",
                    "tilt_left", "tilt_right",
                ],
            },
        },
        "required": ["name"],
    },
}


async def _handle_hotdog_action(args: dict, **kw) -> str:
    name = args.get("name")
    if not name:
        return tool_error("name is required")
    try:
        await _post_bridge(
            "/rpc",
            {"method": "action", "params": {"name": name}, "timeout": 12.0},
            timeout=15.0,
        )
    except Exception as e:
        return tool_error(str(e))
    return tool_result({"ok": True, "action": name})


# ---------------------------------------------------------------------------
# hotdog_led — animated effect on the 12-LED neon ring
# ---------------------------------------------------------------------------
HOTDOG_LED_SCHEMA = {
    "name": "hotdog_led",
    "description": (
        "Set the 12-LED neon ring effect on the StackChan base. Use animated "
        "effects to convey state (listening / thinking / talking / recording) "
        "or to celebrate (rainbow / sparkle / fire). 'off' disables. "
        "Pure solid color: pick 'solid' + r,g,b."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "enum": [
                    "off", "solid", "rainbow", "breathing", "pulse", "scanner",
                    "wipe", "sparkle", "police", "fire", "chase", "theater",
                    "listening", "thinking", "talking", "recording",
                ],
            },
            "r":         {"type": "integer", "minimum": 0, "maximum": 255, "description": "0..255 base red"},
            "g":         {"type": "integer", "minimum": 0, "maximum": 255},
            "b":         {"type": "integer", "minimum": 0, "maximum": 255},
            "speed_ms":  {"type": "integer", "description": "Frame tick in ms; 0 lets the effect choose."},
        },
        "required": ["name"],
    },
}


async def _handle_hotdog_led(args: dict, **kw) -> str:
    name = args.get("name")
    if not name:
        return tool_error("name is required")
    params: dict[str, Any] = {"name": name}
    for k in ("r", "g", "b", "speed_ms"):
        if k in args:
            params[k] = args[k]
    try:
        await _post_bridge("/rpc", {"method": "set_led_effect", "params": params})
    except Exception as e:
        return tool_error(str(e))
    return tool_result({"ok": True, "effect": name})


# ---------------------------------------------------------------------------
# hotdog_move — precise head pose
# ---------------------------------------------------------------------------
HOTDOG_MOVE_SCHEMA = {
    "name": "hotdog_move",
    "description": (
        "Move the StackChan head to a specific position. Two modes:\n"
        "  • absolute: pass `x` (yaw, -1280..1280) and/or `y` (pitch, 0..850, "
        "~425 = neutral)\n"
        "  • normalized: pass `nx`/`ny` in [-1, 1] — face-tracking convention\n"
        "Prefer `hotdog_action` for canned gestures (nod/shake/...). Use this "
        "tool when you need a specific pose."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "x":     {"type": "integer", "description": "Yaw in StackChan units, -1280..1280 (negative = left)"},
            "y":     {"type": "integer", "description": "Pitch in StackChan units, 0..850 (small = up, ~425 neutral)"},
            "nx":    {"type": "number",  "description": "Normalized yaw [-1, 1] — alternative to x"},
            "ny":    {"type": "number",  "description": "Normalized pitch [-1, 1] — alternative to y"},
            "speed": {"type": "integer", "minimum": 0, "maximum": 1000, "description": "0..1000 (200 = natural)"},
        },
    },
}


async def _handle_hotdog_move(args: dict, **kw) -> str:
    # If the caller passed normalized coordinates, route through
    # head_normalized; otherwise use the raw `move` RPC.
    has_norm = "nx" in args or "ny" in args
    has_abs  = "x"  in args or "y"  in args
    if has_norm and has_abs:
        return tool_error("provide either x/y OR nx/ny, not both")
    if not has_norm and not has_abs:
        return tool_error("provide at least one of x/y or nx/ny")
    if has_norm:
        method = "head_normalized"
        params = {}
        if "nx" in args:    params["nx"]    = args["nx"]
        if "ny" in args:    params["ny"]    = args["ny"]
        if "speed" in args: params["speed"] = args["speed"]
    else:
        method = "move"
        params = {}
        for k in ("x", "y", "speed"):
            if k in args:
                params[k] = args[k]
    try:
        await _post_bridge("/rpc", {"method": method, "params": params})
    except Exception as e:
        return tool_error(str(e))
    return tool_result({"ok": True, "method": method, "params": params})


# ---------------------------------------------------------------------------
# hotdog_capture — snap a JPEG through the onboard camera
# ---------------------------------------------------------------------------
HOTDOG_CAPTURE_SCHEMA = {
    "name": "hotdog_capture",
    "description": (
        "Snap one photo through the StackChan's onboard camera. Returns "
        "JSON with base64-encoded JPEG plus width/height/bytes metadata. "
        "Use to answer questions about what's in front of the device, or "
        "to verify physical state (e.g. \"is the cat on the desk?\"). "
        "Camera capture takes ~400 ms."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "quality": {"type": "integer", "minimum": 10, "maximum": 95,
                        "description": "JPEG quality 10..95, default 85"},
        },
    },
}


async def _handle_hotdog_capture(args: dict, **kw) -> str:
    body = {}
    if "quality" in args:
        body["quality"] = args["quality"]
    try:
        data = await _post_bridge(
            "/capture", body, params={"format": "json"}, timeout=30.0
        )
    except Exception as e:
        return tool_error(str(e))
    # Bridge returns the base64 verbatim — let the agent decide whether to
    # surface it (description, OCR, …) or just acknowledge.
    return tool_result(data)


# ---------------------------------------------------------------------------
# hotdog_show — display image or rendered text card on the LCD
# ---------------------------------------------------------------------------
HOTDOG_SHOW_SCHEMA = {
    "name": "hotdog_show",
    "description": (
        "Display content on the StackChan's 320×240 LCD. Three modes:\n"
        "  • image_url: bridge downloads + letterboxes + ships the image\n"
        "  • image_path: bridge reads a local file (same host)\n"
        "  • text: bridge renders a card with optional title + inset face\n"
        "Exactly one of image_url / image_path / text is required."
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "image_url":  {"type": "string", "description": "HTTP(S) URL to fetch"},
            "image_path": {"type": "string", "description": "Local file path on the bridge host"},
            "text":       {"type": "string", "description": "Text body for the card"},
            "title":      {"type": "string", "description": "Optional card title (with text)"},
            "face":       {"type": "string", "description": "Optional inset face name (happy/sad/...)"},
        },
    },
}


async def _handle_hotdog_show(args: dict, **kw) -> str:
    if args.get("image_url") or args.get("image_path"):
        params: dict[str, Any] = {}
        if args.get("image_url"):  params["url"]  = args["image_url"]
        if args.get("image_path"): params["path"] = args["image_path"]
        try:
            await _post_bridge("/show_image", None, params=params, timeout=30.0)
        except Exception as e:
            return tool_error(str(e))
        return tool_result({"ok": True, "rendered": "image"})
    if args.get("text"):
        body = {"text": args["text"]}
        for k in ("title", "face"):
            if args.get(k):
                body[k] = args[k]
        try:
            await _post_bridge("/show_text", body)
        except Exception as e:
            return tool_error(str(e))
        return tool_result({"ok": True, "rendered": "text"})
    return tool_error("provide one of image_url, image_path, or text")


# ---------------------------------------------------------------------------
# hotdog_status — live device telemetry
# ---------------------------------------------------------------------------
HOTDOG_STATUS_SCHEMA = {
    "name": "hotdog_status",
    "description": (
        "Read live device state: battery voltage + percent, free internal "
        "heap, free PSRAM, current speaker volume, LCD brightness, firmware "
        "version, charging state. Use to answer 'how are you?' or to "
        "diagnose before a motion-heavy task."
    ),
    "parameters": {"type": "object", "properties": {}},
}


async def _handle_hotdog_status(args: dict, **kw) -> str:
    try:
        data = await _post_bridge("/rpc", {"method": "status"})
    except Exception as e:
        return tool_error(str(e))
    return tool_result(data)


# ---------------------------------------------------------------------------
# Registration entry point — called from adapter.py:register()
# ---------------------------------------------------------------------------
HOTDOG_TOOLS = (
    # (name,             schema,                handler,                emoji)
    ("hotdog_face",      HOTDOG_FACE_SCHEMA,    _handle_hotdog_face,    "\U0001f603"),
    ("hotdog_action",    HOTDOG_ACTION_SCHEMA,  _handle_hotdog_action,  "\U0001f939"),
    ("hotdog_led",       HOTDOG_LED_SCHEMA,     _handle_hotdog_led,     "\U0001f4a1"),
    ("hotdog_move",      HOTDOG_MOVE_SCHEMA,    _handle_hotdog_move,    "\U0001f3af"),
    ("hotdog_capture",   HOTDOG_CAPTURE_SCHEMA, _handle_hotdog_capture, "\U0001f4f8"),
    ("hotdog_show",      HOTDOG_SHOW_SCHEMA,    _handle_hotdog_show,    "\U0001f5bc"),
    ("hotdog_status",    HOTDOG_STATUS_SCHEMA,  _handle_hotdog_status,  "\U0001f4df"),
)


def register_all(ctx, bridge_url: str = "http://127.0.0.1:8766") -> None:
    """Register every hotdog tool with the supplied PluginContext.

    Called once at plugin load from ``adapter.py:register(ctx)`` after
    ``ctx.register_platform(...)``. The agent on the ``hotdog`` platform
    will then see these tools in its catalog (subject to
    ``platform_toolsets.hotdog`` in config.yaml routing them in)."""
    set_bridge_url(bridge_url)
    for name, schema, handler, emoji in HOTDOG_TOOLS:
        ctx.register_tool(
            name=name,
            toolset="hotdog",
            schema=schema,
            handler=handler,
            is_async=True,
            description=schema.get("description", "")[:160],
            emoji=emoji,
        )
    logger.info("Hotdog: registered %d platform tools (bridge=%s)",
                len(HOTDOG_TOOLS), BRIDGE_URL)
