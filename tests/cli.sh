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
trap 'chmod -R 700 "$D" 2> /dev/null; rm -rf "$D"' EXIT
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
# 文字列が含まれていないこと
hasnt() {
    case "$2" in
    *"$1"*)
        ng=$((ng + 1))
        echo "NG   $3"
        echo "     「$1」が出ています。出たのは: $2"
        ;;
    *) ok=$((ok + 1)) ;;
    esac
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

# ---------------------------------------------------------------- --json

# ★結果は標準出力・お知らせと失敗は標準エラー、と分けてあります。
#   混ぜて取ると、お知らせの行が結果の 1 行目に見えます（実測で踏みました）。
"$YUI" --json "$D/j1.yui" '作る (人:田中 {役職: "部長"})' > "$D/j1.out" 2> "$D/j1.err"
check "--json の終了コード" 0 $?
first=$(head -1 "$D/j1.out")
has '"kind":"node"' "$first" "--json で点が kind=node で出ること"
has '"label":"人"' "$first" "--json でラベルが鍵で取れること"
last=$(tail -1 "$D/j1.out")
has '"kind":"summary"' "$last" "--json で締めが kind=summary で出ること"

# 1 行 1 つの JSON になっていること（{ で始まり } で終わる行だけ）
bad=$(grep -c -v '^{.*}$' "$D/j1.out" 2>/dev/null || true)
check "--json の出力が 1 行 1 つの JSON であること" 0 "$bad"

# 失敗も JSON で、標準エラーへ
err=$("$YUI" --json "$D/j1.yui" 'これは誤り' 2>&1 > /dev/null)
has '"kind":"error"' "$err" "--json で失敗が kind=error で出ること"

# 流し込みのときは、何行目かも JSON に入る
printf '作る (点:一)
これは誤り
' > "$D/j2in.txt"
err=$("$YUI" --json "$D/j2.yui" < "$D/j2in.txt" 2>&1 > /dev/null)
has '"line":2' "$err" "--json で何行目かが入ること"

# --json だけ渡したら使い方
"$YUI" --json > /dev/null 2>&1
check "--json だけ渡したときの終了コード" 2 $?

# ------------------------------------------- 無いパスを開いたときのお知らせ

# ★結は無いファイルを作って開きます。便利ですが、置き場を取り違えたときに
#   「0 件」と同じ見た目になります。実際に踏んだ人がいるので、知らせます。
err=$("$YUI" "$D/あたらしい.yui" '一覧 ()' 2>&1 > /dev/null)
has "新しいファイルとして始めました" "$err" "無いパスを開いたら知らせること"
has "あたらしい.yui" "$err" "お知らせにパスを出すこと"

# 2 回目は言わない（中身があるので）
err=$("$YUI" "$D/あたらしい.yui" '一覧 ()' 2>&1 > /dev/null)
check "2 回目は言わないこと" "" "$err"

# ★結果（標準出力）には混ぜない
out=$("$YUI" "$D/もうひとつ.yui" '一覧 ()' 2> /dev/null)
check "お知らせを結果に混ぜないこと" "0 件" "$out"

