#!/bin/sh
#
# デーモン（yuid.c・段 1 と段 2）のテスト。
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

#
# ★素の `wait` を使わないこと。素の `wait` は**全部の背景の仕事**を待つので、
#   長く居座るデーモン（--idle 600）まで待ってしまい、テストが 10 分 止まります
#   （実際に踏みました）。待つなら PID を指定してください。
#
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
    "$YUID" "$F" --port "$i" --idle 600 > "$D/log" 2>&1 &
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

# ★段 2: 読むだけで立てたデーモンでも名乗れて、しかし書けないこと
# まず、読むだけで立てたデーモンでも名乗れて、しかし書けないこと
out=$("$SAY" "$PORT" '名乗る "読む人"' 2>&1)
has '"kind":"hello"' "$out" "読むだけのデーモンでも名乗れること"
has '"write":false' "$out" "読むだけなら write:false と言うこと"
out=$("$SAY" "$PORT" "$(printf '名乗る "読む人"\n作る (人:新顔3)')" 2>&1)
has "読むだけで開いています" "$out" "読むだけのデーモンでは、名乗っても書けないこと"


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

# ----------------------------- 返りの上限（--max-rows）と 読まない相手
#
# ★ここが段 1 で「入っていない」と書いていた 2 つです。
#   上限が無いと、1 回の 探す が 10 万行 返したときに溜まりだけでメモリを食う。
#   待って送る作りだと、読まないクライアント 1 つで全員 止まる。

# ★別のファイルを使います。同じファイルには 2 本 立てられません
#   （先に立っている 1 本が錠を持っているので。それが正しい振る舞いです）
CAPF="$D/cap.yui"
"$YUI" "$CAPF" '作る (人:甲)
作る (人:乙)
作る (人:丙)
作る (人:丁)
作る (人:戊)
作る (案件:ひとつ)' > /dev/null 2>&1

CAPPORT=0
i=39230
while [ "$i" -lt 39240 ]; do
    "$YUID" "$CAPF" --port "$i" --idle 600 --max-rows 3 > "$D/log3" 2>&1 &
    echo $! > "$D/pid3"
    j=0
    while [ "$j" -lt 50 ]; do
        if grep -q "で待っています" "$D/log3" 2> /dev/null; then break; fi
        if ! kill -0 "$(cat "$D/pid3")" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    if grep -q "で待っています" "$D/log3" 2> /dev/null; then
        CAPPORT=$i
        break
    fi
    kill "$(cat "$D/pid3")" 2> /dev/null
    i=$((i + 1))
done

if [ "$CAPPORT" -eq 0 ]; then
    ng=$((ng + 1))
    echo "NG   上限つきのデーモンが立ちませんでした: $(cat "$D/log3")"
else
    has "3 行まで" "$(cat "$D/log3")" "立つときに返りの上限を言うこと"

    out=$("$SAY" "$CAPPORT" '一覧 (人:) 名前だけ' 2>&1)
    has '"cut":true' "$out" "上限で打ち切ったら end に cut を付けること"
    has "打ち切りました" "$out" "打ち切ったと言うこと"
    has "新しい N" "$out" "どうすればよいかを言うこと"
    has '"kind":"end","rc":1' "$out" "打ち切ったら rc が 1 であること"
    check "上限の行数だけ返すこと" "3" "$(printf '%s\n' "$out" | grep -c '"kind":"node"')"

    # ★上限を超えなければ cut は付かない
    out=$("$SAY" "$CAPPORT" '一覧 (案件:) 名前だけ' 2>&1)
    hasnt '"cut":true' "$out" "上限を超えなければ cut を付けないこと"

    kill "$(cat "$D/pid3")" 2> /dev/null
    wait "$(cat "$D/pid3")" 2> /dev/null
fi

# ★読まない相手が居ても、他のクライアントが答えてもらえること
#   （--max-rows 0 で上限を外し、溜まりを作ってから別の接続で聞きます）
DEAFF="$D/deaf.yui"
# ★溜まりが 1 MB を超えるだけの返りを作ります。
#   点の数を増やすより、1 点の本文を長くするほうが速いので、そうしています
#   （1 万点 作ると、この段だけで 1 分 以上 かかりました）。
awk 'BEGIN{
    t = ""; for (j = 0; j < 300; j++) t = t "あ"
    for (i = 1; i <= 10000; i++) printf "作る (人:ふやす%d {本文: \"%s\"})\n", i, t
}' > "$D/big.txt"
"$YUI" "$DEAFF" < "$D/big.txt" > /dev/null 2>&1
"$YUI" "$DEAFF" '作る (案件:ひとつ)' > /dev/null 2>&1

