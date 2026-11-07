#!/bin/sh
#
# CLI（yui_cli.c）のテスト。
#   sh tests/cli.sh ./yui
#
# ライブラリの側は tests/test_yui.c と tests/cases/*.txt が見ています。
# ここで見るのは、CLI にしか無いもの —— 終了コードと、標準入力の読み方です。
#
# ★Windows では動きません（sh が要ります）。CI の Windows では
#   .github/workflows/ci.yml の pwsh の段が同じことを見ています。
#   同じものを 2 回 書いているので、片方を直したらもう片方も直してください。

set -u
YUI=${1:-./yui}
[ -x "$YUI" ] || { echo "NG   $YUI がありません。先に make してください"; exit 2; }

D=$(mktemp -d) || exit 2
trap 'rm -rf "$D"' EXIT
ok=0
ng=0

# 確かめる: 名前 / 期待 / 実際
check() {
    if [ "$2" = "$3" ]; then
        ok=$((ok + 1))
    else
        ng=$((ng + 1))
        echo "NG   $1"
        echo "     期待: $2"
        echo "     実際: $3"
    fi
}
# 文字列が含まれているか
has() {
    case "$2" in
    *"$1"*) ok=$((ok + 1)) ;;
    *)
        ng=$((ng + 1))
        echo "NG   $3"
        echo "     「$1」が出ていません。出たのは: $2"
        ;;
    esac
}

# ---------------------------------------------------------------- 終了コード

"$YUI" > /dev/null 2>&1
check "引数が無いときの終了コード" 2 $?

"$YUI" a.yui b c > /dev/null 2>&1
check "引数が多いときの終了コード" 2 $?

"$YUI" "$D/t1.yui" '一覧 ()' > /dev/null 2>&1
check "成功したときの終了コード" 0 $?

"$YUI" "$D/t1.yui" 'これは誤り' > /dev/null 2>&1
check "失敗したときの終了コード" 1 $?

out=$("$YUI" "$D/t1.yui" 'これは誤り' 2>&1 > /dev/null)
has "エラー:" "$out" "失敗の理由が標準エラーに出ること"

# 開けないファイル（ディレクトリを渡す）
"$YUI" "$D" '一覧 ()' > /dev/null 2>&1
check "開けないファイルを渡したときの終了コード" 1 $?

# ---------------------------------------------------------------- 標準入力

# ★誤りの行があっても、そこで止まらずに次の行へ進む。
#   300 行 流して 1 行 間違っていたときに、残りを全部 捨てるほうが困るため。
printf '作る (点:一)\nこれは誤り\n作る (点:三)\n' > "$D/in1.txt"
err=$("$YUI" "$D/t2.yui" < "$D/in1.txt" 2>&1 > /dev/null)
rc=$?
check "誤りの行があるときの終了コード" 1 $rc
has "2 行目" "$err" "何行目で失敗したかが出ること"
left=$("$YUI" "$D/t2.yui" '一覧 (点:) 名前だけ' | tr '\n' ' ')
check "誤りの行の前後が実行されること" "(点:一) (点:三) 2 件 " "$left"

# ★NUL が入った行。
#   古い版は NUL から先を次の行で上書きし、次の行を巻き込んでいた
#   （2 行 渡して 2 行目が黙って消えた）。
printf '作る (点:甲 {本文: "x' > "$D/in2.txt"
printf 'A' | tr 'A' '\000' >> "$D/in2.txt"
printf 'y"})\n作る (点:乙)\n' >> "$D/in2.txt"
err=$("$YUI" "$D/t3.yui" < "$D/in2.txt" 2>&1 > /dev/null)
rc=$?
check "NUL が入っているときの終了コード" 1 $rc
has "NUL" "$err" "NUL だと言うこと"
has "1 行目" "$err" "NUL のある行が出ること"
left=$("$YUI" "$D/t3.yui" '一覧 (点:) 名前だけ' | tr '\n' ' ')
check "NUL の次の行が巻き込まれないこと" "(点:乙) 1 件 " "$left"

# 終わる で、そこから先は実行されない
printf '作る (点:前)\n終わる\n作る (点:後)\n' > "$D/in3.txt"
"$YUI" "$D/t4.yui" < "$D/in3.txt" > /dev/null 2>&1
check "終わる のあとの終了コード" 0 $?
left=$("$YUI" "$D/t4.yui" '一覧 (点:) 名前だけ' | tr '\n' ' ')
check "終わる のあとの行が実行されないこと" "(点:前) 1 件 " "$left"

# 日本語のファイル名
"$YUI" "$D/知識.yui" '作る (人:田中)' > /dev/null 2>&1
check "日本語のファイル名が開けること" 0 $?

# 空の入力
: > "$D/in4.txt"
"$YUI" "$D/t5.yui" < "$D/in4.txt" > /dev/null 2>&1
check "空の入力の終了コード" 0 $?

# ---------------------------------------------------------------- 締め

if [ "$ng" -eq 0 ]; then
    echo "ok  CLI: 確かめた $ok 件 — 失敗 0 件"
    exit 0
fi
echo "NG  CLI: 確かめた $ok 件 のうち $ng 件 失敗"
exit 1