# --json のときは、お知らせも JSON で
err=$("$YUI" --json "$D/みっつめ.yui" '一覧 ()' 2>&1 > /dev/null)
has '"kind":"notice"' "$err" "--json ではお知らせも JSON で出ること"
bad=$(printf '%s
' "$err" | grep -c -v '^{.*}$' 2>/dev/null || true)
check "--json のお知らせが 1 行 1 つの JSON であること" 0 "$bad"

# ------------------------------------------------- 詰め直す の失敗の道

# ★README に「途中で失敗したら、元のファイルはそのままです」と書いてあるので、
#   実際に失敗させて確かめます。本物の失敗（置き場を書けなくする）で起こします。
# ★root で走ると権限のビットが効かないので、先に「本当に書けないか」を
#   確かめます（効いていなければ、この節は飛ばして「見ていません」と出します）。
mkdir -p "$D/ro"
"$YUI" "$D/ro/x.yui" '作る (人:田中)' > /dev/null 2>&1
"$YUI" "$D/ro/x.yui" '作る (人:佐藤)' > /dev/null 2>&1
"$YUI" "$D/ro/x.yui" '消す (人:佐藤)' > /dev/null 2>&1
before=$(md5sum < "$D/ro/x.yui")
chmod 500 "$D/ro"
# ★`:` は特殊組み込みなので、リダイレクトが失敗するとシェルごと終わります
#   （POSIX の規定。dash で実測）。外のコマンドを部分シェルで使います。
if (touch "$D/ro/書けるか") 2> /dev/null; then
    rm -f "$D/ro/書けるか"
    chmod 700 "$D/ro"
    echo "-- 見ていません: 詰め直す の失敗の道（置き場を読み取り専用にしても書けてしまうので、この環境では起こせません。root で走っていませんか）"
else
    err=$("$YUI" "$D/ro/x.yui" '詰め直す' 2>&1 > /dev/null)
    rc=$?
    check "作業用のファイルを作れないときの終了コード" 1 $rc
    has "詰め直し用のファイルを作れません" "$err" "作れない理由を言うこと"
    chmod 700 "$D/ro"
    # ★元のファイルがそのままであること（これが README の約束）
    after=$(md5sum < "$D/ro/x.yui")
    check "失敗しても元のファイルが変わらないこと" "$before" "$after"

    # ★同じプロセスの中で、失敗のあとも使えること（錠を手放していない＝fd が生きている）。
    #   ★別のプロセスで確かめてはいけません。次のプロセスは開き直すので、
    #     立て直しを壊しても通ってしまいます（陽性対照で実測しました）。
    #     なので 詰め直す と 一覧 を 1 回の流し込みで続けて打ちます。
    chmod 500 "$D/ro"
    printf '詰め直す
一覧 (人:) 名前だけ
' > "$D/rin.txt"
    left=$("$YUI" "$D/ro/x.yui" < "$D/rin.txt" 2> /dev/null | tr '
' ' ')
    chmod 700 "$D/ro"
    check "同じプロセスで、失敗のあとも読めること" "(人:田中) 1 件 " "$left"
    # 書けること（失敗のあとに同じプロセスで書く）
    chmod 500 "$D/ro"
    printf '詰め直す
作る (人:鈴木)
' > "$D/win.txt"
    "$YUI" "$D/ro/x.yui" < "$D/win.txt" > /dev/null 2>&1
    chmod 700 "$D/ro"
    left=$("$YUI" "$D/ro/x.yui" '一覧 (人:) 名前だけ' | tr '
' ' ')
    check "同じプロセスで、失敗のあとも書けること" "(人:田中) (人:鈴木) 2 件 " "$left"
    # ★作業用のファイルを残さないこと
    if [ -e "$D/ro/x.yui.詰" ]; then
        ng=$((ng + 1))
        echo "NG   失敗したとき作業用の .詰 を残さないこと"
        echo "     $D/ro/x.yui.詰 が残っています"
    else
        ok=$((ok + 1))
    fi
fi

# ------------------------------------------------- 端末として開いたとき

# ★isatty(0) が真のときだけ通る道です（呼び出しの合図を出す・行番号を出さない）。
#   ふつうの流し込みでは通らないので、疑似端末を割り当てて測ります。
#   script の書き方は GNU と BSD（macOS）で違うので、両方 試します。
PTY=""
if (script -q -c "echo ok" /dev/null) > /dev/null 2>&1; then
    PTY=gnu
elif (script -q /dev/null echo ok) > /dev/null 2>&1; then
    PTY=bsd
fi
run_pty() { # $1=ファイル、標準入力から命令を流す
    if [ "$PTY" = gnu ]; then
        script -q -c "$YUI $1" /dev/null 2>&1
    else
        script -q /dev/null "$YUI" "$1" 2>&1
    fi
}
if [ -z "$PTY" ]; then
    echo "-- 見ていません: 端末として開いたときの道（script が無いので疑似端末を作れません）"
else
    out=$(printf '一覧 ()
これは誤り
終わる
' | run_pty "$D/pty.yui")
    has "yui> " "$out" "端末では呼び出しの合図を出すこと"
    has "手引き" "$out" "端末では使い方の 1 行を出すこと"
    # ★端末では行番号を出しません（「1 行目」と言われても手がかりにならないので）
    hasnt "行目" "$out" "端末では行番号を出さないこと"
    has "0 件" "$out" "端末でも結果を出すこと"
    has "知らない言葉です" "$out" "端末でも失敗の理由を出すこと"
fi

# ---------------------------------------------------------------- 締め

if [ "$ng" -eq 0 ]; then
    echo "ok  CLI: 確かめた $ok 件 — 失敗 0 件"
    exit 0
fi
echo "NG  CLI: 確かめた $ok 件 のうち $ng 件 失敗"
exit 1
