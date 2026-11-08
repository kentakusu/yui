#!/usr/bin/env python3
# 陽性対照: 錠の実装をわざと壊して、どの検査が落ちるかを見る。
# 落ちなければ、その検査は何も守っていない。
import glob
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import sys

# このファイルの 2 つ上（tests/breakers/ → tests/ → リポジトリの根）
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "yui.c")
BAK = os.path.join(tempfile.gettempdir(), "yui.c.orig")
BIN = "/tmp/yui_pc_test"
RUNDIR = "/tmp/yui_pc_run"

BREAKS = [
    (
        "閉じても錠を消さない",
        [("        remove(db->lockpath);", "        /* わざと壊した */;")],
    ),
    (
        "断られた側が持ち主の錠を消す",
        [
            (
                "        free(lpath);\n        return YUI_ERROR;",
                "        remove(lpath);\n        free(lpath);\n        return YUI_ERROR;",
            )
        ],
    ),
    (
        "名乗りの制御文字を掃除しない",
        [
            (
                "        buf[i] = (c < 0x20 || c == 0x7f) ? ' ' : src[i];",
                "        buf[i] = src[i];\n        (void)c;",
            )
        ],
    ),
    (
        "錠のファイル名を変える",
        [('#define LOCK_SUFFIX ".錠"', '#define LOCK_SUFFIX ".錠べつ"')],
    ),
    (
        "錠を flock より後に当てる（順番の誤り）",
        [
            (
                "    lpath = lock_path_of(path);\n    tag = lock_tag();",
                "    if (open_body(db, wait_ms) != YUI_OK) return YUI_ERROR;\n"
                "    lpath = lock_path_of(path);\n    tag = lock_tag();",
            ),
            (
                "    /* ★ここから先で失敗しても、yui_close が錠を外します */\n"
                "    db->lockpath = lpath;\n    return open_body(db, wait_ms);",
                "    db->lockpath = lpath;\n    return YUI_OK;",
            ),
        ],
    ),
    (
        "旗が無くても勝手に壊して取る",
        [
            (
                "    if (rc == 1 && (flags & YUI_LOCK_BREAK)) {",
                "    if (rc == 1) {",
            )
        ],
    ),
    (
        "印に秒より細かい時計・乱数・連番を混ぜない",
        [
            (
                "    return r ^ fine ^ (t << 20) ^ (p << 7) ^ a ^ (a >> 19) ^ (++seq << 40);",
                "    (void)r;\n    (void)fine;\n    (void)seq;\n"
                "    return (t << 20) ^ (p << 7) ^ a ^ (a >> 19);",
            )
        ],
    ),
    (
        "読むだけの関所を外す",
        [("    if (CMDS[i].writes && db->readonly) {", "    if (0) {")],
    ),
    (
        "表で 消す を「書かない」扱いにする",
        [('{"消す", cmd_delete, 1}', '{"消す", cmd_delete, 0}')],
    ),
    (
        "共有で開くときに読むだけを求めない",
        [
            (
                '        set_err(db, "共有で開くなら、読むだけ（YUI_READONLY）も付けてください");\n        return YUI_ERROR;',
                "        db->shared = 0;",
            )
        ],
    ),
    (
        "共有でも <パス>.錠 を取ってしまう",
        [
            (
                "    if ((flags & YUI_NO_LOCKFILE) || db->shared) return open_body(db, wait_ms);",
                "    if (flags & YUI_NO_LOCKFILE) return open_body(db, wait_ms);",
            )
        ],
    ),
    (
        "辿る の [?] の手がかりを出さない",
        [("    if (count == 0 && e.type && e.type[0] == '?') {", "    if (0) {")],
    ),
    (
        "壊して取ったあと読み返して確かめない",
        [
            (
                "            if (lock_slurp(lpath, got, sizeof got) != 0 || !strstr(got, want))\n"
                "                rc = 2;",
                "            if (0)\n                rc = 2;",
            )
        ],
    ),
]


def build():
    r = subprocess.run(
        [
            "cc", "-std=c99", "-Wall", "-Wextra", "-O1", "-DYUI_SELFCHECK",
            "-o", BIN, "yui.c", "tests/test_yui.c", "-lm",
        ],
        cwd=REPO, capture_output=True, text=True,
    )
    return r.returncode, r.stderr


def run():
    os.makedirs(RUNDIR, exist_ok=True)
    # ★ケースファイルも渡します。渡さないと direct_tests と錠の検査しか走らず、
    #   ケース（tests/cases/*.txt）で守っているものを壊しても落ちません。
    #   実際、これを渡していなかったせいで「辿る の [?] の手がかり」を
    #   壊しても落ちませんでした。
    cases = sorted(glob.glob(os.path.join(REPO, "tests", "cases", "*.txt")))
    if not cases:
        print("★ケースが 0 本です。置き場が違うかもしれません")
    r = subprocess.run([BIN] + cases, cwd=RUNDIR, capture_output=True, text=True)
    ng = [ln for ln in r.stdout.splitlines() if ln.startswith("NG ")]
    return r.returncode, ng, r.stdout


def main():
    shutil.copy(SRC, BAK)
    orig = open(BAK, encoding="utf-8").read()

    rc, err = build()
    if rc != 0:
        print("素の状態でビルドできません:\n" + err[-2000:])
        return 1
    rc, ng, _ = run()
    print("=== 素の状態 ===")
    print("  終了コード %d / NG %d 件" % (rc, len(ng)))
    if rc != 0 or ng:
        print("  ★素の状態で落ちています。対照が取れません")
        for ln in ng:
            print("   " + ln)
        return 1

    bad = []
    for name, reps in BREAKS:
        text = orig
        missing = False
        for old, new in reps:
            n = text.count(old)
            if n != 1:
                print('=== %s ===\n  ★当てる場所が %d 件（1 件のはず）。壊せていません' % (name, n))
                bad.append(name + "（場所が見つからない）")
                missing = True
                break
            text = text.replace(old, new)
        if missing:
            continue
        open(SRC, "w", encoding="utf-8").write(text)
        brc, berr = build()
        print("=== %s ===" % name)
        if brc != 0:
            print("  ビルドできません（壊し方が悪い）:\n" + berr[-800:])
            bad.append(name + "（ビルド不能）")
        else:
            rc2, ng2, _ = run()
            print("  終了コード %d / NG %d 件" % (rc2, len(ng2)))
            for ln in ng2:
                print("   " + ln)
            if rc2 == 0 or not ng2:
                print("  ★落ちませんでした ＝ この壊れ方を見ている検査が無い")
                bad.append(name + "（落ちない）")
        open(SRC, "w", encoding="utf-8").write(orig)

    # 元に戻ったことを突き合わせる
    now = hashlib.md5(open(SRC, "rb").read()).hexdigest()
    was = hashlib.md5(open(BAK, "rb").read()).hexdigest()
    print("=== 後始末 ===")
    print("  yui.c は元どおり: %s（%s）" % (now == was, now))
    if now != was:
        print("  ★戻っていません。/tmp/yui.c.orig から戻してください")
        return 1

    if bad:
        print("\n★検出力が無い／壊せなかったもの %d 件:" % len(bad))
        for b in bad:
            print("  - " + b)
        return 1
    print("\nぜんぶの壊し方で落ちました ＝ 検査に検出力があります")
    return 0


sys.exit(main())
