#!/bin/sh
#
# デーモン（yuid.c・段 1）のテスト。
#   sh tests/daemon.sh ./yuid ./build/yuid_say ./yui
#
# ★Windows では動きません（sh が要ります）。ただし話しかける道具
#   （tests/yuid_say.c）は土台を問わないので、CI の Windows の段は
#   同じ道具を使います。nc を使わないのはそのためです（フラグが土台で違い、
#   「手元では通るが別の土台で通らない」を作ります。tests/README.md）。

set -u
YUID=${1:-./yuid}
SAY=${2:-./build/yuid_say}
YUI=${3:-./yui}
for f in "$YUID" "$SAY" "$YUI"; do
    [ -x "$f" ] || { echo "NG   $f がありません。先に make してください"; exit 2; }
done

D=$(mktemp -d) || exit 2
PIDFILE="$D/pid"
trap 'if [ -f "$PIDFILE" ]; then kill "$(cat "$PIDFILE")" 2> /dev/null; fi; rm -rf "$D"' EXIT
ok=0
ng=0

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

F="$D/t.yui"
"$YUI" "$F" '作る (人:田中 {役職: "部長"})
結ぶ (人:田中)-[担当]->(案件:結)' > /dev/null 2>&1 ||
    { echo "NG   下ごしらえ（ファイルを作る）に失敗しました"; exit 2; }

# ---------------------------------------------------------------- 立てる
# ★空いているポートを探します。固定にすると、他のものが使っていたときに
#   「デーモンの不具合」に見えてしまいます。
PORT=0
i=39210
while [ "$i" -lt 39230 ]; do
    "$YUID" "$F" --port "$i" --idle 60 > "$D/log" 2>&1 &
    echo $! > "$PIDFILE"
    # 立ち上がりを待ちます（待ち受けの 1 行が出るまで）
    j=0
    while [ "$j" -lt 50 ]; do
        if grep -q "で待っています" "$D/log" 2> /dev/null; then break; fi
        if ! kill -0 "$(cat "$PIDFILE")" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    if grep -q "で待っています" "$D/log" 2> /dev/null; then
        PORT=$i
        break
    fi
    kill "$(cat "$PIDFILE")" 2> /dev/null
    : > "$PIDFILE"
    i=$((i + 1))
done
[ "$PORT" -ne 0 ] || { echo "NG   デーモンが立ちませんでした: $(cat "$D/log")"; exit 1; }

has "読むだけ" "$(cat "$D/log")" "立つときに「読むだけ」と言うこと"
[ -f "$F.錠" ] && ok=$((ok + 1)) || { ng=$((ng + 1)); echo "NG   立つと錠を作ること"; }
has "yuid" "$(cat "$F.錠")" "錠に自分の名乗りを書くこと"

# ---------------------------------------------------------------- 読む
out=$("$SAY" "$PORT" '一覧 ()' 2>&1)
has '"kind":"node"' "$out" "点を JSON で返すこと"
has '"name":"田中"' "$out" "名前を返すこと"
has '"役職":"部長"' "$out" "属性を返すこと（値は日本語のまま）"
has '"kind":"summary"' "$out" "締めくくりの行を返すこと"
has '"kind":"end","rc":0' "$out" "成功なら end の rc が 0 であること"

out=$("$SAY" "$PORT" '見る (人:田中)' 2>&1)
has '"kind":"link"' "$out" "つながりを返すこと"

# ---------------------------------------------- 読むだけ（書き込みを断る）
for q in '作る (人:新顔)' '結ぶ (人:田中)-[印]->(人:新顔)' '消す (人:田中)' '詰め直す'; do
    out=$("$SAY" "$PORT" "$q" 2>&1)
    has "読むだけで開いています" "$out" "書き込みを断ること: $q"
    has '"kind":"end","rc":1' "$out" "断ったら end の rc が 1 であること: $q"
done

# ★断ったあとも、ファイルが変わっていないこと
before=$(wc -c < "$F")
"$SAY" "$PORT" '作る (人:新顔2)' > /dev/null 2>&1
check "断ったときファイルが変わらないこと" "$before" "$(wc -c < "$F")"

