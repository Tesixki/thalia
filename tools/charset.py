"""フォントに入れる文字の一覧を作る（mkbcfnt の -w に渡す）

使い方: python tools/charset.py
  fonts/charset_full.txt  本文用: ASCII・Latin-1・JIS X 0208 の全部（プロンプトやタグの日本語名に使う）
  fonts/charset_ui.txt    小さい文字用: ASCII とソースに書いてある文字だけ（容量を抑える）
  fonts/charset_logo.txt  ロゴ用: ASCII だけ
画面の文言を変えたら実行し直す。入っていない文字は本文用フォントで代わりに描かれる。
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTRA = "…✓×・「」（）＋→←↑↓〜"


def ascii_set():
    return set(range(0x20, 0x7F))


def jis_set():
    cps = ascii_set() | set(range(0xA0, 0x100))
    for hi in list(range(0x81, 0xA0)) + list(range(0xE0, 0xF0)):
        for lo in range(0x40, 0xFD):
            try:
                ch = bytes([hi, lo]).decode("cp932")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and not 0xE000 <= ord(ch) <= 0xF8FF:
                cps.add(ord(ch))
    return cps | {ord(c) for c in EXTRA}


def source_set():
    cps = ascii_set() | {ord(c) for c in EXTRA}
    for p in sorted((ROOT / "source").glob("*.[ch]")):
        if p.name == "stb_image.h":
            continue
        for ch in p.read_text(encoding="utf-8"):
            if ord(ch) >= 0x80:
                cps.add(ord(ch))
    return cps


def write(name, cps):
    (ROOT / "fonts" / name).write_text("\n".join(str(c) for c in sorted(cps)) + "\n", encoding="ascii")
    print(f"{name}: {len(cps)} 文字")


if __name__ == "__main__":
    write("charset_full.txt", jis_set())
    write("charset_ui.txt", source_set())
    write("charset_logo.txt", ascii_set())
