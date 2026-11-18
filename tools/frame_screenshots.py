"""README の画像を、本物の 3DS LL の写真に画面をはめこんで作る（開発時に実行。出力はリポジトリに含める）

使い方: python tools/frame_screenshots.py <Nintendo-3DS-XL-angled.png>

写真: Evan-Amos「Nintendo-3DS-XL-angled.png」（Wikimedia Commons、パブリックドメイン）
      https://commons.wikimedia.org/wiki/File:Nintendo-3DS-XL-angled.png
      原寸（4740x3780）でも縮小版（例: 3840px）でもよい。画面の位置は写真の大きさに対する割合で持っている
入力: docs/screenshots/*.png            Azahar で撮った画面（上画面の下に、下画面を中央寄せ。800x960 など）
      docs/screenshots/showcase/*.png   見出し画像に使う画面（同じ並び。400x480）
出力: docs/screenshots/ll/*.jpg         スクリーンショットの節の画像（1 台ずつ）
      docs/showcase.jpg                 README の見出し画像（3 台）
必要なもの: Pillow, numpy
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SHOTS = ROOT / "docs" / "screenshots"

# 写真の中の画面の四隅（左上・右上・右下・左下）。3840x3062 の版で、灰色の液晶の縁に直線を当てはめて求めた値
REF = (3840, 3062)
TOP_QUAD = [(1667.2, 333.9), (3301.6, 777.4), (2843.0, 1655.2), (1193.4, 1182.8)]
BOT_QUAD = [(1211.3, 1490.0), (2540.4, 1870.8), (2020.2, 2548.7), (668.0, 2144.7)]
GROW = 2.0      # 写真の灰色が縁に残らないよう、はめこむ範囲を外へ広げる量（3840px の版でのピクセル）
GLARE = 0.35    # 写真の液晶に映っている照明の反射を、画面にどれだけ残すか

BG_TOP = (0xF3, 0xF0, 0xFE)   # 背景（docs/banner.png と同じ紫系の、明るいグラデーション）
BG_BOTTOM = (0xD6, 0xCB, 0xF6)
SHADOW = (0x2A, 0x1E, 0x5C)


def homography(src, dst):
    """src の 4 点を dst の 4 点へ移す射影変換（3x3）"""
    rows, rhs = [], []
    for (x, y), (u, v) in zip(src, dst):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); rhs.append(u)
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y]); rhs.append(v)
    h = np.linalg.solve(np.array(rows, float), np.array(rhs, float))
    return np.append(h, 1).reshape(3, 3)


def grow(quad, d):
    """四角形を、重心から外へ d だけ広げる"""
    cx = sum(p[0] for p in quad) / 4
    cy = sum(p[1] for p in quad) / 4
    out = []
    for x, y in quad:
        vx, vy = x - cx, y - cy
        n = (vx * vx + vy * vy) ** 0.5
        out.append((x + vx / n * d, y + vy / n * d))
    return out


def split_shot(shot):
    """上画面と下画面に分ける（下画面は横幅の 8 割で、中央にある）"""
    w, h = shot.size
    top = shot.crop((0, 0, w, h // 2))
    bw = w * 4 // 5
    bot = shot.crop(((w - bw) // 2, h // 2, (w + bw) // 2, h))
    return top, bot


def place(photo, screen, quad):
    """画面を写真の四角形にはめこむ。液晶の灰色に映っていた照明の明暗を、少しだけ重ねる"""
    W, H = photo.size
    sx, sy = W / REF[0], H / REF[1]
    q = [(x * sx, y * sy) for x, y in grow(quad, GROW)]
    # 出力の座標 → 画面の座標（PIL の PERSPECTIVE は逆向きの変換を使う）
    sw, sh = screen.size
    m = homography(q, [(0, 0), (sw, 0), (sw, sh), (0, sh)])
    x0, y0 = int(min(p[0] for p in q)) - 2, int(min(p[1] for p in q)) - 2
    x1, y1 = int(max(p[0] for p in q)) + 3, int(max(p[1] for p in q)) + 3
    # 切り出した範囲の左上を原点にした変換にする
    m = m @ np.array([[1, 0, x0], [0, 1, y0], [0, 0, 1]], float)
    m = m / m[2, 2]
    warped = screen.convert("RGB").transform((x1 - x0, y1 - y0), Image.PERSPECTIVE, tuple(m.flatten()[:8]), Image.BICUBIC)

    # 縁をなめらかにした四角形の形（4 倍で描いて縮める）
    ss = 4
    mask = Image.new("L", ((x1 - x0) * ss, (y1 - y0) * ss), 0)
    ImageDraw.Draw(mask).polygon([((x - x0) * ss, (y - y0) * ss) for x, y in q], fill=255)
    mask = mask.resize((x1 - x0, y1 - y0), Image.LANCZOS)

    # 液晶に映っていた照明: 写真の灰色の明るさのむらを、画面に足す
    region = photo.crop((x0, y0, x1, y1)).convert("RGB")
    lum = np.asarray(region.convert("L"), float)
    inside = np.asarray(mask) > 250
    glare = np.clip((lum - np.median(lum[inside])) * GLARE + 128, 0, 255).astype(np.uint8)
    glare = np.asarray(Image.fromarray(glare).filter(ImageFilter.GaussianBlur(3)), float) - 128
    px = np.asarray(warped, float) + glare[..., None]
    warped = Image.fromarray(np.clip(px, 0, 255).astype(np.uint8))

    photo.paste(warped, (x0, y0), mask)


def device(photo, shot):
    """1 台ぶん: 写真の 2 つの画面に、スクリーンショットの上下をはめこむ（RGBA、写真と同じ大きさ）"""
    top, bot = split_shot(shot)
    out = photo.copy()
    place(out, top, TOP_QUAD)
    place(out, bot, BOT_QUAD)
    return out


def backdrop(size):
    w, h = size
    col = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        col.putpixel((0, y), tuple(round(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3)))
    return col.resize((w, h)).convert("RGBA")


def shadow_of(dev, blur, strength, dy):
    """机に置いたときの影（本体の形をぼかして、下へずらす）"""
    a = dev.split()[3].filter(ImageFilter.GaussianBlur(blur)).point(lambda v: int(v * strength))
    sh = Image.new("RGBA", dev.size, SHADOW + (0,))
    sh.putalpha(a)
    out = Image.new("RGBA", (dev.width, dev.height + dy), (0, 0, 0, 0))
    out.alpha_composite(sh, (0, dy))
    return out


def card(dev, width):
    """スクリーンショットの節の 1 枚: 明るい背景に 1 台"""
    bbox = dev.split()[3].getbbox()
    dev = dev.crop(bbox)
    k = width * 0.86 / dev.width
    dev = dev.resize((round(dev.width * k), round(dev.height * k)), Image.LANCZOS)
    h = round(dev.height / 0.86 * 1.02)
    out = backdrop((width, h))
    x, y = (width - dev.width) // 2, round((h - dev.height) * 0.42)
    out.alpha_composite(shadow_of(dev, width / 60, 0.45, round(width / 70)), (x, y))
    out.alpha_composite(dev, (x, y))
    return out.convert("RGB")


def showcase(devs, width):
    """見出し画像: 3 台を少し重ねて並べる（まん中を大きく手前に）"""
    devs = [d.crop(d.split()[3].getbbox()) for d in devs]
    h = round(width * 0.44)
    out = backdrop((width, h))
    side_w, mid_w = width * 0.36, width * 0.45
    layout = [  # (台, 幅, 中心 x, 中心 y)
        (devs[0], side_w, width * 0.215, h * 0.53),
        (devs[2], side_w, width * 0.785, h * 0.53),
        (devs[1], mid_w, width * 0.5, h * 0.50),
    ]
    for dev, w, cx, cy in layout:
        k = w / dev.width
        d = dev.resize((round(dev.width * k), round(dev.height * k)), Image.LANCZOS)
        x, y = round(cx - d.width / 2), round(cy - d.height / 2)
        out.alpha_composite(shadow_of(d, width / 90, 0.5, round(width / 110)), (x, y))
        out.alpha_composite(d, (x, y))
    return out.convert("RGB")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    photo = Image.open(sys.argv[1]).convert("RGBA")
    out_dir = SHOTS / "ll"
    out_dir.mkdir(exist_ok=True)
    for path in sorted(SHOTS.glob("*.png")):
        img = card(device(photo, Image.open(path)), 900)
        dst = out_dir / (path.stem + ".jpg")
        img.save(dst, quality=90, optimize=True, progressive=True)
        print(dst, img.size)
    devs = [device(photo, Image.open(SHOTS / "showcase" / f"{n}.png")) for n in ("create", "edit", "inpaint")]
    img = showcase(devs, 1600)
    dst = ROOT / "docs" / "showcase.jpg"
    img.save(dst, quality=90, optimize=True, progressive=True)
    print(dst, img.size)


if __name__ == "__main__":
    main()