# ------------------------------------------------- 1 行 間違っても続ける
out=$("$SAY" "$PORT" 'そんな言葉
一覧 (人:)' 2>&1)
has "知らない言葉です" "$out" "誤った行の理由を返すこと"
has '"line":1' "$out" "何行目かを返すこと"
has '"name":"田中"' "$out" "1 行目が誤っていても 2 行目を実行すること"
has '"kind":"end","rc":1' "$out" "1 行でも失敗したら rc が 1 であること"

# -------------------------------------------- 同じ接続で何度も（持続接続）
out=$("$SAY" "$PORT" '一覧 (人:)' '一覧 (案件:)' '手引き' 2>&1)
has '"name":"田中"' "$out" "持続接続の 1 回目が返ること"
has '"name":"結"' "$out" "持続接続の 2 回目が返ること"
# ★手引き は締めの行（summary）を出しません。だから end が要ります
has "点の書き方" "$out" "手引き の中身を返すこと"
hasnt '"kind":"summary"' "$(printf '%s\n' "$out" | sed -n '/点の書き方/,$p')" \
    "手引き は締めくくりの行を出さないこと（end が必要な理由）"
check "end の行が 3 つ 返ること" "3" "$(printf '%s\n' "$out" | grep -c '"kind":"end"')"

# ---------------------------------------------------------------- 終わる
out=$("$SAY" "$PORT" '終わる' 2>&1)
has '"bye":true' "$out" "終わる なら end に bye を付けること"
# ★終わる は接続を切るだけ。デーモンは生きています
out=$("$SAY" "$PORT" '一覧 (人:)' 2>&1)
has '"kind":"end","rc":0' "$out" "終わる のあとも繋ぎ直せること（デーモンは止まらない）"

# ------------------------------------------------------------ 形の誤り
printf 'いろは\n一覧 ()' > "$D/badlen"
out=$("$SAY" "$PORT" --raw "$D/badlen" 2>&1)
has "長さが数字ではありません" "$out" "長さの行が数字でなければ断ること"

printf '99999999\n一覧 ()' > "$D/biglen"
out=$("$SAY" "$PORT" --raw "$D/biglen" 2>&1)
has "大きすぎます" "$out" "長さが大きすぎれば断ること"

# ★NUL は sh の引数に入れられないので、ファイル経由で渡します
printf '\345\205\250\000\344\275\223' > "$D/nul"
out=$("$SAY" "$PORT" --file "$D/nul" 2>&1)
has "NUL" "$out" "NUL が入っていたら言うこと"
has "バイト目" "$out" "NUL の位置を言うこと"

# ------------------------------------------------ 2 本 目は立たないこと
out=$("$YUID" "$F" --port "$PORT" 2>&1)
rc=$?
check "2 本 目のデーモンは立たないこと（終了コード）" "1" "$rc"
has "使えません" "$out" "2 本 目には口が使えないと言うこと"
hasnt "で待っています" "$out" "2 本 目は待ち受けを始めないこと"

# ★別のポートでも、同じファイルなら錠で止まること
out=$("$YUID" "$F" --port $((PORT + 100)) 2>&1)
rc=$?
check "同じファイルなら別のポートでも立たないこと（終了コード）" "1" "$rc"
has "錠を持っています" "$out" "同じファイルなら錠で止まると言うこと"

# -------------------------------------------------- 止めると錠が外れる
kill "$(cat "$PIDFILE")" 2> /dev/null
j=0
while [ "$j" -lt 50 ]; do
    kill -0 "$(cat "$PIDFILE")" 2> /dev/null || break
    j=$((j + 1))
    sleep 0.1
done
: > "$PIDFILE"
if [ -f "$F.錠" ]; then
    ng=$((ng + 1))
    echo "NG   止めると錠を外すこと"
    echo "     錠が残っています: $(cat "$F.錠")"
else
    ok=$((ok + 1))
fi
has "錠を外しました" "$(cat "$D/log")" "止まるときに錠を外したと言うこと"

# ---------------------------------------------------- 空き時間で終わる
"$YUID" "$F" --port "$PORT" --idle 1 > "$D/log2" 2>&1 &
echo $! > "$PIDFILE"
j=0
while [ "$j" -lt 80 ]; do
    kill -0 "$(cat "$PIDFILE")" 2> /dev/null || break
    j=$((j + 1))
    sleep 0.1
done
if kill -0 "$(cat "$PIDFILE")" 2> /dev/null; then
    ng=$((ng + 1))
    echo "NG   空き時間が過ぎたら終わること（まだ生きています）"
    kill "$(cat "$PIDFILE")" 2> /dev/null
else
    ok=$((ok + 1))
    has "空いたので終わります" "$(cat "$D/log2")" "空き時間で終わったと言うこと"
fi
: > "$PIDFILE"

# ---------------------------------------------------------------- 締め

if [ "$ng" -eq 0 ]; then
    echo "ok  デーモン: 確かめた $ok 件 — 失敗 0 件"
    exit 0
fi
echo "NG  デーモン: 確かめた $ok 件 のうち $ng 件 失敗"
exit 1
