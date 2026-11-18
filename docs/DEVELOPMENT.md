# 開発ガイド

## ビルド

### Docker（おすすめ）

devkitPro を入れなくても、公式のイメージでビルドできます。

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitarm:latest make
```

### devkitPro

```sh
dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib
make          # thalia.3dsx
make cia      # thalia.cia（makerom と bannertool が PATH に必要）
```

フォント（`fonts/*.ttf`）は、ビルドのときに `mkbcfnt` で表示用の `romfs/font/*.bcfnt` に変換されます。本文は Zen Maru Gothic、ロゴの文字は Cinzel Decorative です。

## 配布

GitHub Actions（`.github/workflows/build.yml`）が、push のたびに `thalia.3dsx` と `thalia.cia` を作って Artifacts に置きます。
`v` で始まるタグを push すると、Releases に `thalia.3dsx`・`thalia.cia`・`qr.png` が置かれます。README の QR コードは、最新リリースの `thalia.cia` を指しています。

```sh
git tag v1.0.0
git push origin v1.0.0
```

CIA を作る makerom と bannertool は、CI の中で版を固定してダウンロードし、SHA-256 で中身を確かめてから使っています。

## ソースの構成

```
source/
├── main.c      画面とタブ、入力、上画面の表示
├── ui.c        描画の部品（角丸・ボタン・選択枠・フォント・テクスチャ）
├── fx.c        演出（起動時のロゴ、読み込み中の点の輪・きらめき・進捗バー）
├── nai.c       通信と裏で動く処理（生成・編集・ディレクターツール・タグ検索・残高）
├── config.c    設定の保存と読み込み、リクエスト JSON の組み立て、Anlas の見積もり
├── util.c      3DS に依存しない処理（JSON・ZIP・PNG・縮小・Base64）
└── imgcodec.c  画像の展開（PNG は stb_image、WebP は libwebp）
third_party/libwebp/  libwebp v1.6.0 の展開部分だけ（makefile.unix の DEC / DSP_DEC / UTILS_DEC）
cia/            CIA の設定（app.rsf）、HOME メニューのバナーと起動音
fonts/          同梱フォントと、フォントに入れる文字の一覧
romfs/          アプリに埋め込むファイル（ロゴ、証明書、変換後のフォント）
tools/          素材を作るスクリプト
test/           PC で動かすテスト
```

画像はメモリを抑えるため、次のように扱っています。

- 透過のない PNG は最初から RGB で展開する（RGBA を作ってから変換しない）
- WebP は表示用の大きさに縮小しながら展開する（元の大きさの画像を作らない）。PNG は展開して縮小したら、元の大きさの画像をすぐ手放す
- 受信バッファは Content-Length ちょうどの大きさで確保し、1 枚だけ受け取るときは画像を取り出したら ZIP をすぐ手放す
- 設定の「受信形式」が WebP のときは `"image_format":"webp"`（可逆）で受け取り、そのまま `.webp` で保存する。img2img などで送るときは PNG に作り直す

一覧から開く画像は、初回に展開したときに 3DS 表示用（長辺 1024px、RGB565、zlib 圧縮）の `.vw` として保存し、2 回目からはそれを読みます（`nai.c` の `finish_view()` / `load_view_cache()`）。

API が変わったときに直す場所は、`source/config.c` の `request_build_ex()`（生成・img2img・インペイント）と `director_build()`（ディレクターツール）です。

## 文言とフォント

- 画面の文言は `L("日本語", "English")` で書きます。表示する言語は設定の「言語」で決まります
- 文言に新しい漢字を足したら、小さい文字用フォントの収録文字を作り直します。入っていない文字は本文用フォントで代わりに描かれるので、忘れても表示はされます

```sh
python tools/charset.py
```

## 素材

| 出力 | スクリプト | 元にするもの |
| --- | --- | --- |
| `icon.png`、`romfs/gfx/*.png` | `tools/make_assets.py` | NovelAI プレスキットの `Logos/logo_light.svg`、NovelAI の Web 画面の Anlas アイコン（`/_next/static/media/anla.*.svg`） |
| `cia/banner.png`、`cia/audio.wav`、`docs/banner.png`、`docs/qr.png` | `tools/make_release_art.py` | 同じくプレスキットのロゴ（QR コードの URL は第 2 引数で変更可） |
| `docs/showcase.jpg`、`docs/screenshots/ll/*.jpg` | `tools/frame_screenshots.py` | Evan-Amos による 3DS LL の写真（[Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Nintendo-3DS-XL-angled.png)、パブリックドメイン）に、`docs/screenshots/*.png`（Azahar で撮った画面）と `docs/screenshots/showcase/*.png` をはめこむ |

`make_release_art.py` には Pillow・fontTools・segno が、`frame_screenshots.py` には Pillow・numpy が必要です。

## テスト

3DS に依存しない部分（リクエストの組み立て、ZIP・PNG・WebP の処理、設定の保存と読み込み、Anlas の見積もり）を PC で確かめます。

```sh
gcc -std=gnu11 -Wall -Wextra -Isource -Ithird_party/libwebp test/host_test.c source/util.c source/config.c \
    source/imgcodec.c third_party/libwebp/src/*/*.c -lz -lm -lpthread -o host_test
./host_test <出力先フォルダ> <PNG入りのZIP>
```

出力先フォルダに `s_rgb.png` / `s_rgb.webp` / `s_rgba.png` / `s_rgba.webp`（同じ画像の PNG と可逆 WebP）と、WebP を 1 枚入れた `img_webp.zip` を置くと、WebP の展開も確かめます。

生成・img2img・インペイントのリクエストが、非公式 SDK（[caru-ini/novelai-sdk](https://github.com/caru-ini/novelai-sdk)）の出力と同じになることを確かめます（Pillow が必要）。

```sh
PYTHONPATH=<novelai-sdk>/src python3 test/compare_sdk.py <出力先>/requests.jsonl <出力先>/requests_edit.jsonl
```

画面の確認には、エミュレータの [Azahar](https://github.com/azahar-emu/azahar) で `thalia.3dsx` を直接開けます。SD カードの中身は Windows なら `%APPDATA%\Azahar\sdmc` にあります。

## 動作確認の状況（v1.0.0）

- [x] devkitARM でビルドできる
- [x] Azahar で、全画面の表示と操作、一覧、マスク塗り、言語の切り替えが動く
- [x] Azahar から NovelAI まで HTTPS（証明書の確認を含む）で届く
- [x] 生成・img2img・インペイントのリクエストが SDK の出力と一致する（13 パターン）
- [x] Anlas の見積もりが SDK のテスト値と一致する
- [ ] GitHub Actions での CIA の作成
- [ ] 本物の API キーでの生成・編集の結果
- [ ] V5 の残量表示（Opus のアカウント）
- [ ] 実機での動作（速度・メモリ・Wi-Fi）
