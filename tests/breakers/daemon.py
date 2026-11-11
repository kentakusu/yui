#!/usr/bin/env python3
"""陽性対照（デーモン）: yuid.c をわざと壊して、tests/daemon.sh が落ちるか見る。"""
import hashlib
import os
import shutil
import subprocess
import tempfile
import sys

# このファイルの 2 つ上（tests/breakers/ → tests/ → リポジトリの根）
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "yuid.c")
BAK = os.path.join(tempfile.gettempdir(), "yuid.c.orig")

BREAKS = [
    (
        "締めの行（end）を出さない",
        [
            (
                '    buf_str(out, failed ? "{\\"kind\\":\\"end\\",\\"rc\\":1" : "{\\"kind\\":\\"end\\",\\"rc\\":0");',
                '    buf_str(out, failed ? "{\\"kind\\":\\"owari\\",\\"rc\\":1" : "{\\"kind\\":\\"owari\\",\\"rc\\":0");',
            )
        ],
    ),
    (
        "ほかが先に持っているときも 1 で終わる（建て直せるかが分からなくなる）",
        [("        return busy ? 3 : 1;", "        return 1;")],
    ),
    (
        "終わる で接続を切らない",
        [("                bye = 1; /* ★この接続を切るだけ。デーモンは止めません */", "                bye = 0;")],
    ),
    (
        "最初の誤りで残りの行を捨てる",
        [
            (
                "                failed = 1;\n                /* ★止めません。次の行へ進みます（CLI と同じ）。",
                "                failed = 1;\n                bye = 1;\n                /* ★止めません。次の行へ進みます（CLI と同じ）。",
            )
        ],
    ),
    (
        "読むだけで開かない（YUI_READONLY を外す）",
        [
            (
                "if (yui_open_locked(path, name, writable ? 0 : YUI_READONLY, &db) != YUI_OK) {",
                "if (yui_open_locked(path, name, 0, &db) != YUI_OK) {",
            )
        ],
    ),
    (
        "NUL の検査を外す",
        [("        if (strlen(line) != j - i) {", "        if (0) {")],
    ),
    (
        "長さの行の検査を外す（数字かどうか見ない）",
        [
            (
                '                *why = "長さが数字ではありません";\n                return -1;',
                "                len = len * 0;\n                break;",
            )
        ],
    ),
    (
        "長さの上限を外す",
        [
            (
                '                *why = "問い合わせが大きすぎます（4 MB まで）";\n                return -1;',
                "                len = MAX_REQ;",
            )
        ],
    ),
    (
        "空き時間で終わらない",
        [
            (
                "            if (idle > 0 && nconn == 0 && (long)(time(NULL) - last) >= idle) {",
                "            if (0) {",
            )
        ],
    ),
    (
        "待って送る作りに戻す（読まない相手で全員 止まる）",
        [("                    set_nonblock(s);", "                    (void)0;")],
    ),
    (
        "返りの上限を無視する",
        [
            (
                "    if (k->max_rows > 0 && k->rows >= k->max_rows) {",
                "    if (0) {",
            )
        ],
    ),
    (
        "上限で打ち切ったことを言わない",
        [("    if (sink.cut) {", "    if (0) {")],
    ),
    (
        "溜まりが膨らんでも接続を切らない",
        [
            (
                "            if (!drop && conns[i].out.n > max_out) {",
                "            if (0) {",
            )
        ],
    ),
    (
        "錠を取らない（ファイルの持ち主にならない）",
        [
            (
                "if (yui_open_locked(path, name, writable ? 0 : YUI_READONLY, &db) != YUI_OK) {",
                "if (yui_open(path, &db) != YUI_OK) {",
            )
        ],
    ),
    (
        '名乗っていない接続にも書かせる',
        [
            (
                'if (writable && yui_set_readonly(db, (c->me ? 0 : 1)) != YUI_OK) {',
                'if (writable && yui_set_readonly(db, 0) != YUI_OK) {',
            ),
        ],
    ),
    (
        "時刻を線でなく「機」の点の属性に置く（文書の例の形。上書きで消えます）",
        [
            (
                '        buf_str(&q, "\\")-[書いた {時: \\"");\n        buf_str(&q, when);\n        buf_str(&q, "\\"}]->(機:\\"");\n        buf_stmt_str(&q, me); /* ★名乗りは生の文字列なので、ここで逃がします */\n        buf_str(&q, "\\")");',
                '        buf_str(&q, "\\")-[書いた]->(機:\\"");\n        buf_stmt_str(&q, me);\n        buf_str(&q, "\\" {時: \\"");\n        buf_str(&q, when);\n        buf_str(&q, "\\"})");',
            )
        ],
    ),
    (
        '往復できない名前にも黙って押す（別の点に付き、結ぶ が無い点を作ります）',
        [
            (
                '        if (!stamp_safe(k->tgt[i].label) || !stamp_safe(k->tgt[i].name)) {',
                '        if (0) {',
            ),
        ],
    ),
    (
        '読む文でも押す（書き込みかどうかを見ない）',
        [
            (
                '            if (yui_writes(db) > w0 && c->me && c->stamp) {',
                '            if (c->me && c->stamp) {',
            ),
        ],
    ),
    (
        '「印なし」を無視して押す',
        [
            (
                '            if (yui_writes(db) > w0 && c->me && c->stamp) {',
                '            if (yui_writes(db) > w0 && c->me) {',
            ),
        ],
    ),
    (
        '押しきれなかったことを黙る',
        [
            (
                '    if (k->tgt_over) {',
                '    if (0) {',
            ),
        ],
    ),
]


