#!/usr/bin/env bash
# 重新生成 LVGL 中文裁剪字库（多档）。
#
#   tools/gen_font.sh                # 生成全部档位
#   SIZES="16" tools/gen_font.sh     # 只生成某一档
#
# 依赖: fonttools (pip) + node 的 lv_font_conv (npm) + 系统里的 Noto Sans CJK。
# 生成物: src/ui/fonts/ui_font_cn_<size>.c  (提交进仓, CI 只编译不生成)
#
# 字符集: tools/font_charset.txt —— 界面用到的全部中文 + ASCII。
#         改了界面文案就要把新字加进这个文件, 否则屏幕上显示成空白。
#
# 为什么分档: 一档 16px 撑不起层级 —— 标题、正文、状态行、提示全是同一个字号，
# 屏幕上只能靠位置和留白硬凑。分档后标题能更大、正文能收窄，信号格和 SSID
# 才不会在真机上挤成一团（模拟屏正文 11.5~12px，固件原 16px）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SRC_TTF="${SRC_TTF:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}"
SIZES="${SIZES:-12 16 20}"
BPP="${BPP:-4}"

echo "[gen_font] source: $SRC_TTF  sizes=$SIZES bpp=$BPP"

# 字体子集与字号无关 —— 切一次给所有档位复用。
python3 "$ROOT/tools/subset_font.py" "$SRC_TTF" "$TMP/subset.ttf" "$ROOT/tools/font_charset.txt"

if [ -n "${LV_FONT_CONV:-}" ] && [ -x "${LV_FONT_CONV}" ]; then
    # 已经装好了就不必再走 npm（本机 npm 直连才能成，装一次就够）
    CONV="${LV_FONT_CONV}"
elif [ -x /tmp/lvtest/node_modules/.bin/lv_font_conv ]; then
    CONV=/tmp/lvtest/node_modules/.bin/lv_font_conv
else
    # npm 走直连: 本机出口代理会把 registry 挡成 403
    (cd "$TMP" && env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
        -u all_proxy -u ALL_PROXY npm install --silent lv_font_conv@1.5.3)
    CONV="$TMP/node_modules/.bin/lv_font_conv"
fi

echo "[gen_font] lv_font_conv: $CONV"

# --symbols 需要的是**字符本身**, 不是码位。
# 注意: 不能用 `fold -w1` 拆字符 —— 它按**字节**切, 会把 UTF-8 中文切碎,
# lv_font_conv 收到乱码后会把中文全部丢掉 (实测: 只剩 95 个 ASCII 字形)。
SYMBOLS=$(python3 - "$ROOT/tools/font_charset.txt" <<'PY'
import sys
chars = set()
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    chars.update(line)
chars.update(chr(c) for c in range(0x20, 0x7F))
sys.stdout.write("".join(sorted(chars)))
PY
)

for SIZE in $SIZES; do
    OUT="$ROOT/src/ui/fonts/ui_font_cn_${SIZE}.c"
    (cd "$TMP" && "$CONV" \
        --font "$TMP/subset.ttf" \
        --size "$SIZE" --bpp "$BPP" --format lvgl --lv-include lvgl.h \
        --no-compress --no-kerning --lv-font-name "ui_font_cn_${SIZE}" \
        --symbols "$SYMBOLS" \
        -o "$OUT")
    echo "[gen_font] wrote $OUT ($(wc -c < "$OUT") bytes)"
done

# 生成物自检 —— 字符集里每个字都必须在字库里有字形。
# 少了就是屏上空白（而且 CI 只编译不生成，不会有人发现）。
python3 - "$ROOT/tools/font_charset.txt" "$ROOT/src/ui/fonts" <<'PY'
import glob
import os
import re
import sys

charset_path, fonts_dir = sys.argv[1], sys.argv[2]
chars = set()
for line in open(charset_path, encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    chars.update(line)
chars.update(chr(c) for c in range(0x20, 0x7F))
want = {ord(c) for c in chars}

files = sorted(glob.glob(os.path.join(fonts_dir, "ui_font_cn_*.c")))
if not files:
    sys.exit("gen_font: 字体目录里没有生成的字库")
bad = False
for path in files:
    with open(path, encoding="utf-8") as fp:
        src = fp.read()
    have = {int(m, 16) for m in re.findall(r"U\+([0-9A-Fa-f]{4,6})", src)}
    missing = sorted(want - have)
    if missing:
        bad = True
        print("gen_font: %s 缺 %d 个字形: %s"
              % (os.path.basename(path), len(missing),
                 "".join(chr(c) for c in missing[:20])))
    else:
        print("gen_font: %s 字形齐全 (%d 字)" % (os.path.basename(path), len(have)))
if bad:
    sys.exit("gen_font: 字库缺字 —— 先把字加进 font_charset.txt 再重新生成")
PY
