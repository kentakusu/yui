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
