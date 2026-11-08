/*
 * 結（yui）— 日本語特化の組み込みグラフ DB
 * 自分用の緩いソフトです。堅牢さは求めていません。詳しくは README.md。
 *
 * 使い方はこれだけです。
 *
 *   yui *db;
 *   if (yui_open("知識.yui", &db) != YUI_OK) { 表示(yui_errmsg(db)); }
 *   yui_exec(db, "結ぶ (案件:結の開発)-[担当]->(人:田中)", 行ごとの関数, 文脈);
 *   yui_close(db);
 *
 * 1 つの yui * を複数のスレッドから同時に使わないでください。
 * メモリが確保できないときはプロセスごと止まります（abort）。
 */
#ifndef YUI_H
#define YUI_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#define YUI_VERSION "0.1.0"

/* 戻り値 */
#define YUI_OK    0 /* 成功 */
#define YUI_ERROR 1 /* 失敗。理由は yui_errmsg() */
#define YUI_END   2 /* 「終わる」を実行した */

/* 結果の種類（yui_cb の第 2 引数） */
#define YUI_ROW     0 /* 結果の 1 行 */
#define YUI_SUMMARY 1 /* 「3 件」のような締めの 1 行 */

typedef struct yui yui;

/*
 * 結果を 1 行ずつ受け取る関数。text は呼び出しの間だけ有効です。
 * 0 以外を返すと、そこで打ち切ります（この場合も戻り値は YUI_OK です）。
 *
 * ★打ち切るのは「その文」ではなく「yui_exec に渡した残り全部」です。
 *   複数の文をまとめて渡しているときは、そこから先の文が実行されません。
 *   書き込みの文が混ざっていると、書かれないまま YUI_OK で返ります。
 *   「返す行を N 件までにする」つもりでここで止めると、後ろの文が静かに落ちます。
 *   数を絞りたいだけなら、0 を返し続けて受け取る側で捨ててください。
 */
typedef int (*yui_cb)(void *ctx, int kind, const char *text);

/*
 * ファイルを開きます。無ければ作ります。
 * 失敗しても *db には値が入るので、yui_errmsg() で理由を読んでから
 * yui_close() してください。
 */
int yui_open(const char *path, yui **db);

/* yui_open_locked の旗 */
#define YUI_LOCK_BREAK 1 /* 塞がれていたら壊して取る */

/*
 * yui_open と同じですが、`<パス>.錠` も取ります。
 *
 * ★何のためか: flock（POSIX）と LockFileEx（Windows）は互いを排除せず、
 *   wasm の flock は何もしません。**同じファイルを別の土台から開くと、
 *   どちらも「自分が持ち主だ」と思ったまま書けます。**この関数は
 *   `<パス>.錠` を O_EXCL で作ることで、土台をまたいで排除します。
 *
 * ★既定が off なのは、錠を既定にすると「開けないことがある」が増えるからです。
 *   yui_open の振る舞いは変わりません。土台をまたいで使うときだけこちらを呼んでください。
 *
 * name は名乗りです（例 "my-app 1.2.3"）。機械名と pid だけでは
 * 同じ機械の中の別のプログラムと区別できないので、**必ず渡してください**。
 * 制御文字は空白に置き換えます。NULL や空なら "-" になります。
 *
 * flags に YUI_LOCK_BREAK を入れると、塞がれていたときに壊して取ります。
 * 入れなければ、持ち主を名乗って YUI_ERROR で返ります（yui_errmsg() に錠の中身が入ります）。
 *
 * ★持ち主が落ちても錠は外れません（flock は外れます）。
 *   「壊してよいか」は結には判断できません——pid の生死は同じ土台の中でしか見られず、
 *   「同じ機械か」も判定できません。**判断は呼び出した側がしてください。**
 *   先に yui_lock_owner() で持ち主を読み、自分が落ちた残りだと分かったときだけ
 *   YUI_LOCK_BREAK を付けてください。
 *
 * ★錠は yui_close() で外れます。異常終了したときは残ります。
 * ★`詰め直す` の入れ替えの一瞬も、この錠は外れません（flock は外れます）。
 *   ただし守れるのは、**同じファイルを使う全員がこの関数で開いているとき**だけです。
 *
 * 失敗しても *db には値が入るので、yui_errmsg() を読んでから yui_close() してください。
 */
int yui_open_locked(const char *path, const char *name, int flags, yui **db);

/*
 * `<パス>.錠` の中身を、ファイルを開かずに読みます。
 * 錠が無ければ buf は空文字列になり YUI_OK を返します（＝「無い」は失敗ではありません）。
 * 読めなかったときだけ YUI_ERROR です。
 *
 * 中身は 1 行 1 項目で、キーは ASCII です。
 *   yui-lock 1
 *   name=...          呼び出し側が渡した名乗り
 *   machine=...       機械の名前（wasm では意味のある値になりません）
 *   pid=...           ★その機械の中でしか意味がありません
 *   since=...         取った時刻（エポック秒）。★機械をまたいで比べるならこちらを使ってください
 *   since-local=...   取った時刻（持ち主の地方時）
 *   tag=...           結が内部で使う印
 *
 * ★項目は増えることがあります。知らないキーは読み飛ばしてください。
 */
int yui_lock_owner(const char *path, char *buf, size_t bufn);

/*
 * 問い合わせを実行します。cb は NULL でも構いません。
 * 改行か「。」で区切って複数の文を渡せます。
 * 途中の文で失敗すると、そこで止まります。前の文の結果は残ります。
 *
 * ★query に NUL（文字コード 0）を入れないでください。C の文字列として受け取るので、
 *   NUL があるとそこが終わりになり、そこから先は渡りません。
 *   文字列の途中に NUL があると「文字列が閉じていません」という、
 *   理由の分からないエラーになります。
 *   バイト数で読んでいる側にしか分からないので、渡す前に数えてください
 *   （yui_cli.c が実際にそうしています）。
 */
int yui_exec(yui *db, const char *query, yui_cb cb, void *ctx);

/*
 * yui_exec と同じですが、1 行を 1 つの JSON で返します（JSON Lines）。
 * 行の数と順は yui_exec と同じです。
 *
 * ★鍵は ASCII です。値（ラベル・名前・属性）は日本語のまま UTF-8 で入ります。
 * ★行の種類は "kind" で分かります。
 *     node     {"kind":"node","label":…,"name":…,"attrs":{…}}
 *     edge     {"kind":"edge","type":…,"from":{…},"to":{…},"attrs":{…}}
 *     path     {"kind":"path","nodes":[…],"edges":[{"type":…,"forward":真偽,"attrs":{…}}]}
 *              （辿る の道。nodes は edges より 1 つ多い）
 *     link     {"kind":"link","dir":"out"|"in","type":…,"attrs":{…},"other":{…}}
 *              （見る のつながり）
 *     text     {"kind":"text","text":…}（手引き など、人へのそのままの 1 行）
 *     summary  {"kind":"summary","text":…}。件数が分かるときは "count" も入る
 * ★失敗の理由は JSON になりません。今までどおり yui_errmsg() で読んでください。
 */
int yui_exec_json(yui *db, const char *query, yui_cb cb, void *ctx);

/* 直前の失敗の理由。失敗していなければ空文字列です。 */
const char *yui_errmsg(yui *db);

/*
 * 開いたときの知らせ（書きかけの末尾を切り捨てた等）。無ければ NULL です。
 */
const char *yui_notice(yui *db);

/* 書き出しを確定して閉じます。NULL を渡しても構いません。 */
int yui_close(yui *db);

#ifdef __cplusplus
}
#endif
#endif /* YUI_H */