def build():
    r = subprocess.run(["make", "-s", "yuid", "build/yuid_say", "yui"], cwd=REPO,
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def run():
    r = subprocess.run(["sh", "tests/daemon.sh", "./yuid", "./build/yuid_say", "./yui"],
                       cwd=REPO, capture_output=True, text=True, timeout=600)
    ng = [ln for ln in r.stdout.splitlines() if ln.startswith("NG ")]
    return r.returncode, ng


def main():
    shutil.copy(SRC, BAK)
    orig = open(BAK, encoding="utf-8").read()
    rc, err = build()
    if rc != 0:
        print("素でビルドできません:\n" + err[-1500:])
        return 1
    rc, ng = run()
    print("=== 素の状態 === 終了コード %d / NG %d 件" % (rc, len(ng)))
    if rc != 0:
        for ln in ng:
            print("   " + ln)
        print("★素で落ちています。対照が取れません")
        return 1

    bad = []
    for name, reps in BREAKS:
        text = orig
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
        open(SRC, "w", encoding="utf-8").write(text)
        brc, berr = build()
        print("=== %s ===" % name)
        if brc != 0:
            print("  ビルドできません:\n" + berr[-500:])
            bad.append(name + "（ビルド不能）")
        else:
            try:
                rc2, ng2 = run()
            except subprocess.TimeoutExpired:
                print("  時間切れ（＝止まった。これも落ちたと数えます）")
                rc2, ng2 = 1, ["(時間切れ)"]
            print("  終了コード %d / NG %d 件" % (rc2, len(ng2)))
            for ln in ng2[:6]:
                print("   " + ln)
            if rc2 == 0:
                print("  ★落ちませんでした ＝ この壊れ方を見ている検査が無い")
                bad.append(name + "（落ちない）")
        open(SRC, "w", encoding="utf-8").write(orig)

    now = hashlib.md5(open(SRC, "rb").read()).hexdigest()
    was = hashlib.md5(open(BAK, "rb").read()).hexdigest()
    print("=== 後始末 === yuid.c は元どおり: %s" % (now == was))
    build()
    if now != was or bad:
        for b in bad:
            print("  - " + b)
        return 1
    print("\nぜんぶの壊し方で落ちました ＝ 検査に検出力があります")
    return 0


sys.exit(main())
