"""配布用の素材を作る（開発時に実行。出力はリポジトリに含める）

使い方: python tools/make_release_art.py <Press Kit の Logos/logo_light.svg> [CIAのURL]

出力:
  cia/banner.png   HOMEメニューの上画面に出るバナー（256x128）
  cia/audio.wav    HOMEメニューでアイコンを選んだときの音（約1.6秒）
  docs/banner.png  README の見出し画像
  docs/qr.png      FBI の「Scan QR Code」で読み取る QR コード（CIAのURL）
必要なもの: Pillow, fontTools, segno
"""
import math
import struct
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_assets import CREAM, NAVY, NAVY_HI, NAVY_LO, PURPLE, SS, load_svg, nib_3d, nib_mask, rounded_mask, solid, vgrad  # noqa: E402
from PIL import Image, ImageDraw, ImageFilter, ImageFont  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BOLD = str(ROOT / "fonts" / "ZenMaruGothic-Bold.ttf")
MEDIUM = str(ROOT / "fonts" / "ZenMaruGothic-Medium.ttf")
TITLE = str(ROOT / "fonts" / "CinzelDecorative-Bold.ttf")  # ロゴの「THALIA」
SERIF = str(ROOT / "fonts" / "Cinzel[wght].ttf")           # 添え書き
DEFAULT_URL = "https://github.com/Tesixki/thalia/releases/latest/download/thalia.cia"


def sparkle(draw, x, y, s, color):
    w = s * 0.26
    draw.polygon([(x, y - s), (x + w, y), (x, y + s), (x - w, y)], fill=color)
    draw.polygon([(x - s, y), (x, y - w), (x + s, y), (x, y + w)], fill=color)


def glow_layer(shape, size, box, color, blur, strength):
    """ペン先の形をぼかした光"""
    w, h = size
    m = Image.new("L", size, 0)
    m.paste(nib_mask(shape, box[2], box[3]), (box[0], box[1]))
    m = m.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: min(255, int(v * strength)))
    return solid(w, h, color, m)


def text_layer(size, xy, text, font, color):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).text(xy, text, font=font, fill=color)
    return layer