DEAFPORT=0
i=39241
while [ "$i" -lt 39250 ]; do
    "$YUID" "$DEAFF" --port "$i" --idle 600 --max-rows 0 > "$D/log4" 2>&1 &
    echo $! > "$D/pid4"
    j=0
    while [ "$j" -lt 50 ]; do
        if grep -q "で待っています" "$D/log4" 2> /dev/null; then break; fi
        if ! kill -0 "$(cat "$D/pid4")" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    if grep -q "で待っています" "$D/log4" 2> /dev/null; then
        DEAFPORT=$i
        break
    fi
    kill "$(cat "$D/pid4")" 2> /dev/null
    i=$((i + 1))
done

if [ "$DEAFPORT" -eq 0 ]; then
    ng=$((ng + 1))
    echo "NG   上限なしのデーモンが立ちませんでした: $(cat "$D/log4")"
else
    # --- その 1: 溜まる（けれど切るほどではない）相手が居ても、他に答えること
    #     1200 件ぶん ≒ 1.1 MB。溜まりの関所（1 MB）は超え、切る線（8 MB）は超えない
    # ★居座る時間（15 秒）は、聞く側の待ち時間（yuid_say の 10 秒）より長くします。
    #   短いと、待って送る作りに戻しても「相手が去ったあとに答える」で間に合って
    #   しまい、検査に検出力がありません（実測で分かりました）。
    #   通るときは 1 秒 ほどで終わるので、長くしても遅くなりません。
    "$SAY" "$DEAFPORT" --deaf 15 '一覧 (人:) 新しい 1200' > /dev/null 2>&1 &
    DEAFPID=$!
    sleep 1
    out=$("$SAY" "$DEAFPORT" '一覧 (案件:) 名前だけ' 2>&1)
    check "読まない相手が居ても別の接続に答えること（終了コード）" "0" "$?"
    has '"kind":"end","rc":0' "$out" "読まない相手が居ても締めの行が返ること"
    kill "$DEAFPID" 2> /dev/null
    wait "$DEAFPID" 2> /dev/null

    # --- その 2: 溜まりが切る線を超えたら、その接続を切ること
    #
    # ★切る線を小さくしたデーモンを別に立てます。既定の 8 MB のままだと、
    #   **OS の緩衝がそれだけ飲み込んでしまう土台**があり（macOS の CI で実測）、
    #   溜まりが線を超えず、検査が噛みません。
    #   OS の緩衝の大きさに依存しない形にするため、線のほうを小さくします。
    CUTF="$D/cut.yui"
    cp "$DEAFF" "$CUTF"
    CUTPORT=0
    i=39251
    while [ "$i" -lt 39260 ]; do
        "$YUID" "$CUTF" --port "$i" --idle 600 --max-rows 0 --max-out 65536 \
            > "$D/log5" 2>&1 &
        echo $! > "$D/pid5"
        j=0
        while [ "$j" -lt 50 ]; do
            if grep -q "で待っています" "$D/log5" 2> /dev/null; then break; fi
            if ! kill -0 "$(cat "$D/pid5")" 2> /dev/null; then break; fi
            j=$((j + 1))
            sleep 0.1
        done
        if grep -q "で待っています" "$D/log5" 2> /dev/null; then
            CUTPORT=$i
            break
        fi
        kill "$(cat "$D/pid5")" 2> /dev/null
        i=$((i + 1))
    done
    if [ "$CUTPORT" -eq 0 ]; then
        ng=$((ng + 1))
        echo "NG   切る線を小さくしたデーモンが立ちませんでした: $(cat "$D/log5")"
    else
    DEAFPORT=$CUTPORT
    "$SAY" "$DEAFPORT" --deaf 15 '一覧 ()' > /dev/null 2>&1 &
    DEAFPID2=$!
    j=0
    while [ "$j" -lt 100 ]; do
        if grep -q "受け取らない接続を切りました" "$D/log5" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    has "受け取らない接続を切りました" "$(cat "$D/log5")" \
        "溜まりが切る線を超えたら、その接続を切ること"
    # ★切ったあとも、デーモンは生きていて他に答えること
    out=$("$SAY" "$CUTPORT" '一覧 (案件:) 名前だけ' 2>&1)
    has '"kind":"end","rc":0' "$out" "接続を切ったあともデーモンが生きていること"
    kill "$DEAFPID2" 2> /dev/null
    wait "$DEAFPID2" 2> /dev/null
    kill "$(cat "$D/pid5")" 2> /dev/null
    wait "$(cat "$D/pid5")" 2> /dev/null
    fi

    kill "$(cat "$D/pid4")" 2> /dev/null
    wait "$(cat "$D/pid4")" 2> /dev/null
