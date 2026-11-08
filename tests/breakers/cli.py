#!/usr/bin/env python3
"""陽性対照（CLI の --read / --wait）: わざと壊して tests/cli.sh が落ちるか見る。"""
import hashlib
import os
import shutil
import subprocess
import tempfile
import sys

# このファイルの 2 つ上（tests/breakers/ → tests/ → リポジトリの根）
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILES = {"yui.c": os.path.join(tempfile.gettempdir(), "pc_yui.c"), "yui_cli.c": os.path.join(tempfile.gettempdir(), "pc_yui_cli.c")}

BREAKS = [
    (
        "--read でも排他の錠を取る（共有にしない）",
        "yui.c",
        [("    int op = db->shared ? LOCK_SH : LOCK_EX;", "    int op = LOCK_EX;")],
    ),
    (
        "--wait を無視する",
        "yui_cli.c",
        [("                      wait_s * 1000, &db) != YUI_OK) {", "                      0, &db) != YUI_OK) {")],
    ),
    (
        "CLI が <パス>.錠 を取ってしまう",
        "yui_cli.c",
        [
            (
                "    if (yui_open_full(path, NULL,\n                      YUI_NO_LOCKFILE | (readonly ? (YUI_READONLY | YUI_SHARED) : 0),",
                '    if (yui_open_full(path, "yui-cli",\n                      (readonly ? (YUI_READONLY | YUI_SHARED) : 0),',
            )
        ],
    ),
    (
        "知らない旗をファイル名として扱う",
        "yui_cli.c",
        [
            (
                '        } else if (strncmp(argv[ai], "--", 2) == 0) {',
                '        } else if (0) {',
            )
        ],
    ),
    (
        "詰め直す が元の権限を引き継がない（秘匿のものを分けた意味が消える）",
        "yui.c",
        [("    if (keep_mode >= 0 && fchmod(fd, (mode_t)keep_mode) != 0) {",
          "    if (0 && keep_mode >= 0 && fchmod(fd, (mode_t)keep_mode) != 0) {")],
    ),
    (
        "読むだけでも書けてしまう",
        "yui.c",
        [("    if (CMDS[i].writes && db->readonly) {", "    if (0) {")],
    ),
]


def build():
    r = subprocess.run(["make", "-s", "yui"], cwd=REPO, capture_output=True, text=True)
    return r.returncode, r.stderr


def run():
    r = subprocess.run(["sh", "tests/cli.sh", "./yui"], cwd=REPO,
                       capture_output=True, text=True, timeout=600)
    return r.returncode, [ln for ln in r.stdout.splitlines() if ln.startswith("NG ")]


def main():
    orig = {}
    for f, bak in FILES.items():
        shutil.copy(os.path.join(REPO, f), bak)
        orig[f] = open(bak, encoding="utf-8").read()

    rc, err = build()
    if rc != 0:
        print("素でビルドできません:\n" + err[-1200:])
        return 1
    rc, ng = run()
    print("=== 素の状態 === 終了コード %d / NG %d 件" % (rc, len(ng)))
    if rc != 0:
        for ln in ng:
            print("   " + ln)
        return 1

    bad = []
    for name, target, reps in BREAKS:
        text = orig[target]
        miss = False
        for old, new in reps:
            if text.count(old) != 1:
                print("=== %s ===\n  ★当てる場所が %d 件。壊せていません" % (name, text.count(old)))
                bad.append(name + "（場所なし）")
                miss = True
                break
            text = text.replace(old, new)
        if miss:
            continue
        open(os.path.join(REPO, target), "w", encoding="utf-8").write(text)
        brc, berr = build()
        print("=== %s ===" % name)
        if brc != 0:
            print("  ビルドできません:\n" + berr[-400:])
            bad.append(name + "（ビルド不能）")
        else:
            try:
                rc2, ng2 = run()
            except subprocess.TimeoutExpired:
                print("  時間切れ（＝止まった。落ちたと数えます）")
                rc2, ng2 = 1, ["(時間切れ)"]
            print("  終了コード %d / NG %d 件" % (rc2, len(ng2)))
            for ln in ng2[:6]:
                print("   " + ln)
            if rc2 == 0:
                print("  ★落ちませんでした ＝ この壊れ方を見ている検査が無い")
                bad.append(name + "（落ちない）")
        open(os.path.join(REPO, target), "w", encoding="utf-8").write(orig[target])

    okall = True
    for f, bak in FILES.items():
        now = hashlib.md5(open(os.path.join(REPO, f), "rb").read()).hexdigest()
        was = hashlib.md5(open(bak, "rb").read()).hexdigest()
        print("=== 後始末 === %s は元どおり: %s" % (f, now == was))
        if now != was:
            okall = False
    build()
    if bad or not okall:
        for b in bad:
            print("  - " + b)
        return 1
    print("\nぜんぶの壊し方で落ちました ＝ 検査に検出力があります")
    return 0


sys.exit(main())
