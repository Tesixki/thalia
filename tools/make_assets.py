"""ロゴ素材とアイコンを作る（開発時に一度だけ実行。出力はリポジトリに含める）

使い方: python tools/make_assets.py <Press Kit の Logos/logo_light.svg> <NovelAI WebUI の anla.svg>
  anla.svg は https://novelai.net の画面で使われている Anlas のアイコン（/_next/static/media/anla.*.svg）

出力:
  icon.png                 SMDH 用 48x48 アイコン
  romfs/gfx/nib_l.png      上画面のロゴ（ペン先マーク、紺）
  romfs/gfx/nib_s.png      ステータスバー用の小さいロゴ（クリーム）
  romfs/gfx/nib_w.png      ボタンなどに乗せる白いロゴ
  romfs/gfx/anlas.png      ステータスバーの Anlas アイコン
  romfs/gfx/nib_lw.png, nib_lc.png, glow.png  ロゴの演出用（白・クリーム・にじみ）
必要なもの: Pillow, fontTools
"""
import re
import sys
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.svgLib.path import parse_path
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SS = 8  # スーパーサンプリング倍率

NAVY = (0x21, 0x23, 0x35)
NAVY_HI = (0x3A, 0x3D, 0x5C)
NAVY_LO = (0x10, 0x12, 0x24)
CREAM = (0xF5, 0xF3, 0xC2)
PURPLE = (0x97, 0x73, 0xFF)


class PolyPen(BasePen):
    """曲線を折れ線にして、サブパスごとの点列を集める"""

    def __init__(self):
        super().__init__(None)
        self.polys, self.cur = [], []

    def _moveTo(self, p):
        self.cur = [p]

    def _lineTo(self, p):
        self.cur.append(p)

    def _curveToOne(self, p1, p2, p3):
        p0 = self.cur[-1]
        for i in range(1, 17):
            t = i / 16
            mt = 1 - t
            self.cur.append((
                mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0],
                mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1],
            ))

    def _qCurveToOne(self, p1, p2):
        p0 = self.cur[-1]
        for i in range(1, 17):
            t = i / 16
            mt = 1 - t
            self.cur.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                             mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))

    def _closePath(self):
        if len(self.cur) > 2:
            self.polys.append(self.cur)
        self.cur = []

    _endPath = _closePath


def load_svg(svg_path):
    """SVGの全パスを折れ線にする。(点列のリスト, viewBoxの幅, 高さ) を返す"""
    svg = Path(svg_path).read_text(encoding="utf-8")
    vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
    pen = PolyPen()
    for d in re.findall(r'<path[^>]*\sd="([^"]+)"', svg):
        parse_path(d, pen)
    return (pen.polys, vb[2], vb[3])


def nib_mask(shape, w, h, pad=0.0, ss=SS):
    """図形を w x h に収めたアルファ（even-odd 塗り）"""
    polys, vw, vh = shape
    s = min((w - 2 * pad) / vw, (h - 2 * pad) / vh)
    ox = (w - vw * s) / 2
    oy = (h - vh * s) / 2
    W, H = w * ss, h * ss
    acc = Image.new("1", (W, H), 0)
    for poly in polys:
        im = Image.new("1", (W, H), 0)
        ImageDraw.Draw(im).polygon([((ox + x * s) * ss, (oy + y * s) * ss) for x, y in poly], fill=1)
        acc = ImageChops.logical_xor(acc, im)
    return acc.convert("L").resize((w, h), Image.LANCZOS)


def solid(w, h, rgb, alpha):
    im = Image.new("RGBA", (w, h), rgb + (0,))
    im.putalpha(alpha)
    return im


def vgrad(w, h, top, bottom):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return im


