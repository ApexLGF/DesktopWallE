"""Single source of truth for bridge runtime config.

All bridge code reads its settings (Doubao credentials, server ports, agent
URLs, ...) through this module — there are no environment-variable fallbacks
and no scattered defaults. The contract is:

    1. `bridge/config.toml` next to this file (preferred)
    2. `~/.stackproxy/config.toml`              (legacy install layout)

If neither exists, `load()` writes a stub of (1) from
`bridge/config.toml.example` and raises a clear error telling the user what
to fill in. That way a freshly cloned repo prints a one-line fix instead of
crashing deep inside an async task.

Add new settings by extending `Config` and the `.example` file. Keep the
schema flat — TOML, not JSON-style nesting beyond the existing sections.
"""
from __future__ import annotations

import logging
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib                                  # Python 3.11+
except ModuleNotFoundError:                         # pragma: no cover
    import tomli as tomllib                          # type: ignore

log = logging.getLogger("bridge.config")

_HERE = Path(__file__).resolve().parent
_PRIMARY = _HERE / "config.toml"
_LEGACY  = Path.home() / ".stackproxy" / "config.toml"
_EXAMPLE = _HERE / "config.toml.example"


@dataclass(frozen=True)
class DoubaoCreds:
    appid:        str
    access_token: str
    secret_key:   str = ""        # optional; only needed for signed v1 APIs

    @property
    def app_key(self) -> str:
        """Alias used by the Flash ASR endpoint (the legacy console uses
        `app_key` and `app_id` interchangeably — they're the same value)."""
        return self.appid


@dataclass(frozen=True)
class WsSettings:
    host: str = "0.0.0.0"
    port: int = 8765


@dataclass(frozen=True)
class HttpSettings:
    host: str = "0.0.0.0"
    port: int = 8766


@dataclass(frozen=True)
class McpSettings:
    host:       str = "0.0.0.0"
    port:       int = 8767
    bridge_url: str = "http://127.0.0.1:8766"


@dataclass(frozen=True)
class AgentSettings:
    hermes_url:   str = ""
    openclaw_url: str = ""


@dataclass(frozen=True)
class TtsSettings:
    """Default TTS knobs. Each /say call can still override these per-request."""
    # Empty string → fall back to doubao_tts_unidirectional.DEFAULT_SPEAKER.
    speaker:       str = ""
    sample_rate:   int = 16000
    audio_format:  str = "pcm"
    speech_rate:   int = 0     # [-50, 100]  -50=½×  0=normal  100=2×
    loudness_rate: int = 0     # [-50, 100]


@dataclass(frozen=True)
class Config:
    doubao: DoubaoCreds
    ws:     WsSettings    = field(default_factory=WsSettings)
    http:   HttpSettings  = field(default_factory=HttpSettings)
    mcp:    McpSettings   = field(default_factory=McpSettings)
    agent:  AgentSettings = field(default_factory=AgentSettings)
    tts:    TtsSettings   = field(default_factory=TtsSettings)
    source: Path | None   = None     # where the toml came from, for logging


_CACHED: Config | None = None


def _read_toml(path: Path) -> dict:
    with path.open("rb") as f:
        return tomllib.load(f)


def _build(data: dict, source: Path) -> Config:
    d = data.get("doubao") or {}
    appid = (d.get("appid") or d.get("app_id") or d.get("app_key") or "").strip()
    token = (d.get("access_token") or d.get("access_key") or "").strip()
    if not appid or not token:
        raise RuntimeError(
            f"Doubao credentials missing in {source}\n"
            f"Set [doubao] appid and access_token. Get them from\n"
            f"  https://console.volcengine.com/speech/  →  应用管理")
    ws   = data.get("ws") or {}
    http = data.get("http") or {}
    mcp  = data.get("mcp") or {}
    ag   = data.get("agent") or {}
    tts  = data.get("tts") or {}
    return Config(
        doubao = DoubaoCreds(
            appid        = appid,
            access_token = token,
            secret_key   = (d.get("secret_key") or "").strip(),
        ),
        ws   = WsSettings(host=ws.get("host", "0.0.0.0"),
                          port=int(ws.get("port", 8765))),
        http = HttpSettings(host=http.get("host", "0.0.0.0"),
                            port=int(http.get("port", 8766))),
        mcp  = McpSettings(
            host       = mcp.get("host", "0.0.0.0"),
            port       = int(mcp.get("port", 8767)),
            bridge_url = mcp.get("bridge_url", "http://127.0.0.1:8766"),
        ),
        agent = AgentSettings(
            hermes_url   = (ag.get("hermes_url") or "").strip(),
            openclaw_url = (ag.get("openclaw_url") or "").strip(),
        ),
        tts = TtsSettings(
            speaker       = (tts.get("speaker") or "").strip(),
            sample_rate   = int(tts.get("sample_rate",   16000)),
            audio_format  = (tts.get("audio_format") or "pcm").strip(),
            speech_rate   = int(tts.get("speech_rate",   0)),
            loudness_rate = int(tts.get("loudness_rate", 0)),
        ),
        source = source,
    )


def load(*, reload: bool = False) -> Config:
    """Return the singleton Config. Cache survives subsequent calls.

    Pass `reload=True` to re-read the TOML (mainly for tests / `kill -HUP`).
    """
    global _CACHED
    if _CACHED is not None and not reload:
        return _CACHED
    for candidate in (_PRIMARY, _LEGACY):
        if candidate.exists():
            try:
                cfg = _build(_read_toml(candidate), candidate)
                log.info("config loaded from %s", candidate)
                _CACHED = cfg
                return cfg
            except RuntimeError:
                raise
            except Exception as e:
                raise RuntimeError(f"failed to parse {candidate}: {e}") from e
    # Nothing found — give a precise, actionable error.
    raise RuntimeError(
        f"no bridge config found.\n"
        f"  cp {_EXAMPLE} {_PRIMARY}\n"
        f"  edit {_PRIMARY}  (fill in your Doubao appid + access_token)\n"
        f"\n"
        f"Both {_PRIMARY} and {_LEGACY} are gitignored, so your keys will\n"
        f"never end up in the repo.")


def get_doubao() -> DoubaoCreds:
    return load().doubao


def get_ws() -> WsSettings:
    return load().ws


def get_http() -> HttpSettings:
    return load().http


def get_mcp() -> McpSettings:
    return load().mcp


def get_agent() -> AgentSettings:
    return load().agent


def get_tts() -> TtsSettings:
    return load().tts


if __name__ == "__main__":
    # `python3 config.py` prints a sanity report (creds masked).
    try:
        c = load()
    except RuntimeError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
    print(f"source: {c.source}")
    appid = c.doubao.appid
    print(f"doubao.appid:        {appid[:4]}...{appid[-4:]} (len={len(appid)})")
    tok = c.doubao.access_token
    print(f"doubao.access_token: {tok[:4]}...{tok[-4:]} (len={len(tok)})")
    print(f"ws:    {c.ws.host}:{c.ws.port}")
    print(f"http:  {c.http.host}:{c.http.port}")
    print(f"mcp:   {c.mcp.host}:{c.mcp.port}  bridge={c.mcp.bridge_url}")
    print(f"agent: hermes={c.agent.hermes_url or '-'}  openclaw={c.agent.openclaw_url or '-'}")