fi

# ------------------------------------------------ 2 本 目は立たないこと
#
# ★終了コードは 3（「ほかが先に持っている」）です。口でも錠でも同じ 3 にしています。
#   見張る仕組みからは、どちらも「あとで建て直せばよい」で同じなので。
#
# ★--idle 1 を付けます。立たないことを見る段ですが、万一 立ってしまったときに
#   付けていないと、**永久に走り続けてテストが止まります**（実際に踏みました。
#   前の段が長引いて 1 本目が空き時間で終わっており、2 本目が立ってしまった）。
#   前提が崩れたときは、止まるのではなく落ちてほしい。
out=$("$YUID" "$F" --port "$PORT" --idle 1 2>&1)
rc=$?
check "2 本 目のデーモンは立たないこと（終了コード）" "3" "$rc"
has "使えません" "$out" "2 本 目には口が使えないと言うこと"
hasnt "で待っています" "$out" "2 本 目は待ち受けを始めないこと"

# ★別のポートでも、同じファイルなら錠で止まること（同じ理由で --idle 1）
out=$("$YUID" "$F" --port $((PORT + 100)) --idle 1 2>&1)
rc=$?
check "同じファイルなら別のポートでも立たないこと（終了コード）" "3" "$rc"
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

# ------------------------------------------------------ 段 2: 書けるようにする
#
# ★ここで見ているのは 3 つです。
#   (a) 名乗らない接続は読むだけのまま（--write を付けても）
#   (b) 書き込みが通ると「誰が・いつ」が押される
#   (c) ★時刻は**線の属性**に入る（点の属性に置くと、同じ機が次に書いたとき
#       上書きされて消えます。設計の文書の例が誤っていたので、対照を取ります）

# 書けるデーモンを別に立てます
FW="$D/w.yui"
"$YUI" "$FW" '作る (決定:ある決めごと)' > /dev/null 2>&1 ||
    { echo "NG   下ごしらえ（書ける方のファイル）に失敗しました"; exit 2; }
WPID="$D/wpid"
trap 'for f in "$PIDFILE" "$WPID"; do if [ -f "$f" ]; then kill "$(cat "$f")" 2> /dev/null; fi; done; rm -rf "$D"' EXIT
WPORT=0
i=39240
while [ "$i" -lt 39260 ]; do
    "$YUID" "$FW" --write --port "$i" --idle 600 > "$D/wlog" 2>&1 &
    echo $! > "$WPID"
    j=0
    while [ "$j" -lt 50 ]; do
        if grep -q "で待っています" "$D/wlog" 2> /dev/null; then break; fi
        if ! kill -0 "$(cat "$WPID")" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    if grep -q "で待っています" "$D/wlog" 2> /dev/null; then
        WPORT=$i
        break
    fi
    kill "$(cat "$WPID")" 2> /dev/null
    : > "$WPID"
    i=$((i + 1))
done
[ "$WPORT" -ne 0 ] || { echo "NG   書けるデーモンが立ちませんでした: $(cat "$D/wlog")"; exit 1; }
has "名乗れば書けます" "$(cat "$D/wlog")" "書けるときは、立つときにそう言うこと"

# (a) 名乗らずに書く
out=$("$SAY" "$WPORT" '作る (人:勝手に)' 2>&1)
has "読むだけで開いています" "$out" "名乗らない接続は書けないこと"
has '"rc":1' "$out" "そのとき rc=1 になること"
out=$("$SAY" "$WPORT" '一覧 (人:)' 2>&1)
hasnt '"name":"勝手に"' "$out" "断られた書き込みが入っていないこと"

# (b) 名乗ってから書く（1 本の接続で続けて送ります）
out=$("$SAY" "$WPORT" "$(printf '名乗る "書き手 たろう"\n作る (人:田中 {役職: "部長"})')" 2>&1)
has '"write":true' "$out" "書けるデーモンでは write:true と言うこと"
has '"stamp":true' "$out" "既定では帰属を押すと言うこと"
has "作りました" "$out" "名乗った接続は書けること"
has '"rc":0' "$out" "書けたら rc=0 になること"

out=$("$SAY" "$WPORT" '辿る (機:"書き手 たろう")<-[書いた]-(?何)' 2>&1)
has '"name":"田中"' "$out" "押した帰属から、書いた点を辿れること"

# (c) ★時刻は線の属性に入っていること
has '"attrs":{"時"' "$out" "時刻が**線**の属性に入っていること"
out=$("$SAY" "$WPORT" '一覧 (機:)' 2>&1)
has '"name":"書き手 たろう"' "$out" "名乗りが点になっていること"
hasnt '"時"' "$out" "★時刻が**機の点**の属性に入っていないこと（入れると上書きで消えます）"