def rounded_mask(w, h, r):
    m = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), r * SS, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def _lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def nib_3d(shape, w, h, depth=0.13, light=1.0):
    """厚みと光沢のある立体のペン先（RGBA, w x h）。depth は高さに対する厚みの割合"""
    Q = 4  # 内部で描く倍率
    W, H = w * Q, h * Q
    d = max(2, int(h * Q * depth))
    dx, dy = 0.45, 0.9  # 厚みが伸びる向き（右下）
    fw = int((W - d * dx) * 0.98)
    fh = int((H - d * dy) * 0.98)
    front = nib_mask(shape, fw, fh, ss=2)
    # 側面（奥ほど暗い紺、手前ほど紫）
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for i in range(d, 0, -1):
        t = i / d
        col = _lerp((0x7A, 0x58, 0xF0), (0x18, 0x16, 0x34), t)
        layer = Image.new("RGBA", (fw, fh), col + (255,))
        out.paste(layer, (int(i * dx), int(i * dy)), front)
    # 表面（上は明るいクリーム、下は金色）
    face = Image.new("RGBA", (fw, fh))
    fd = ImageDraw.Draw(face)
    for y in range(fh):
        t = y / max(1, fh - 1)
        fd.line((0, y, fw, y), fill=_lerp((0xFF, 0xFE, 0xF0), (0xE2, 0xC9, 0x6E), t) + (255,))
    # 左上から光が当たるふち（明）と、右下のふち（暗）
    k = max(2, fh // 60)
    blur = front.filter(ImageFilter.GaussianBlur(k * 1.6))
    a = ImageChops.offset(blur, -k, -k)
    b = ImageChops.offset(blur, k, k)
    hi = ImageChops.subtract(a, b).point(lambda v: min(255, int(v * 1.6 * light)))
    lo = ImageChops.subtract(b, a).point(lambda v: min(255, int(v * 1.2)))
    face.alpha_composite(solid(fw, fh, (255, 255, 255), ImageChops.multiply(hi, front)))
    face.alpha_composite(solid(fw, fh, (0x8A, 0x6A, 0x20), ImageChops.multiply(lo, front)))
    # 光沢の帯（斜め）
    band = Image.new("L", (fw, fh), 0)
    ImageDraw.Draw(band).polygon([(0, fh * 0.18), (fw, -fh * 0.05), (fw, fh * 0.12), (0, fh * 0.36)], fill=90)
    band = band.filter(ImageFilter.GaussianBlur(fh / 40))
    face.alpha_composite(solid(fw, fh, (255, 255, 255), ImageChops.multiply(band, front)))
    face.putalpha(front)
    out.alpha_composite(face, (0, 0))
    return out.resize((w, h), Image.LANCZOS)


def make_icon(polys):
    W = H = 48
    bg = vgrad(W, H, NAVY_HI, NAVY_LO).convert("RGBA")
    # 紫のにじみ
    glow = nib_mask(polys, W, H, pad=6).filter(ImageFilter.GaussianBlur(4)).point(lambda v: v * 0.7)
    bg = Image.composite(Image.new("RGBA", (W, H), PURPLE + (255,)), bg, glow)
    # 厚みは右下にのびる。厚みを含めた外枠をまん中にすると左上に、明るい表面をまん中にすると右下に
    # 寄って見えるので、表面の中心 3：外枠の中心 1 の点をまん中に置く。半端な位置になるので 4 倍で描いて縮める
    nw, nh, q = 32, 38, 4
    nib = nib_3d(polys, nw, nh)
    bright = Image.eval(nib.convert("L"), lambda v: 255 if v > 150 else 0)
    opaque = Image.eval(nib.split()[3], lambda v: 255 if v > 128 else 0)
    fx0, fy0, fx1, fy1 = ImageChops.multiply(bright, opaque).getbbox()
    bx0, by0, bx1, by1 = nib.split()[3].getbbox()
    cx = (3 * (fx0 + fx1) + bx0 + bx1) / 8
    cy = (3 * (fy0 + fy1) + by0 + by1) / 8
    layer = Image.new("RGBA", (W * q, H * q), (0, 0, 0, 0))
    layer.alpha_composite(nib_3d(polys, nw * q, nh * q), (round((W / 2 - cx) * q), round((H / 2 - cy) * q)))
    bg.alpha_composite(layer.convert("RGBa").resize((W, H), Image.LANCZOS).convert("RGBA"))
    # 上半分のつや（当時のアイコンらしい光沢）
    gloss = Image.new("L", (W * SS, H * SS), 0)
    ImageDraw.Draw(gloss).ellipse((-W * SS * 0.4, -H * SS * 1.05, W * SS * 1.4, H * SS * 0.48), fill=38)
    gloss = gloss.resize((W, H), Image.LANCZOS)
    bg.alpha_composite(solid(W, H, (255, 255, 255), gloss))
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(bg, (0, 0), rounded_mask(W, H, 9))
    out.save(ROOT / "icon.png")


def main():
    polys = load_svg(sys.argv[1])
    gfx = ROOT / "romfs" / "gfx"
    gfx.mkdir(parents=True, exist_ok=True)
    make_icon(polys)
    solid(44, 55, NAVY, nib_mask(polys, 44, 55)).save(gfx / "nib_l.png")
    solid(11, 14, CREAM, nib_mask(polys, 11, 14)).save(gfx / "nib_s.png")
    solid(13, 16, (255, 255, 255), nib_mask(polys, 13, 16)).save(gfx / "nib_w.png")
    # 演出用: 光の筋を重ねる白、暗い背景用のクリーム、後ろのにじみ
    solid(44, 55, (255, 255, 255), nib_mask(polys, 44, 55)).save(gfx / "nib_lw.png")
    solid(44, 55, CREAM, nib_mask(polys, 44, 55)).save(gfx / "nib_lc.png")
    glow = Image.new("L", (84, 95), 0)
    glow.paste(nib_mask(polys, 44, 55), (20, 20))
    glow = glow.filter(ImageFilter.GaussianBlur(7)).point(lambda v: min(255, int(v * 1.9)))
    solid(84, 95, (255, 255, 255), glow).save(gfx / "glow.png")
    if len(sys.argv) > 2:
        solid(13, 12, CREAM, nib_mask(load_svg(sys.argv[2]), 13, 12)).save(gfx / "anlas.png")
    print("ok")


if __name__ == "__main__":
    main()
