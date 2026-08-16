#!/usr/bin/env python3
"""Build indexed dialogue-emotion v2 assets from concatenated JPEG MJPEG files."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


EMOTIONS = ("standby", "neutral", "happy", "sad", "angry", "loving")
MOUTH_LEVELS = ("closed", "small", "medium", "large")
EYE_LEVELS = ("open", "half", "closed")
LIFE_TRACK_ROWS = 48
LIFE_SEQUENCE_OFFSETS = (0, 1, 2, 3, 4, 3, 2, 1, 0)


@dataclass(frozen=True)
class Frame:
    offset: int
    data: bytes


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def scan_mjpeg(data: bytes) -> list[Frame]:
    frames: list[Frame] = []
    cursor = 0
    while True:
        soi = data.find(b"\xff\xd8", cursor)
        if soi < 0:
            break
        eoi = data.find(b"\xff\xd9", soi + 2)
        if eoi < 0:
            raise ValueError(f"truncated JPEG at byte {soi}")
        frames.append(Frame(soi, data[soi:eoi + 2]))
        cursor = eoi + 2
    if not frames:
        raise ValueError("no JPEG frames found")
    return frames


def rgb(frame: Frame) -> np.ndarray:
    with Image.open(io.BytesIO(frame.data)) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def gray_small(frame: Frame, size: tuple[int, int]) -> np.ndarray:
    with Image.open(io.BytesIO(frame.data)) as im:
        return np.asarray(im.convert("L").resize(size, Image.Resampling.BILINEAR), dtype=np.float32)


def load_profile(path: Path | None) -> dict:
    if path is None:
        return {}
    with path.open("r", encoding="utf-8") as f:
        value = json.load(f)
    if value.get("schema") not in (None, "dialogue-emotion-profile-v1"):
        raise ValueError("unsupported profile schema")
    return value


def segments(count: int, override: dict | None) -> dict[str, dict[str, int]]:
    if override:
        result = {}
        for name in ("enter", "hold", "exit"):
            part = override[name]
            start, end = int(part["start"]), int(part["end"])
            if not (0 <= start <= end < count):
                raise ValueError(f"invalid {name} range {start}..{end} for {count} frames")
            result[name] = {"start": start, "end": end}
        return result
    a = max(1, count // 3)
    b = max(a + 1, (count * 2) // 3)
    b = min(b, count - 1)
    return {
        "enter": {"start": 0, "end": a - 1},
        "hold": {"start": a, "end": b - 1},
        "exit": {"start": b, "end": count - 1},
    }


def clamp_rect(rect: dict, width: int, height: int) -> dict[str, int]:
    x = max(0, min(width - 1, int(rect["x"])))
    y = max(0, min(height - 1, int(rect["y"])))
    w = max(8, min(width - x, int(rect.get("width", rect.get("w", 0)))))
    h = max(8, min(height - y, int(rect.get("height", rect.get("h", 0)))))
    return {"x": x, "y": y, "width": w, "height": h}


def suggest_mouth(frames: list[Frame], hold: dict[str, int], width: int, height: int) -> tuple[dict, dict]:
    sw, sh = 120, 120
    ids = np.linspace(hold["start"], hold["end"], min(20, hold["end"] - hold["start"] + 1), dtype=int)
    samples = np.stack([gray_small(frames[int(i)], (sw, sh)) for i in ids])
    motion = np.mean(np.abs(np.diff(samples, axis=0)), axis=0) if len(samples) > 1 else np.zeros((sh, sw))
    # Constrain the search to the central/lower face, while still allowing profile overrides.
    x0, x1 = int(sw * .28), int(sw * .72)
    y0, y1 = int(sh * .40), int(sh * .62)
    ww, wh = max(12, int(sw * .24)), max(8, int(sh * .12))
    best = (-1.0, x0, y0)
    for y in range(y0, max(y0 + 1, y1 - wh + 1), 3):
        for x in range(x0, max(x0 + 1, x1 - ww + 1), 3):
            inner = float(motion[y:y + wh, x:x + ww].mean())
            pad = 4
            outer = motion[max(0, y-pad):min(sh, y+wh+pad), max(0, x-pad):min(sw, x+ww+pad)]
            cx = (x + ww / 2) / sw
            cy = (y + wh / 2) / sh
            center_penalty = 10.0 * abs(cx - .50) + 8.0 * abs(cy - .48)
            score = inner - .25 * float(outer.mean()) - center_penalty
            if score > best[0]:
                best = (score, x, y)
    _, x, y = best
    rect = {
        "x": int(round(x * width / sw)), "y": int(round(y * height / sh)),
        "width": int(round(ww * width / sw)), "height": int(round(wh * height / sh)),
    }
    confidence = min(1.0, max(0.0, best[0] / 12.0))
    return clamp_rect(rect, width, height), {"motion_score": round(best[0], 3), "confidence": round(confidence, 3)}


def crop_array(a: np.ndarray, rect: dict) -> np.ndarray:
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    return a[y:y+h, x:x+w]


def choose_mouth_frames(frames: list[Frame], hold: dict, rect: dict) -> tuple[dict[str, int], list[float]]:
    ids = list(range(hold["start"], hold["end"] + 1))
    arrays = [crop_array(rgb(frames[i]).astype(np.float32), rect) for i in ids]
    median = np.median(np.stack(arrays), axis=0)
    activity = np.asarray([float(np.mean(np.abs(a - median))) for a in arrays])
    order = np.argsort(activity)
    picks = {}
    for name, q in zip(MOUTH_LEVELS, (0.05, 0.38, 0.68, 0.95)):
        pos = int(round(q * (len(order) - 1)))
        picks[name] = ids[int(order[pos])]
    return picks, [round(float(v), 3) for v in activity]


def choose_second_pose(frames: list[Frame], hold: dict, rect: dict, primary: int,
                       activity: list[float], override: int | None) -> tuple[int, float]:
    if override is not None:
        chosen = int(override)
        if not (hold["start"] <= chosen <= hold["end"]):
            raise ValueError("pose_1_frame must be inside hold segment")
    else:
        ids = list(range(hold["start"], hold["end"] + 1))
        cutoff = float(np.quantile(np.asarray(activity), 0.40))
        candidates = [i for i, a in zip(ids, activity) if a <= cutoff and i != primary]
        primary_small = gray_small(frames[primary], (96, 96))
        scored = [(float(np.mean(np.abs(gray_small(frames[i], (96, 96)) - primary_small))), i)
                  for i in candidates]
        usable = [v for v in scored if 2.0 <= v[0] <= 18.0]
        chosen = max(usable or scored or [(0.0, primary)])[1]
    delta = float(np.mean(np.abs(gray_small(frames[chosen], (96, 96)) -
                                 gray_small(frames[primary], (96, 96)))))
    return chosen, round(delta, 3)


def risk_report(frames: list[Frame], picks: dict[str, int], rect: dict, width: int, height: int) -> dict:
    base = rgb(frames[picks["closed"]]).astype(np.float32)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    pad = max(6, min(w, h) // 8)
    x0, y0, x1, y1 = max(0, x-pad), max(0, y-pad), min(width, x+w+pad), min(height, y+h+pad)
    mask = np.ones((y1-y0, x1-x0), dtype=bool)
    mask[y-y0:y-y0+h, x-x0:x-x0+w] = False
    border_scores = []
    global_scores = []
    for level in MOUTH_LEVELS[1:]:
        arr = rgb(frames[picks[level]]).astype(np.float32)
        delta = np.mean(np.abs(arr - base), axis=2)
        border_scores.append(float(delta[y0:y1, x0:x1][mask].mean()) if mask.any() else 0.0)
        global_scores.append(float(delta.mean()))
    seam = max(border_scores, default=0.0)
    global_motion = max(global_scores, default=0.0)
    safe = seam <= 8.0 and global_motion <= 12.0
    return {
        "layered_safe": safe,
        "recommended_render_mode": "layered" if safe else "full_frame_clip",
        "seam_border_delta": round(seam, 3),
        "global_frame_delta": round(global_motion, 3),
        "reason": "mouth patch boundary is stable" if safe else "motion extends beyond the mouth patch; review or use full-frame fallback",
    }


def mouth_composite(base: np.ndarray, candidate: np.ndarray, rect: dict,
                    feather_px: int, align_px: int) -> tuple[np.ndarray, dict]:
    """Align a mouth candidate to the canonical hold base and fade its edge to that base."""
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    base_patch = base[y:y+h, x:x+w].astype(np.float32)
    feather = max(2, min(int(feather_px), max(2, min(w, h) // 3)))
    yy, xx = np.mgrid[0:h, 0:w]
    edge = np.minimum.reduce((xx, yy, w - 1 - xx, h - 1 - yy)).astype(np.float32)
    alpha = np.clip(edge / float(feather), 0.0, 1.0)
    alpha = alpha * alpha * (3.0 - 2.0 * alpha)
    border = alpha < 0.55

    best_score, best_patch, best_shift = float("inf"), None, (0, 0)
    for dy in range(-align_px, align_px + 1):
        for dx in range(-align_px, align_px + 1):
            sx, sy = x + dx, y + dy
            if sx < 0 or sy < 0 or sx + w > candidate.shape[1] or sy + h > candidate.shape[0]:
                continue
            patch = candidate[sy:sy+h, sx:sx+w].astype(np.float32)
            score = float(np.mean(np.abs(patch[border] - base_patch[border])))
            if score < best_score:
                best_score, best_patch, best_shift = score, patch, (dx, dy)
    if best_patch is None:
        best_patch = candidate[y:y+h, x:x+w].astype(np.float32)
    composed = best_patch * alpha[:, :, None] + base_patch * (1.0 - alpha[:, :, None])
    composed = np.clip(np.rint(composed), 0, 255).astype(np.uint8)
    edge_delta = float(np.mean(np.abs(composed[edge <= 1].astype(np.float32) - base_patch[edge <= 1])))
    return composed, {"dx": best_shift[0], "dy": best_shift[1],
                      "border_match_delta": round(best_score, 3),
                      "composite_edge_delta": round(edge_delta, 3)}


def save_patch_array(patch: np.ndarray, target: Path, quality: int) -> None:
    Image.fromarray(patch, "RGB").save(target, "JPEG", quality=quality, optimize=True)


def save_rgb565_array(patch: np.ndarray, target: Path) -> None:
    """Firmware loads these directly — no on-device JPEG decode for mouth/eye."""
    r = (patch[:, :, 0].astype(np.uint16) >> 3)
    g = (patch[:, :, 1].astype(np.uint16) >> 2)
    b = (patch[:, :, 2].astype(np.uint16) >> 3)
    pix = ((r << 11) | (g << 5) | b).astype("<u2")
    target.write_bytes(pix.tobytes())


def _aligned_span(active: np.ndarray, limit: int, alignment: int = 8) -> tuple[int, int]:
    ids = np.flatnonzero(active)
    if ids.size == 0:
        return 0, limit
    start = max(0, (int(ids[0]) // alignment) * alignment)
    end = min(limit, ((int(ids[-1]) + 1 + alignment - 1) // alignment) * alignment)
    if end - start < 64:
        center = (start + end) // 2
        start = max(0, center - 32)
        end = min(limit, start + 64)
        start = max(0, end - 64)
    return start, end


def build_life_layer(frames: list[Frame], hold: dict, base_frame: int, base: np.ndarray,
                     mouth_rect: dict, eye_rect: dict, out: Path, emotion: str) -> dict:
    """Generate same-generation, pre-cropped RGB565+alpha life tracks."""
    height, width = base.shape[:2]
    direction = 1 if base_frame + 4 <= hold["end"] else -1
    ids = [max(hold["start"], min(hold["end"], base_frame + direction * n))
           for n in LIFE_SEQUENCE_OFFSETS]
    arrays = [rgb(frames[i]) for i in ids]
    delta_stack = np.stack([
        np.mean(np.abs(a.astype(np.float32) - base.astype(np.float32)), axis=2)
        for a in arrays
    ])
    motion = np.mean(delta_stack, axis=0)
    for rect in (mouth_rect, eye_rect):
        x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
        motion[y:y+h, x:x+w] = 0.0

    track_ranges = ((max(8, height // 12), min(height, height // 2)),
                    (max(0, height // 2), min(height, height - height // 10)))
    life_root = out / "life"
    life_root.mkdir(exist_ok=True)
    previews = [base.copy() for _ in arrays]
    tracks = []
    for track_id, (search_y0, search_y1) in enumerate(track_ranges):
        if search_y1 - search_y0 < LIFE_TRACK_ROWS:
            continue
        row_score = motion.mean(axis=1)
        best_y, best_score = search_y0, -1.0
        for y in range(search_y0, search_y1 - LIFE_TRACK_ROWS + 1, 4):
            score = float(row_score[y:y + LIFE_TRACK_ROWS].mean())
            if score > best_score:
                best_y, best_score = y, score
        band_motion = motion[best_y:best_y + LIFE_TRACK_ROWS]
        threshold = max(2.5, float(np.quantile(band_motion, 0.70)))
        x0, x1 = _aligned_span(np.any(band_motion >= threshold, axis=0), width)
        roi = {"x": x0, "y": best_y, "width": x1 - x0, "height": LIFE_TRACK_ROWS}
        track_dir = life_root / f"track_{track_id}"
        track_dir.mkdir(exist_ok=True)
        entries = []
        for seq, (frame_id, candidate, delta) in enumerate(zip(ids, arrays, delta_stack)):
            patch = candidate[best_y:best_y + LIFE_TRACK_ROWS, x0:x1]
            local_delta = delta[best_y:best_y + LIFE_TRACK_ROWS, x0:x1]
            alpha = np.clip((local_delta - 2.0) * (255.0 / 14.0), 0, 220).astype(np.uint8)
            yy, xx = np.mgrid[0:LIFE_TRACK_ROWS, 0:x1-x0]
            edge = np.minimum.reduce((xx, yy, x1-x0-1-xx, LIFE_TRACK_ROWS-1-yy))
            alpha = (alpha.astype(np.float32) * np.clip(edge / 8.0, 0.0, 1.0)).astype(np.uint8)
            alpha = np.array(Image.fromarray(alpha, "L").filter(ImageFilter.GaussianBlur(2.0)),
                             dtype=np.uint8, copy=True)
            if seq in (0, len(arrays) - 1):
                alpha.fill(0)
            # Precompose every life frame against the canonical hold base.  The
            # device can then replace the ROI deterministically instead of
            # accumulating alpha over the previous canvas frame (which leaves
            # ghosts when the final zero-alpha frame is reached).
            base_crop = base[best_y:best_y + LIFE_TRACK_ROWS, x0:x1].astype(np.float32)
            a = alpha.astype(np.float32)[:, :, None] / 255.0
            composite = np.clip(
                patch.astype(np.float32) * a + base_crop * (1.0 - a), 0, 255).astype(np.uint8)
            rgb_path = track_dir / f"{seq:02d}.rgb565"
            mask_path = track_dir / f"{seq:02d}.a8"
            save_rgb565_array(composite, rgb_path)
            mask_path.write_bytes(alpha.tobytes())
            previews[seq][best_y:best_y + LIFE_TRACK_ROWS, x0:x1] = composite
            entries.append({"sequence": seq, "source_frame": frame_id,
                            "rgb565": f"life/track_{track_id}/{seq:02d}.rgb565",
                            "rgb565_sha256": sha256(rgb_path.read_bytes()),
                            "mask_a8": f"life/track_{track_id}/{seq:02d}.a8",
                            "mask_sha256": sha256(mask_path.read_bytes()),
                            "peak_alpha": int(alpha.max()),
                            "active_ratio": round(float(np.count_nonzero(alpha)) / alpha.size, 4)})
        tracks.append({"id": track_id, "roi": roi, "motion_score": round(best_score, 3),
                       "frames": entries})

    generation_seed = (sha256(base.tobytes()) + emotion + json.dumps(ids) +
                       json.dumps([t["roi"] for t in tracks], sort_keys=True)).encode("utf-8")
    generation_id = sha256(generation_seed)[:20]
    Image.fromarray(base, "RGB").save(life_root / "base_preview.png")
    preview_images = [Image.fromarray(p, "RGB") for p in previews]
    preview_images[0].save(life_root / "life_preview.gif", save_all=True,
                           append_images=preview_images[1:], duration=120, loop=0)
    return {"schema": "dialogue-life-layer-v1", "generation_id": generation_id,
            "composite_mode": "canonical_base_precomposited_v1",
            "base_rgb565_sha256": sha256((out / "hold_base.rgb565").read_bytes()),
            "enabled_default": emotion in ("standby", "neutral", "happy", "loving"),
            "sequence_ms": 120, "sequence": ids, "max_tracks_per_tick": 1,
            "schedule": "interleave_latest_drop_old", "tracks": tracks,
            "preview": "life/life_preview.gif"}


def save_composite_preview(base: np.ndarray, patches: dict[str, np.ndarray], rect: dict,
                           target: Path, label: str) -> None:
    views = []
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    for level in MOUTH_LEVELS:
        view = base.copy()
        view[y:y+h, x:x+w] = patches[level]
        views.append(Image.fromarray(view, "RGB"))
    canvas = Image.new("RGB", (base.shape[1] * 2, base.shape[0] * 2))
    draw = ImageDraw.Draw(canvas)
    for i, view in enumerate(views):
        ox, oy = (i % 2) * base.shape[1], (i // 2) * base.shape[0]
        canvas.paste(view, (ox, oy))
        draw.rectangle((ox + x, oy + y, ox + x + w - 1, oy + y + h - 1), outline=(0, 255, 0), width=2)
        draw.rectangle((ox + 8, oy + 8, ox + 180, oy + 34), fill=(0, 0, 0))
        draw.text((ox + 14, oy + 13), f"{label} {MOUTH_LEVELS[i]}", fill=(255, 255, 255))
    canvas.save(target, "PNG")


def suggest_eye(frames: list[Frame], hold: dict[str, int], width: int, height: int) -> tuple[dict, dict]:
    sw, sh = 120, 120
    ids = np.linspace(hold["start"], hold["end"], min(16, hold["end"] - hold["start"] + 1), dtype=int)
    samples = np.stack([gray_small(frames[int(i)], (sw, sh)) for i in ids])
    motion = np.mean(np.abs(np.diff(samples, axis=0)), axis=0) if len(samples) > 1 else np.zeros((sh, sw))
    x0, x1 = int(sw * 0.22), int(sw * 0.78)
    y0, y1 = int(sh * 0.22), int(sh * 0.42)
    ww, wh = max(20, int(sw * 0.40)), max(10, int(sh * 0.12))
    best = (-1.0, x0, y0)
    for y in range(y0, max(y0 + 1, y1 - wh + 1), 3):
        for x in range(x0, max(x0 + 1, x1 - ww + 1), 3):
            score = float(motion[y:y + wh, x:x + ww].mean())
            if score > best[0]:
                best = (score, x, y)
    _, x, y = best
    rect = {
        "x": int(round(x * width / sw)),
        "y": int(round(y * height / sh)),
        "width": int(round(ww * width / sw)),
        "height": int(round(wh * height / sh)),
    }
    return clamp_rect(rect, width, height), {"motion_score": round(best[0], 3)}


def choose_eye_frames(frames: list[Frame], hold: dict, rect: dict) -> dict[str, int]:
    ids = list(range(hold["start"], hold["end"] + 1))
    arrays = [crop_array(rgb(frames[i]).astype(np.float32), rect) for i in ids]
    # Darker = more closed for typical eye crops.
    brightness = np.asarray([float(a.mean()) for a in arrays])
    order = np.argsort(brightness)  # low → closed
    return {
        "closed": ids[int(order[0])],
        "half": ids[int(order[len(order) // 2])],
        "open": ids[int(order[-1])],
    }


def save_preview(frame: Frame, rect: dict, target: Path, label: str) -> None:
    with Image.open(io.BytesIO(frame.data)) as im:
        canvas = im.convert("RGB")
    draw = ImageDraw.Draw(canvas)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    draw.rectangle((x, y, x+w-1, y+h-1), outline=(255, 0, 0), width=4)
    draw.rectangle((8, 8, 8 + max(120, len(label)*8), 34), fill=(0, 0, 0))
    draw.text((14, 13), label, fill=(255, 255, 255))
    canvas.save(target, "PNG")


def frame_index(frames: list[Frame], duration_ms: int) -> list[dict]:
    return [{"frame": i, "offset": f.offset, "size": len(f.data), "duration_ms": duration_ms,
             "sha256": sha256(f.data)} for i, f in enumerate(frames)]


def build_one(source: Path, out: Path, profile: dict, duration_ms: int, quality: int) -> tuple[dict, dict]:
    data = source.read_bytes()
    frames = scan_mjpeg(data)
    first = rgb(frames[0])
    height, width = first.shape[:2]
    for i in sorted(set((0, len(frames)//2, len(frames)-1))):
        a = rgb(frames[i])
        if a.shape[:2] != (height, width):
            raise ValueError(f"mixed dimensions at frame {i}")
    seg = segments(len(frames), profile.get("segments"))
    suggested_rect, detection = suggest_mouth(frames, seg["hold"], width, height)
    rect = clamp_rect(profile.get("mouth_roi", suggested_rect), width, height)
    picks, activity = choose_mouth_frames(frames, seg["hold"], rect)
    if "mouth_frames" in profile:
        for level in MOUTH_LEVELS:
            picks[level] = int(profile["mouth_frames"][level])
            if not (seg["hold"]["start"] <= picks[level] <= seg["hold"]["end"]):
                raise ValueError(f"{level} frame must be inside hold segment")
    risk = risk_report(frames, picks, rect, width, height)
    source_risk = dict(risk)
    base = rgb(frames[picks["closed"]])
    pose_1_frame, pose_delta = choose_second_pose(
        frames, seg["hold"], rect, picks["closed"], activity, profile.get("pose_1_frame"))
    pose_bases = [base, rgb(frames[pose_1_frame])]
    feather_px = int(profile.get("mouth_feather_px", 10))
    align_px = max(0, min(8, int(profile.get("mouth_align_px", 4))))
    # Normalize pose 1 to the same closed-mouth semantic state before building its atlas.
    pose1_closed, _ = mouth_composite(pose_bases[1], base, rect, feather_px, align_px)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    pose_bases[1] = pose_bases[1].copy()
    pose_bases[1][y:y+h, x:x+w] = pose1_closed
    pose_mouth, pose_alignment = [], []
    for pose_index, pose_base in enumerate(pose_bases):
        mouth_patches, alignment = {}, {}
        for level in MOUTH_LEVELS:
            candidate = pose_base if level == "closed" else rgb(frames[picks[level]])
            mouth_patches[level], alignment[level] = mouth_composite(
                pose_base, candidate, rect, feather_px, align_px)
        pose_mouth.append(mouth_patches)
        pose_alignment.append(alignment)
    mouth_patches, alignment = pose_mouth[0], pose_alignment[0]
    composite_edge = max(v["composite_edge_delta"] for v in alignment.values())
    risk = dict(risk)
    risk.update({"source_layered_safe": source_risk["layered_safe"],
                 "source_seam_border_delta": source_risk["seam_border_delta"],
                 "composite_edge_delta": composite_edge,
                 "layered_safe": composite_edge <= 1.0,
                 "recommended_render_mode": "layered" if composite_edge <= 1.0 else "full_frame_clip",
                 "reason": "mouth patches share canonical base with feathered edges"})
    requested_mode = profile.get("render_mode", "auto")
    if requested_mode == "auto" and profile.get("force_layered"):
        requested_mode = "layered"
    mode = risk["recommended_render_mode"] if requested_mode == "auto" else requested_mode
    if mode not in ("full_frame_clip", "fixed_roi_clip", "layered", "static"):
        raise ValueError(f"unsupported render mode: {mode}")
    if mode == "layered" and not risk["layered_safe"]:
        risk = dict(risk)
        risk["forced_layered"] = True
        risk["reason"] = (risk.get("reason", "") +
                          "; layered forced by profile/CLI — review mouth_roi before SD deploy")

    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, out / "frames.mjpeg")
    patches = out / "mouth"
    patches.mkdir(exist_ok=True)
    for level in MOUTH_LEVELS:
        save_patch_array(mouth_patches[level], patches / f"{level}.jpg", quality)
        save_rgb565_array(mouth_patches[level], patches / f"{level}.rgb565")

    eye_rect, eye_det = suggest_eye(frames, seg["hold"], width, height)
    eye_rect = clamp_rect(profile.get("eye_roi", eye_rect), width, height)
    eye_picks = choose_eye_frames(frames, seg["hold"], eye_rect)
    if "eye_frames" in profile:
        for level in EYE_LEVELS:
            eye_picks[level] = int(profile["eye_frames"][level])
    eyes = out / "eye"
    eyes.mkdir(exist_ok=True)
    pose_eyes = []
    eye_feather_px = int(profile.get("eye_feather_px", 8))
    for pose_base in pose_bases:
        eye_set = {}
        for level in EYE_LEVELS:
            eye_set[level], _ = mouth_composite(
                pose_base, rgb(frames[eye_picks[level]]), eye_rect, eye_feather_px, align_px)
        pose_eyes.append(eye_set)
    for level in EYE_LEVELS:
        eye_patch = pose_eyes[0][level]
        save_patch_array(eye_patch, eyes / f"{level}.jpg", quality)
        save_rgb565_array(eye_patch, eyes / f"{level}.rgb565")

    hold_base = out / "hold_base.jpg"
    hold_base.write_bytes(frames[picks["closed"]].data)
    save_rgb565_array(base, out / "hold_base.rgb565")
    save_composite_preview(base, mouth_patches, rect, out / "mouth_roi_preview.png", source.stem)
    save_preview(frames[eye_picks["open"]], eye_rect, out / "eye_roi_preview.png", f"{source.stem}: eye ROI")

    pose_1_dir = out / "pose_1"
    (pose_1_dir / "mouth").mkdir(parents=True, exist_ok=True)
    (pose_1_dir / "eye").mkdir(exist_ok=True)
    Image.fromarray(pose_bases[1], "RGB").save(pose_1_dir / "hold_base.jpg", "JPEG", quality=quality, optimize=True)
    save_rgb565_array(pose_bases[1], pose_1_dir / "hold_base.rgb565")
    for level in MOUTH_LEVELS:
        save_patch_array(pose_mouth[1][level], pose_1_dir / "mouth" / f"{level}.jpg", quality)
        save_rgb565_array(pose_mouth[1][level], pose_1_dir / "mouth" / f"{level}.rgb565")
    for level in EYE_LEVELS:
        save_patch_array(pose_eyes[1][level], pose_1_dir / "eye" / f"{level}.jpg", quality)
        save_rgb565_array(pose_eyes[1][level], pose_1_dir / "eye" / f"{level}.rgb565")
    save_composite_preview(pose_bases[1], pose_mouth[1], rect,
                           out / "mouth_roi_preview_pose_1.png", f"{source.stem} pose1")

    life_layer = build_life_layer(frames, seg["hold"], picks["closed"], base,
                                  rect, eye_rect, out, source.stem)

    manifest = {
        "schema": "dialogue-emotion-clip-v2", "emotion": source.stem,
        "source": {"file": "frames.mjpeg", "bytes": len(data), "sha256": sha256(data),
                   "width": width, "height": height, "frame_count": len(frames)},
        "render": {"mode": mode, "fallback": "full_frame_clip", "mouth_roi": rect,
                   "hold_base": {"frame": picks["closed"], "file": "hold_base.jpg",
                                 "rgb565": "hold_base.rgb565", "width": width, "height": height},
                   "mouth_composite": {"method": "canonical_base_feather_v1",
                                       "feather_px": feather_px, "alignment_px": align_px,
                                       "levels": alignment},
                   "mouth_levels": {
                       k: {"frame": picks[k], "file": f"mouth/{k}.jpg", "rgb565": f"mouth/{k}.rgb565"}
                       for k in MOUTH_LEVELS
                   },
                   "eye_roi": eye_rect,
                   "eye_levels": {
                       k: {"frame": eye_picks[k], "file": f"eye/{k}.jpg", "rgb565": f"eye/{k}.rgb565"}
                       for k in EYE_LEVELS
                   },
                   "pose_bank": {"version": 1, "count": 2, "switch_min_ms": 6000,
                                 "switch_max_ms": 12000, "blink_masked": True,
                                 "poses": [
                                     {"id": 0, "base_frame": picks["closed"], "root": "."},
                                     {"id": 1, "base_frame": pose_1_frame, "root": "pose_1"}
                                 ]},
                   "life_layer": life_layer},
        "segments": seg, "frames": frame_index(frames, duration_ms),
        "analysis": {"mouth_detection": detection, "mouth_activity": activity, "risk": risk,
                     "eye_detection": eye_det,
                     "pose_bank": {"pose_1_frame": pose_1_frame, "pose_delta": pose_delta}},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    generated = {"segments": seg, "mouth_roi": rect, "mouth_frames": picks,
                 "eye_roi": eye_rect, "eye_frames": eye_picks,
                 "pose_1_frame": pose_1_frame,
                 "render_mode": mode, "auto_suggested_mouth_roi": suggested_rect}
    return manifest, generated


def validate_clip(folder: Path, manifest: dict) -> list[str]:
    errors: list[str] = []
    data = (folder / manifest["source"]["file"]).read_bytes()
    if sha256(data) != manifest["source"]["sha256"]:
        errors.append("stream SHA-256 mismatch")
    hold = manifest["render"].get("hold_base", {})
    hold_rgb = folder / hold.get("rgb565", "")
    expected_hold = int(hold.get("width", 0)) * int(hold.get("height", 0)) * 2
    if not hold_rgb.is_file() or hold_rgb.stat().st_size != expected_hold:
        errors.append("missing or invalid canonical hold_base rgb565")
    for item in manifest["frames"]:
        chunk = data[item["offset"]:item["offset"] + item["size"]]
        if not (chunk.startswith(b"\xff\xd8") and chunk.endswith(b"\xff\xd9")):
            errors.append(f"frame {item['frame']} JPEG boundary invalid")
        if sha256(chunk) != item["sha256"]:
            errors.append(f"frame {item['frame']} hash mismatch")
    for level, item in manifest["render"]["mouth_levels"].items():
        if not (folder / item["file"]).is_file():
            errors.append(f"missing mouth patch {level}")
        rgb565 = item.get("rgb565")
        if rgb565 and not (folder / rgb565).is_file():
            errors.append(f"missing mouth rgb565 {level}")
    for level, item in manifest["render"].get("eye_levels", {}).items():
        if not (folder / item["file"]).is_file():
            errors.append(f"missing eye patch {level}")
        rgb565 = item.get("rgb565")
        if rgb565 and not (folder / rgb565).is_file():
            errors.append(f"missing eye rgb565 {level}")
    pose_bank = manifest["render"].get("pose_bank", {})
    for pose in pose_bank.get("poses", [])[1:]:
        root = folder / pose["root"]
        if (root / "hold_base.rgb565").stat().st_size != expected_hold:
            errors.append(f"invalid pose base {pose['id']}")
        for level in MOUTH_LEVELS:
            if not (root / "mouth" / f"{level}.rgb565").is_file():
                errors.append(f"missing pose {pose['id']} mouth {level}")
        for level in EYE_LEVELS:
            if not (root / "eye" / f"{level}.rgb565").is_file():
                errors.append(f"missing pose {pose['id']} eye {level}")
    life = manifest["render"].get("life_layer", {})
    if life:
        if life.get("base_rgb565_sha256") != sha256(hold_rgb.read_bytes()):
            errors.append("life layer base hash mismatch")
        for track in life.get("tracks", []):
            roi = track.get("roi", {})
            expected = int(roi.get("width", 0)) * int(roi.get("height", 0))
            if int(roi.get("height", 0)) > LIFE_TRACK_ROWS:
                errors.append(f"life track {track.get('id')} exceeds row budget")
            for item in track.get("frames", []):
                rgb_path = folder / item["rgb565"]
                mask_path = folder / item["mask_a8"]
                if not rgb_path.is_file() or rgb_path.stat().st_size != expected * 2:
                    errors.append(f"invalid life RGB565 {rgb_path.name}")
                if not mask_path.is_file() or mask_path.stat().st_size != expected:
                    errors.append(f"invalid life mask {mask_path.name}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Build dialogue emotion manifest v2 and volume-driven mouth assets")
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--profile", type=Path)
    ap.add_argument("--frame-duration-ms", type=int, default=160)
    ap.add_argument("--quality", type=int, default=82)
    ap.add_argument("--force-layered", action="store_true",
                    help="Force render_mode=layered even when risk says unsafe (review ROI!)")
    args = ap.parse_args()
    if args.input.resolve() == args.output.resolve() or args.input.resolve() in args.output.resolve().parents:
        raise ValueError("output must not be the input directory or inside it")
    profile = load_profile(args.profile)
    if args.force_layered:
        profile["force_layered"] = True
        for emo in EMOTIONS:
            profile.setdefault("emotions", {}).setdefault(emo, {})["render_mode"] = "layered"
    args.output.mkdir(parents=True, exist_ok=True)
    clips, generated, rows = {}, {"schema": "dialogue-emotion-profile-v1", "emotions": {}}, []
    for emotion in EMOTIONS:
        source = args.input / f"{emotion}.mjpeg"
        if not source.is_file():
            raise FileNotFoundError(source)
        manifest, suggestion = build_one(source, args.output / emotion,
                                         profile.get("emotions", {}).get(emotion, {}),
                                         args.frame_duration_ms, args.quality)
        clips[emotion] = f"{emotion}/manifest.json"
        generated["emotions"][emotion] = suggestion
        risk = manifest["analysis"]["risk"]
        rows.append({"emotion": emotion, "frames": manifest["source"]["frame_count"],
                     "mode": manifest["render"]["mode"], "layered_safe": risk["layered_safe"],
                     "source_seam_delta": risk["source_seam_border_delta"],
                     "composite_edge_delta": risk["composite_edge_delta"],
                     "global_delta": risk["global_frame_delta"]})
    pack = {
        "schema": "dialogue-emotion-pack-v2", "version": 2,
        "state_machine": {"states": ["standby", "enter", "hold", "exit"],
                          "emotion_slots": ["requested", "pending", "committed"],
                          "tts_flow": ["enter", "hold", "exit"],
                          "mouth_signal": {"source": "smoothed_audio_rms", "levels": list(MOUTH_LEVELS),
                                           "delivery": "latest_value", "lvgl_from_audio_task": False}},
        "capabilities": ["full_frame_clip", "fixed_roi_clip", "layered", "static"],
        "legacy_fallback": {"enabled": True, "files": [f"{e}.mjpeg" for e in EMOTIONS]},
        "clips": clips,
    }
    (args.output / "pack_manifest.json").write_text(json.dumps(pack, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.output / "profile.generated.json").write_text(json.dumps(generated, ensure_ascii=False, indent=2), encoding="utf-8")
    with (args.output / "analysis_report.csv").open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys())
        writer.writeheader(); writer.writerows(rows)
    errors = []
    for emotion, rel in clips.items():
        manifest = json.loads((args.output / rel).read_text(encoding="utf-8"))
        errors.extend(f"{emotion}: {e}" for e in validate_clip(args.output / emotion, manifest))
    validation = {"ok": not errors, "errors": errors, "clips": len(clips)}
    (args.output / "validation_report.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(args.output), "validation": validation, "analysis": rows}, ensure_ascii=False, indent=2))
    return 0 if not errors else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
