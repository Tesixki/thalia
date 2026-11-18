"""3DS側が組み立てるリクエストJSONを、非公式Python SDK (caru-ini/novelai-sdk) の出力と突き合わせる。

使い方: PYTHONPATH=<novelai-sdk>/src python3 compare_sdk.py <requests.jsonl> [requests_edit.jsonl]
"""
import json
import sys

from PIL import Image

from novelai.types import Character, GenerateImageParams
from novelai.types.user.image import I2iParams, InpaintParams

G = {0: 0.1, 1: 0.3, 2: 0.5, 3: 0.7, 4: 0.9}

CASES = [
    dict(model="nai-diffusion-5-full", prompt='1girl, "quoted" back\\slash, 日本語', negative_prompt="bad hands", seed=12345),
    dict(model="nai-diffusion-5-full-medium", prompt="1boy, city", seed=1),
    dict(
        model="nai-diffusion-4-5-curated", prompt="2girls, park", uc_preset="human_focus", seed=4294967294,
        size=(1216, 832), variety_boost=True,
        characters=[
            Character(prompt="girl, red hair", negative_prompt="hat", position=(G[1], G[2])),
            Character(prompt="girl, blue hair", negative_prompt=""),
            Character(prompt="boy", negative_prompt="", enabled=False),
        ],
    ),
    dict(model="nai-diffusion-3", prompt="1girl, smile:1.2|landscape", negative_prompt="text", uc_preset="strong",
         sampler="k_dpmpp_sde", noise_schedule="exponential", steps=28, scale=6.5, seed=7),
    dict(model="nai-diffusion-4-full", prompt="nsfw, 1girl", quality=False, uc_preset="none", cfg_rescale=0.3, seed=99),
    dict(model="nai-diffusion-4-5-full", prompt="1girl, ||a|b||, tree | extra part", uc_preset="furry_focus", seed=5),
    dict(model="nai-diffusion-3", prompt="cat", uc_preset="none", negative_prompt="dog", seed=6),
    dict(model="nai-diffusion-3", prompt="cat", uc_preset="none", seed=6),
    dict(model="nai-diffusion-5-curated", prompt="scenery", uc_preset="strong", seed=8),
    dict(model="nai-diffusion-4-5-full", prompt="1girl", seed=10, image_format="webp"),
]


def _img(w, h):
    return Image.new("RGB", (w, h), (120, 140, 160))


def _mask(w, h):
    m = Image.new("RGB", (w, h), (0, 0, 0))
    m.paste((255, 255, 255), (64, 64, 256, 256))
    return m


# host_test.c の edit_requests() と同じ順番
EDIT_CASES = [
    dict(model="nai-diffusion-5-full", prompt="1girl, smile", seed=42, size=(1024, 704),
         i2i=I2iParams(image=_img(1024, 704), strength=0.55, noise=0.1, seed=777)),
    dict(model="nai-diffusion-5-curated", prompt="cat", seed=43, size=(832, 1216),
         inpaint=InpaintParams(image=_img(832, 1216), mask=_mask(832, 1216), strength=0.8, seed=888)),
    dict(model="nai-diffusion-3", prompt="landscape", seed=44, size=(832, 1216),
         i2i=I2iParams(image=_img(832, 1216), seed=5)),
    dict(model="nai-diffusion-4-curated-preview", prompt="dog", negative_prompt="blurry", seed=45, size=(1216, 832),
         inpaint=InpaintParams(image=_img(1216, 832), mask=_mask(1216, 832), seed=6)),
]


def strip_images(req):
    """画像は3DS側では別パートで送り、JSONにはパート名だけを書く。SDKのto_api_requestはbase64を入れるのでそろえる"""
    p = req.get("parameters", {})
    for k in ("image", "mask"):
        if k in p:
            p[k] = k
    return req


def norm(o):
    """数値の表記ゆれ（5 と 5.0）をそろえる"""
    if isinstance(o, dict):
        return {k: norm(v) for k, v in o.items()}
    if isinstance(o, list):
        return [norm(v) for v in o]
    if isinstance(o, (int, float)) and not isinstance(o, bool):
        return round(float(o), 4)
    if isinstance(o, str):
        return o.replace(" ,", ",")  # 3DS側はタグの前の余分な空白を詰める（意味は同じ）
    return o


def diff(a, b, path=""):
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a:
                out.append(f"{path}/{k}: 3DS側に無い (SDK={b[k]!r})")
            elif k not in b:
                out.append(f"{path}/{k}: SDK側に無い (3DS={a[k]!r})")
            else:
                out += diff(a[k], b[k], f"{path}/{k}")
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            out += diff(x, y, f"{path}[{i}]")
    elif a != b:
        out.append(f"{path}: 3DS={a!r} SDK={b!r}")
    return out


def check(path, cases, label):
    mine = [json.loads(line) for line in open(path, encoding="utf-8")]
    assert len(mine) == len(cases), (len(mine), len(cases))
    bad = 0
    for i, (m, kw) in enumerate(zip(mine, cases)):
        kw = dict(kw)
        kw.setdefault("size", (832, 1216))
        params = GenerateImageParams(**kw)
        req = strip_images(params.to_api_request(None).model_dump(mode="json", exclude_none=True))
        d = diff(norm(m), norm(req))
        print(f"{label} {i} ({kw['model']}): {'一致' if not d else '不一致'}")
        for line in d:
            print("   ", line)
        bad += bool(d)
    return bad


def main():
    bad = check(sys.argv[1], CASES, "case")
    if len(sys.argv) > 2:
        bad += check(sys.argv[2], EDIT_CASES, "edit")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
