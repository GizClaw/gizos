#!/usr/bin/env python3
"""Generate Qi Duel's embedded H2A4 assets from approved PNG sources."""

from __future__ import annotations

import argparse
import pathlib
import subprocess


def render_rgba(source: pathlib.Path, width: int, height: int,
                crop: tuple[int, int, int, int] | None = None) -> bytearray:
    command = ["magick", str(source)]
    if crop is not None:
        x, y, crop_width, crop_height = crop
        command += ["-crop", f"{crop_width}x{crop_height}+{x}+{y}", "+repage"]
    command += ["-resize", f"{width}x{height}!", "rgba:-"]
    pixels = bytearray(subprocess.check_output(command))
    expected = width * height * 4
    if len(pixels) != expected:
        raise RuntimeError(f"{source}: expected {expected} RGBA bytes, got {len(pixels)}")
    return pixels


def apply_hand_fade(pixels: bytearray, width: int, height: int, side: str) -> None:
    start = (91.0, 101.0) if side == "left" else (54.0, 101.0)
    end = (-8.0, 181.0) if side == "left" else (153.0, 181.0)
    vx, vy = end[0] - start[0], end[1] - start[1]
    denominator = vx * vx + vy * vy
    for y in range(height):
        for x in range(width):
            t = ((x - start[0]) * vx + (y - start[1]) * vy) / denominator
            t = max(0.0, min(1.0, t))
            if t <= 0.48:
                mask = 1.0
            elif t <= 0.8:
                mask = 1.0 + (0.22 - 1.0) * ((t - 0.48) / 0.32)
            else:
                mask = 0.22 * (1.0 - (t - 0.8) / 0.2)
            offset = (y * width + x) * 4 + 3
            pixels[offset] = round(pixels[offset] * mask)


def apply_horizontal_fade(pixels: bytearray, width: int, height: int,
                          direction: str) -> None:
    for y in range(height):
        for x in range(width):
            u = x / max(1, width - 1)
            if direction == "left":
                mask = u / 0.58 if u < 0.58 else 1.0
            else:
                mask = (1.0 - u) / 0.58 if u > 0.42 else 1.0
            mask = max(0.0, min(1.0, mask))
            offset = (y * width + x) * 4 + 3
            pixels[offset] = round(pixels[offset] * mask)


def mask_hud_cells(pixels: bytearray, width: int, side: str) -> None:
    start_x = 64 if side == "player" else 5
    for y in range(20, 38):
        for x in range(start_x, min(width, start_x + 116)):
            offset = (y * width + x) * 4
            pixels[offset:offset + 4] = b"\x00\x02\x05\xff"


def h2a4(width: int, height: int, rgba: bytearray) -> bytes:
    result = bytearray(b"H2A4")
    result += width.to_bytes(2, "little") + height.to_bytes(2, "little")
    for offset in range(0, len(rgba), 4):
        red, green, blue, alpha = rgba[offset:offset + 4]
        pixel = ((alpha >> 4) << 12) | ((red >> 4) << 8) | \
                ((green >> 4) << 4) | (blue >> 4)
        result += pixel.to_bytes(2, "little")
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-c", type=pathlib.Path, required=True)
    parser.add_argument("--output-h", type=pathlib.Path, required=True)
    args = parser.parse_args()

    source = args.source_dir
    assets: dict[str, bytes] = {}

    player_hud = render_rgba(source / "approved-mockup.png", 190, 55,
                             (12, 24, 398, 116))
    enemy_hud = render_rgba(source / "approved-mockup.png", 190, 55,
                            (726, 24, 398, 116))
    mask_hud_cells(player_hud, 190, "player")
    mask_hud_cells(enemy_hud, 190, "enemy")
    assets["player_hud"] = h2a4(190, 55, player_hud)
    assets["enemy_hud"] = h2a4(190, 55, enemy_hud)

    opponent = render_rgba(source / "opponent-full.png", 149, 156)
    assets["opponent"] = h2a4(149, 156, opponent)

    for side in ("left", "right"):
        hand = render_rgba(source / f"player-hand-{side}.png", 145, 177)
        apply_hand_fade(hand, 145, 177, side)
        assets[f"hand_{side}"] = h2a4(145, 177, hand)

    carousel = render_rgba(source / "carousel-base-v1.png", 368, 152)
    assets["carousel"] = h2a4(368, 152, carousel)

    sprite = source / "skill-icons-v1.png"
    skill_names = ("charge", "wave", "absorb", "guard")
    for index, name in enumerate(skill_names):
        icon = render_rgba(sprite, 96, 96,
                           ((index % 2) * 627, (index // 2) * 627, 627, 627))
        assets[f"skill_{name}"] = h2a4(96, 96, icon)
        for direction in ("left", "right"):
            faded = bytearray(icon)
            apply_horizontal_fade(faded, 96, 96, direction)
            assets[f"skill_{name}_{direction}"] = h2a4(96, 96, faded)

    guard = "H2_QI_DUEL_ASSETS_GENERATED_H"
    header_lines = [f"#ifndef {guard}", f"#define {guard}", "", "#include <stddef.h>",
                    "#include <stdint.h>", ""]
    source_lines = ['#include "qi_duel_assets.h"', ""]
    for name, data in assets.items():
        symbol = f"h2_qi_duel_asset_{name}"
        header_lines += [f"extern const uint8_t {symbol}[];",
                         f"extern const size_t {symbol}_size;"]
        source_lines.append(f"const uint8_t {symbol}[] = {{")
        for offset in range(0, len(data), 16):
            source_lines.append("    " + ", ".join(
                f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
        source_lines += ["};", f"const size_t {symbol}_size = sizeof({symbol});", ""]
    header_lines += ["", f"#endif /* {guard} */", ""]

    args.output_h.write_text("\n".join(header_lines), encoding="utf-8")
    args.output_c.write_text("\n".join(source_lines), encoding="utf-8")


if __name__ == "__main__":
    main()