def dark_backdrop(W, H, glow_xy, glow_r):
    """紫紺の斜めグラデーションに、紫のにじみと細いしま"""
    im = Image.new("RGBA", (W, H))
    px = im.load()
    a, b = (0x34, 0x2C, 0x66), (0x12, 0x13, 0x26)
    for y in range(H):
        for x in range(W):
            t = min(1.0, (x / W) * 0.55 + (y / H) * 0.6)
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)
    halo = Image.new("L", (W, H), 0)
    gx, gy = glow_xy
    ImageDraw.Draw(halo).ellipse((gx - glow_r, gy - glow_r, gx + glow_r, gy + glow_r), fill=150)
    halo = halo.filter(ImageFilter.GaussianBlur(glow_r * 0.45))
    im.alpha_composite(solid(W, H, PURPLE, halo))
    stripes = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stripes)
    step = max(3, H // 64)
    for y in range(0, H, step * 2):
        sd.rectangle((0, y, W, y + step - 1), fill=(255, 255, 255, 6))
    im.alpha_composite(stripes)
    return im


def gold_text(size, xy, text, font, spacing=0, shadow=True, stroke=0):
    """金色のグラデーションの文字（影付き）"""
    W, H = size
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    x, y = xy
    for ch in text:
        md.text((x, y), ch, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
        x += md.textlength(ch, font=font) + spacing
    box = mask.getbbox() or (0, 0, W, H)
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for yy in range(H):
        t = min(1.0, max(0.0, (yy - box[1]) / max(1, box[3] - box[1])))
        if t < 0.5:
            c = tuple(int(v) for v in (0xFF + (0xF5 - 0xFF) * t * 2, 0xFD + (0xF3 - 0xFD) * t * 2, 0xF0 + (0xC2 - 0xF0) * t * 2))
        else:
            u = (t - 0.5) * 2
            c = tuple(int(v) for v in (0xF5 + (0xD9 - 0xF5) * u, 0xF3 + (0xB8 - 0xF3) * u, 0xC2 + (0x5E - 0xC2) * u))
        gd.line((0, yy, W, yy), fill=c + (255,))
    grad.putalpha(mask)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if shadow:
        r = max(1, H // 90)
        sh = solid(W, H, (0x08, 0x06, 0x18), mask.point(lambda v: int(v * 0.7)))
        sh = sh.filter(ImageFilter.GaussianBlur(r * 1.5))
        out.alpha_composite(sh, (r, r * 2))
    out.alpha_composite(grad)
    return out, box


def spaced_text(draw, xy, text, font, fill, spacing):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + spacing
    return x


def ornament(draw, x0, x1, y, color):
    """細い飾り罫（真ん中に小さな菱形）"""
    cx = (x0 + x1) / 2
    draw.line((x0, y, cx - 6, y), fill=color, width=1)
    draw.line((cx + 6, y, x1, y), fill=color, width=1)
    draw.polygon([(cx, y - 3), (cx + 3, y), (cx, y + 3), (cx - 3, y)], fill=color)


def home_banner(shape):
    """HOMEメニューのバナー。紫紺の地に、立体のペン先と金色の THALIA"""
    W, H = 256, 128
    im = dark_backdrop(W, H, (50, 64), 60)
    d = ImageDraw.Draw(im)
    nib = nib_3d(shape, 62, 80)
    sh = solid(62, 80, (0x05, 0x04, 0x12), nib.split()[3].point(lambda v: int(v * 0.6))).filter(ImageFilter.GaussianBlur(3))
    im.alpha_composite(sh, (20, 28))
    im.alpha_composite(nib, (16, 22))
    f = ImageFont.truetype(TITLE, 30)
    title, box = gold_text((W, H), (92, 30), "THALIA", f, spacing=1)
    im.alpha_composite(title)
    ornament(d, 94, 94 + (box[2] - box[0]), 76, (0xD9, 0xB8, 0x5E, 255))
    f2 = ImageFont.truetype(SERIF, 12)
    f2.set_variation_by_axes([600])
    label = "for NovelAI"
    lw = sum(d.textlength(ch, font=f2) + 1.5 for ch in label) - 1.5
    spaced_text(d, (94 + ((box[2] - box[0]) - lw) / 2, 84), label, f2, (0xD6, 0xCF, 0xFF, 255), 1.5)
    for x, y, sz in ((236, 18, 4), (224, 108, 3), (88, 16, 3), (246, 64, 2), (8, 110, 2)):
        sparkle(d, x, y, sz, (0xF5, 0xF3, 0xC2, 210))
    return im


def jingle(path):
    """きらっとした3音（E6・G#6・B6）に、高い余韻を少し重ねる"""
    rate, length = 32728, 1.6
    n = int(rate * length)
    notes = [(0.00, 1318.5), (0.12, 1661.2), (0.24, 1975.5), (0.36, 2637.0)]
    out = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for start, f in notes:
            if t < start:
                continue
            u = t - start
            env = min(1.0, u / 0.006) * math.exp(-u * 3.2)
            v += env * (math.sin(2 * math.pi * f * u) + 0.25 * math.sin(2 * math.pi * f * 2 * u))
        out.append(int(max(-1, min(1, v * 0.22)) * 32767))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", s) for s in out))


def readme_banner(shape):
    W, H = 1280, 400
    im = dark_backdrop(W, H, (330, 200), 300)
    d = ImageDraw.Draw(im)
    nib = nib_3d(shape, 200, 258)
    sh = solid(200, 258, (0x05, 0x04, 0x12), nib.split()[3].point(lambda v: int(v * 0.55))).filter(ImageFilter.GaussianBlur(12))
    im.alpha_composite(sh, (238, 92))
    im.alpha_composite(nib, (226, 70))
    f = ImageFont.truetype(TITLE, 128)
    title, box = gold_text((W, H), (476, 70), "THALIA", f, spacing=4)
    im.alpha_composite(title)
    f2 = ImageFont.truetype(SERIF, 25)
    f2.set_variation_by_axes([600])
    sub = "NovelAI image generation for Nintendo 3DS"
    sw = sum(d.textlength(ch, font=f2) + 2 for ch in sub) - 2
    ornament(d, 484, 484 + max(box[2] - box[0], sw), 258, (0xD9, 0xB8, 0x5E, 255))
    spaced_text(d, (484, 278), sub, f2, (0xD6, 0xCF, 0xFF, 255), 2)
    for x, y, sz in ((1190, 70, 16), (1130, 120, 8), (1222, 170, 6), (170, 300, 10), (120, 90, 7), (1080, 330, 9), (720, 40, 6), (430, 350, 5)):
        sparkle(d, x, y, sz, (0xF5, 0xF3, 0xC2, 220))
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(im, (0, 0), rounded_mask(W, H, 28))
    return out


def qr_card(shape, url):
    import segno

    qr = segno.make(url, error="h", micro=False)
    scale, border = 12, 4
    matrix = list(qr.matrix)
    n = len(matrix)
    size = (n + border * 2) * scale
    im = Image.new("RGBA", (size, size), (255, 255, 255, 255))
    d = ImageDraw.Draw(im)
    for y, row in enumerate(matrix):
        for x, v in enumerate(row):
            if v:
                x0, y0 = (x + border) * scale, (y + border) * scale
                d.rectangle((x0, y0, x0 + scale - 1, y0 + scale - 1), fill=NAVY + (255,))
    # 真ん中にロゴ（誤り訂正 H なので一部を隠しても読める。面積は1割未満）
    box = int(size * 0.2)
    bx = by = (size - box) // 2
    badge = vgrad(box, box, NAVY_HI, NAVY_LO).convert("RGBA")
    pad = int(box * 0.18)
    badge.paste(Image.new("RGBA", (box - 2 * pad, box - 2 * pad), CREAM + (255,)), (pad, pad), nib_mask(shape, box - 2 * pad, box - 2 * pad))
    d.rounded_rectangle((bx - 6, by - 6, bx + box + 5, by + box + 5), int(box * 0.24), fill=(255, 255, 255, 255))
    im.paste(badge, (bx, by), rounded_mask(box, box, int(box * 0.2)))
    return im, n


def main():
    shape = load_svg(sys.argv[1])
    url = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_URL
    (ROOT / "cia").mkdir(exist_ok=True)
    (ROOT / "docs").mkdir(exist_ok=True)
    home_banner(shape).convert("RGB").save(ROOT / "cia" / "banner.png")
    jingle(ROOT / "cia" / "audio.wav")
    readme_banner(shape).save(ROOT / "docs" / "banner.png")
    qr, n = qr_card(shape, url)
    qr.convert("RGB").save(ROOT / "docs" / "qr.png")
    print(f"ok (QR {n}x{n} modules): {url}")


if __name__ == "__main__":
    main()