# 読む文では押さないこと
# ★読む相手は「まだ押されていない点」にします。既に押されている点を読むと、
#   押してしまっても件数が変わらないので、検査が噛みません（実際に噛みませんでした）。
out=$("$SAY" "$WPORT" "$(printf '名乗る "書き手 たろう"\n一覧 (決定:)')" 2>&1)
has '"rc":0' "$out" "名乗った接続でも、読む文は通ること"
has '"name":"ある決めごと"' "$out" "読む文が中身を返すこと"
out=$("$SAY" "$WPORT" '辿る (機:"書き手 たろう")<-[書いた]-(?何)' 2>&1)
has '"count":1' "$out" "★読む文では帰属を押さないこと（増えていないこと）"

# 印なし
out=$("$SAY" "$WPORT" "$(printf '名乗る "取り込み" 印なし\n作る (荷:ひとつ)')" 2>&1)
has '"stamp":false' "$out" "「印なし」と名乗れば、押さないと言うこと"
has "作りました" "$out" "「印なし」でも書けること"
out=$("$SAY" "$WPORT" '辿る (機:"取り込み")<-[書いた]-(?何)' 2>&1)
hasnt '"name":"ひとつ"' "$out" "★「印なし」では押されていないこと"

# 名乗りの形
out=$("$SAY" "$WPORT" '名乗る' 2>&1)
has "名前がありません" "$out" "名前なしの名乗りを断ること"
out=$("$SAY" "$WPORT" "$(printf '名乗る "改行\001いり"')" 2>&1)
has "0x20 より小さい文字" "$out" "名乗りに制御文字を使わせないこと"

# ★往復できない名前には押さない（押すと別の点に付き、結ぶ が無い点を作ります）
out=$("$SAY" "$WPORT" "$(printf '名乗る "書き手 たろう"\n作る (生:"制御\001あり")')" 2>&1)
has "作りました" "$out" "制御文字の入った名前でも、書き込みそのものは通ること"
has "帰属を押せませんでした" "$out" "★押せないときは、黙らずに理由を言うこと"
has '"rc":1' "$out" "★押せなかったことが rc=1 で分かること"
out=$("$SAY" "$WPORT" '一覧 (機:)' 2>&1)
hasnt '"label":"生"' "$out" "★押せなかったとき、別の点を作っていないこと"

# ★押しきれなかったときに黙らないこと。
#   いまの言葉では上限（作る=1・結ぶ=2）を超えないので、--max-stamp 1 で立て直して見ます。
#   ここを見ないと、この道は一度も通らないまま残ります。
kill "$(cat "$WPID")" 2> /dev/null
: > "$WPID"
sleep 0.3
FS="$D/s.yui"
"$YUI" "$FS" '作る (決定:ある決めごと)' > /dev/null 2>&1
SPORT=0
i=39260
while [ "$i" -lt 39280 ]; do
    "$YUID" "$FS" --write --max-stamp 1 --port "$i" --idle 600 > "$D/slog" 2>&1 &
    echo $! > "$WPID"
    j=0
    while [ "$j" -lt 50 ]; do
        if grep -q "で待っています" "$D/slog" 2> /dev/null; then break; fi
        if ! kill -0 "$(cat "$WPID")" 2> /dev/null; then break; fi
        j=$((j + 1))
        sleep 0.1
    done
    if grep -q "で待っています" "$D/slog" 2> /dev/null; then
        SPORT=$i
        break
    fi
    kill "$(cat "$WPID")" 2> /dev/null
    : > "$WPID"
    i=$((i + 1))
done
[ "$SPORT" -ne 0 ] || { echo "NG   --max-stamp のデーモンが立ちませんでした: $(cat "$D/slog")"; exit 1; }
# 結ぶ は両端の 2 つに押すので、上限 1 では押しきれません
out=$("$SAY" "$SPORT" "$(printf '名乗る "書き手 じろう"\n結ぶ (案件:A)-[担当]->(人:B)')" 2>&1)
has "作りました" "$out" "上限を超えても、書き込みそのものは通ること"
has "押しきれませんでした" "$out" "★押しきれなかったときに黙らないこと"
has '"rc":1' "$out" "★押しきれなかったことが rc=1 で分かること"

kill "$(cat "$WPID")" 2> /dev/null
: > "$WPID"

# ---------------------------------------------------------------- 締め

if [ "$ng" -eq 0 ]; then
    echo "ok  デーモン: 確かめた $ok 件 — 失敗 0 件"
    exit 0
fi
echo "NG  デーモン: 確かめた $ok 件 のうち $ng 件 失敗"
exit 1
