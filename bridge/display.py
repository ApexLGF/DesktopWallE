"""
Display rendering for 热狗's 320x240 LCD.

The face renderer is a faithful port of the old M5Stack Arduino firmware's
face system (`src/main.cpp.sync.bak`):

  - eyes are a 3-layer ring (outer color + bg inner + pupil) plus a small
    white highlight dot — gives them cartoon depth instead of looking
    like flat ovals
  - brows tilt for emotion (angry = inner down, sad = inner up)
  - mouth shapes are line / dot-trail arc smile / dot-trail arc frown /
    donut open mouth / small dot / cat ">w<"
  - decorations: heart, Z's, sweat drops

Both `render_face` and `render_text` return a 320x240 RGB PIL.Image which
the caller JPEG-encodes and ships via the existing ocsc.v2 `KIND_DISP_IMG`
binary frame — no firmware changes.
"""
from __future__ import annotations

import io
import math
import os

from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240

# All vector drawing happens at 2× the LCD resolution and gets downsampled
# with LANCZOS. PIL's primitive shapes (ellipse/line/arc) don't anti-alias on
# their own — drawing them at 2× then resizing produces smooth edges, much
# closer to what the old M5GFX firmware looked like.
SS = 2
SS_W, SS_H = W * SS, H * SS

# palette
BG_DEFAULT    = (0, 0, 0)         # pitch black — like the old firmware
FG_DEFAULT    = (240, 240, 240)
EYE_COLOR     = (255, 255, 255)   # white eyes by default
MOUTH_COLOR   = (255, 255, 255)
ACCENT        = (255, 180, 100)
PINK          = (255, 85, 119)
SWEAT_BLUE    = (102, 187, 238)
HIGHLIGHT     = (255, 255, 255)

# font search — first hit wins
_FONT_PATHS = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Songti.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
]
_FONT_PATH = next((p for p in _FONT_PATHS if os.path.exists(p)), None)


def _font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    if _FONT_PATH:
        return ImageFont.truetype(_FONT_PATH, size)
    return ImageFont.load_default()


# =========================================================================
# face — port of the old `FaceParams` system from src/main.cpp.sync.bak
# =========================================================================

# accepted expression names — anything unknown falls back to "neutral"
FACE_EXPRS: list[str] = [
    "idle", "neutral",
    "happy", "smile",
    "love",
    "sad",
    "angry",
    "surprised",
    "thinking",
    "sleep", "sleepy", "blink",
    "wink_l", "wink_r",
    "stare",
    "dead",
    "embarrassed",
    "cat",
    "speak", "talking",
    "listening", "hear",
    "alert",
]


def _fill_circle(d: ImageDraw.ImageDraw, cx: int, cy: int, r: int, color) -> None:
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


def _draw_eye(d: ImageDraw.ImageDraw, cx: int, cy: int,
              eye_r: int, pupil_dx: int, pupil_dy: int,
              closed: bool, eye_c, bg_c, s: int = SS) -> None:
    """Ported from drawEye() in src/main.cpp.sync.bak:862-876."""
    if closed:
        d.line((cx - eye_r, cy, cx + eye_r, cy), fill=eye_c, width=2 * s)
        d.line((cx - eye_r, cy + 2 * s, cx + eye_r, cy + 2 * s),
               fill=eye_c, width=2 * s)
        return
    _fill_circle(d, cx, cy, eye_r, eye_c)
    _fill_circle(d, cx, cy, eye_r - 4 * s, bg_c)
    _fill_circle(d, cx + pupil_dx, cy + pupil_dy, eye_r - 12 * s, eye_c)
    _fill_circle(d, cx + pupil_dx - 6 * s, cy + pupil_dy - 6 * s, 3 * s, HIGHLIGHT)


def _draw_mouth(d: ImageDraw.ImageDraw, cx: int, mouth_y: int,
                mouth_w: int, mouth_h: int, shape: int,
                tongue: bool, mouth_c, bg_c, s: int = SS) -> None:
    """Ported from drawMouth() in src/main.cpp.sync.bak:878-924."""
    if shape == 0:                                          # neutral line
        d.rounded_rectangle((cx - mouth_w // 2, mouth_y - 2 * s,
                             cx + mouth_w // 2, mouth_y + 3 * s),
                            radius=2 * s, fill=mouth_c)
    elif shape == 1:                                        # smile (dot arc)
        for t in range(13):
            a = t / 12.0
            x = cx - mouth_w // 2 + int(a * mouth_w)
            y = mouth_y + int(math.sin(a * math.pi) * mouth_h)
            _fill_circle(d, x, y, 3 * s, mouth_c)
    elif shape == 2:                                        # frown
        for t in range(13):
            a = t / 12.0
            x = cx - mouth_w // 2 + int(a * mouth_w)
            y = mouth_y - int(math.sin(a * math.pi) * mouth_h)
            _fill_circle(d, x, y, 3 * s, mouth_c)
    elif shape == 3:                                        # open (donut)
        _fill_circle(d, cx, mouth_y, mouth_h, mouth_c)
        _fill_circle(d, cx, mouth_y, max(1, mouth_h - 4 * s), bg_c)
    elif shape == 4:                                        # small dot
        _fill_circle(d, cx, mouth_y, 5 * s, mouth_c)
    elif shape == 5:                                        # cat >w<
        for off in (-1, 0, 1):
            d.line((cx - 16 * s, mouth_y - 6 * s + off * s,
                    cx,           mouth_y + 6 * s + off * s),
                   fill=mouth_c, width=3 * s)
            d.line((cx,           mouth_y + 6 * s + off * s,
                    cx + 16 * s, mouth_y - 6 * s + off * s),
                   fill=mouth_c, width=3 * s)
    if tongue:
        d.rounded_rectangle((cx - 10 * s, mouth_y + 8 * s,
                             cx + 10 * s, mouth_y + 22 * s),
                            radius=5 * s, fill=PINK)


def _draw_brows(d: ImageDraw.ImageDraw, cx: int, eye_y: int, eye_r: int,
                eye_dx: int, brow_angle: int, color, s: int = SS) -> None:
    """Ported from drawBrows() in src/main.cpp.sync.bak:926-945."""
    left_x = cx - eye_dx
    right_x = cx + eye_dx
    brow_y = eye_y - eye_r - 10 * s
    if brow_angle < 0:                                      # angry
        d.polygon([(left_x - 16 * s, brow_y), (left_x + 16 * s, brow_y + 12 * s),
                   (left_x + 16 * s, brow_y - 2 * s)], fill=color)
        d.polygon([(right_x + 16 * s, brow_y), (right_x - 16 * s, brow_y + 12 * s),
                   (right_x - 16 * s, brow_y - 2 * s)], fill=color)
    elif brow_angle > 0:                                    # sad
        d.polygon([(left_x - 16 * s, brow_y + 12 * s), (left_x + 16 * s, brow_y),
                   (left_x - 16 * s, brow_y + 14 * s)], fill=color)
        d.polygon([(right_x + 16 * s, brow_y + 12 * s), (right_x - 16 * s, brow_y),
                   (right_x + 16 * s, brow_y + 14 * s)], fill=color)
    else:                                                    # flat bars
        d.rounded_rectangle((left_x - 16 * s, brow_y,
                             left_x + 16 * s, brow_y + 4 * s),
                            radius=2 * s, fill=color)
        d.rounded_rectangle((right_x - 16 * s, brow_y,
                             right_x + 16 * s, brow_y + 4 * s),
                            radius=2 * s, fill=color)


def _draw_decorations(d: ImageDraw.ImageDraw, cx: int, eye_y: int,
                       show_z: bool, show_heart: bool,
                       show_sweat: bool, s: int = SS) -> None:
    if show_z:
        d.text((SS_W - 60 * s, 24 * s), "z", font=_font(20 * s), fill=(204, 204, 255))
        d.text((SS_W - 40 * s, 44 * s), "Z", font=_font(26 * s), fill=(204, 204, 255))
    if show_heart:
        hx, hy = 50 * s, 50 * s
        _fill_circle(d, hx - 6 * s, hy, 8 * s, PINK)
        _fill_circle(d, hx + 6 * s, hy, 8 * s, PINK)
        d.polygon([(hx - 13 * s, hy + 3 * s), (hx + 13 * s, hy + 3 * s),
                   (hx, hy + 18 * s)], fill=PINK)
    if show_sweat:
        sx, sy = cx + 70 * s, eye_y - 20 * s
        _fill_circle(d, sx, sy, 5 * s, SWEAT_BLUE)
        d.polygon([(sx - 5 * s, sy), (sx + 5 * s, sy),
                   (sx, sy - 12 * s)], fill=SWEAT_BLUE)


# expression → FaceParams (ported from presetExpression())
def _params_for(expr: str) -> dict:
    p = dict(eye_r=28, eye_dx=60, eye_y=110,
             mouth_y=185, mouth_w=70, mouth_h=18,
             pupil_dx=0, pupil_dy=0, brow_angle=0,
             mouth_shape=0,
             left_closed=False, right_closed=False,
             tongue=False, show_z=False, show_heart=False, show_sweat=False)
    expr = (expr or "neutral").lower()
    if expr in ("idle", "neutral"):
        p["mouth_shape"] = 1   # idle = warm slight smile, friendlier than line
    elif expr in ("happy",):
        p["mouth_shape"] = 1
    elif expr == "smile":
        p["mouth_shape"] = 1; p["eye_r"] = 24
    elif expr == "love":
        p["mouth_shape"] = 1; p["show_heart"] = True; p["eye_r"] = 22
    elif expr == "sad":
        p["mouth_shape"] = 2; p["brow_angle"] = 1
    elif expr == "angry":
        p["mouth_shape"] = 2; p["brow_angle"] = -1; p["eye_r"] = 24
    elif expr in ("surprised", "alert"):
        p["mouth_shape"] = 3; p["eye_r"] = 32
    elif expr == "thinking":
        p["mouth_shape"] = 4; p["pupil_dx"] = 12; p["pupil_dy"] = -8
        p["brow_angle"] = -1
    elif expr in ("sleep", "sleepy"):
        p["left_closed"] = True; p["right_closed"] = True
        p["mouth_shape"] = 4; p["show_z"] = True
    elif expr == "blink":
        p["left_closed"] = True; p["right_closed"] = True
        p["mouth_shape"] = 1
    elif expr == "wink_l":
        p["left_closed"] = True; p["mouth_shape"] = 1
    elif expr == "wink_r":
        p["right_closed"] = True; p["mouth_shape"] = 1
    elif expr == "stare":
        p["mouth_shape"] = 0; p["eye_r"] = 30
    elif expr == "dead":
        p["mouth_shape"] = 0; p["pupil_dx"] = -8
    elif expr == "embarrassed":
        p["mouth_shape"] = 1; p["show_sweat"] = True; p["brow_angle"] = 1
    elif expr == "cat":
        p["mouth_shape"] = 5; p["eye_r"] = 22
    elif expr in ("speak", "talking"):
        p["mouth_shape"] = 3; p["mouth_h"] = 10
    elif expr in ("listening", "hear"):
        # half-closed eyes + slight smile (no brow change)
        p["mouth_shape"] = 1; p["mouth_h"] = 10
    return p


def _scale_params(p: dict) -> dict:
    """Scale all pixel-valued FaceParams up by SS for supersampled drawing."""
    scaled_keys = ("eye_r", "eye_dx", "eye_y",
                   "mouth_y", "mouth_w", "mouth_h",
                   "pupil_dx", "pupil_dy")
    return {**p, **{k: p[k] * SS for k in scaled_keys}}


def render_face(expr: str = "idle",
                bg: tuple[int, int, int] = BG_DEFAULT,
                eye: tuple[int, int, int] = EYE_COLOR,
                mouth: tuple[int, int, int] = MOUTH_COLOR) -> Image.Image:
    """Return a 320x240 robot face image. Aesthetic ported from the
    Arduino StackChan firmware (see `src/main.cpp.sync.bak`:973+). The
    actual drawing happens at SS× resolution; the LANCZOS downsample
    smooths every edge."""
    big = Image.new("RGB", (SS_W, SS_H), bg)
    d = ImageDraw.Draw(big)
    p = _scale_params(_params_for(expr))
    cx = SS_W // 2

    _draw_brows(d, cx, p["eye_y"], p["eye_r"], p["eye_dx"], p["brow_angle"], eye)
    _draw_eye(d, cx - p["eye_dx"], p["eye_y"], p["eye_r"],
              p["pupil_dx"], p["pupil_dy"], p["left_closed"], eye, bg)
    _draw_eye(d, cx + p["eye_dx"], p["eye_y"], p["eye_r"],
              p["pupil_dx"], p["pupil_dy"], p["right_closed"], eye, bg)
    _draw_mouth(d, cx, p["mouth_y"], p["mouth_w"], p["mouth_h"],
                p["mouth_shape"], p["tongue"], mouth, bg)
    _draw_decorations(d, cx, p["eye_y"],
                      p["show_z"], p["show_heart"], p["show_sweat"])
    return big.resize((W, H), Image.LANCZOS)


# =========================================================================
# text
# =========================================================================

def _wrap_cjk(text: str, font, max_w: int) -> list[str]:
    lines: list[str] = []
    for paragraph in text.split("\n"):
        cur = ""
        for ch in paragraph:
            candidate = cur + ch
            if font.getlength(candidate) > max_w and cur:
                lines.append(cur)
                cur = ch
            else:
                cur = candidate
        if cur:
            lines.append(cur)
    return lines


def render_text(text: str,
                title: str = "",
                face: str | None = None,
                bg: tuple[int, int, int] = BG_DEFAULT,
                fg: tuple[int, int, int] = FG_DEFAULT,
                size: str = "auto") -> Image.Image:
    """Render `text` at SS× resolution and downsample with LANCZOS.

    Text gets a free anti-aliased pass from PIL's TrueType engine; the
    supersample gives the eventual downsample more pixels to work with
    so strokes and CJK glyph edges look crisp instead of jagged."""
    s = SS
    big = Image.new("RGB", (SS_W, SS_H), bg)
    d = ImageDraw.Draw(big)

    face_inset = 0
    if face:
        try:
            f_img = render_face(face, bg=bg).resize((96 * s, 72 * s), Image.LANCZOS)
            big.paste(f_img, (8 * s, 8 * s))
            face_inset = 110 * s
        except Exception:
            pass

    body_top = 16 * s
    if title:
        f_title = _font(16 * s)
        tw = f_title.getlength(title)
        d.text(((SS_W - tw) // 2, 8 * s), title, font=f_title, fill=(140, 200, 255))
        body_top = 34 * s

    if size == "auto":
        char_n = len(text)
        body_size = 30 if char_n <= 20 else 24 if char_n <= 60 else 18
    else:
        body_size = {"small": 18, "medium": 24, "large": 30}.get(size, 24)

    f_body = _font(body_size * s)
    max_w = SS_W - 30 * s - face_inset
    lines = _wrap_cjk(text, f_body, max_w)
    line_h = int(body_size * 1.45 * s)
    total_h = len(lines) * line_h
    avail_h = SS_H - body_top - 16 * s
    y_start = body_top + max(0, (avail_h - total_h) // 2)
    x_offset = face_inset

    for i, line in enumerate(lines):
        lw = f_body.getlength(line)
        x = x_offset + (SS_W - x_offset - lw) // 2
        d.text((x, y_start + i * line_h), line, font=f_body, fill=fg)

    return big.resize((W, H), Image.LANCZOS)


# =========================================================================
# JPEG helpers + cache
# =========================================================================

def to_jpeg(img: Image.Image, quality: int = 92) -> bytes:
    """JPEG-encode with `subsampling=0` (4:4:4, no chroma downsampling).

    Default JPEG uses 4:2:0 chroma subsampling which **blurs sharp colored
    edges** — exactly what you see as "毛边" on the face strokes and text
    glyphs. With 4:4:4 + quality 92 the file is ~2× bigger (still under
    ~15 KB for our 320×240 frames) but the edges stay crisp."""
    buf = io.BytesIO()
    img.convert("RGB").save(buf, format="JPEG", quality=quality,
                             subsampling=0, optimize=True)
    return buf.getvalue()


def to_rgb565(img: Image.Image) -> bytes:
    """Pack a 320×240 RGB PIL image as little-endian RGB565 (153,600 bytes).

    Used for the `KIND_DISP_RGB565` path: the device blits this straight to
    the LCD framebuffer with no JPEG decode → no chroma subsampling artifacts,
    no blurring on text glyphs. The cost is ~25× larger payload (~150 KB vs
    ~6 KB JPEG) but WS over LAN at <60 ms is still fine for static screens."""
    if img.size != (W, H):
        img = img.resize((W, H), Image.LANCZOS)
    rgb = img.convert("RGB")
    pixels = rgb.tobytes()                       # tightly packed R,G,B,R,G,B …
    # Vectorised pack into rgb565: r5 g6 b5 in little-endian uint16.
    import numpy as np
    arr = np.frombuffer(pixels, dtype=np.uint8).reshape(-1, 3)
    r5 = (arr[:, 0].astype(np.uint16) >> 3) << 11
    g6 = (arr[:, 1].astype(np.uint16) >> 2) << 5
    b5 = (arr[:, 2].astype(np.uint16) >> 3)
    out = (r5 | g6 | b5).astype("<u2").tobytes()
    return out


_screen_cache: dict[str, bytes] = {}


def listening_screen_jpeg() -> bytes:
    """Pre-rendered cached 'I'm listening, talk now' screen with crisp text.

    Rendered once at module load (effectively); subsequent calls just hand
    out the cached bytes so wake-to-pixel latency is just the WS hop.
    JPEG quality 95 + 4:4:4 subsampling keeps the '请讲' glyph strokes crisp.
    """
    if "listening_text" not in _screen_cache:
        img = render_text("请讲…", title="正在聆听", face="listening")
        _screen_cache["listening_text"] = to_jpeg(img, quality=95)
    return _screen_cache["listening_text"]


_face_cache: dict[str, bytes] = {}


def face_jpeg(expr: str = "idle") -> bytes:
    expr = (expr or "idle").lower()
    if expr not in _face_cache:
        _face_cache[expr] = to_jpeg(render_face(expr))
    return _face_cache[expr]


def clear_face_cache() -> None:
    """Bust the per-expression face cache. Useful if the renderer changes."""
    _face_cache.clear()
