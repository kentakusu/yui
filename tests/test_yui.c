/*
 * 結（yui）のテスト
 *
 *   yui_test tests/cases の .txt を全部   書いてある期待と突き合わせる
 *   yui_test --record tests/cases/basic.txt  期待を今の実装から作り直して出す
 *
 * 前半は tests/cases のケースを流す黒箱テスト、後半は問い合わせでは作れない
 * 状況（壊れたファイル・二重オープン）を直に確かめるテストです。
 * ケースの書き方は tests/README.md にあります。
 *
 * ★--record で出した期待は、必ず目で見てから commit してください。
 *   見ないで入れると「今そうなっている」を固めるだけになります。
 *
 * 識別子は本体と同じく ASCII です（日本語の識別子は古い C コンパイラで通りません）。
 */
#include "../yui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/*
 * ★Windows の narrow な fopen / remove も、パスを ANSI（cp932）と解釈します。
 *   テストが見る「<パス>.断片」は UTF-8 なので、ここだけ UTF-16 に直します
 *   （今の実装は作りません。作っていないことを確かめるために名前を持っています）。
 *   本体の側は yui_win.h が同じことをしています。
 *   ここで setlocale(".UTF8") に逃げると、本体の不具合も一緒に隠れてしまうので
 *   使いません（テストが効かなくなります）。
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near
#undef far
#include <fcntl.h>
#include <io.h>
#endif

#define TMP     "yui_test_tmp.yui"
#define TMPFRAG "yui_test_tmp.yui.断片"
#define TMPPACK "yui_test_tmp.yui.詰" /* 詰め直す の作業用。残っていたら失敗 */
#define TMPLOCK "yui_test_tmp.yui.錠" /* 土台をまたぐ錠 */
#define MAXROW  256

static int ng = 0;      /* 失敗した数 */
static int tried = 0;   /* 確かめた数 */
static int skipped = 0; /* この土台では確かめなかった数 */

static void *need(void *p) {
    if (!p) {
        fputs("メモリが足りません\n", stderr);
        exit(2);
    }
    return p;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    return (char *)memcpy(need(malloc(n)), s, n);
}

/* ------------------------------------------------------------------ */
/* 結果を受け取る                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *text[MAXROW];
    int   kind[MAXROW];
    int   n;
    int   over; /* MAXROW を超えた */
} Rows;

static int collect(void *ctx, int kind, const char *text) {
    Rows *g = (Rows *)ctx;
    if (g->n >= MAXROW) {
        g->over = 1;
        return 1;
    }
    g->text[g->n] = dup_str(text);
    g->kind[g->n] = kind;
    g->n++;
    return 0;
}

static void rows_free(Rows *g) {
    int i;
    for (i = 0; i < g->n; i++) free(g->text[i]);
    g->n = 0;
}

/* ------------------------------------------------------------------ */
/* ファイルを読む・消す（パスは UTF-8）                                */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)need(malloc((size_t)n * sizeof *w));
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static FILE *open_file(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

static int remove_file(const char *path) {
    wchar_t *wp = to_wide(path);
    int rc = wp ? _wremove(wp) : -1;
    free(wp);
    return rc;
}
#else
#define open_file fopen
#define remove_file remove
#endif

static char *read_file(const char *path, long *len) {
    FILE *f = open_file(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)need(malloc((size_t)n + 1));
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    b[n] = '\0';
    fclose(f);
    if (len) *len = n;
    return b;
}

static void clean(void) {
    remove_file(TMP);
    remove_file(TMPFRAG);
    remove_file(TMPPACK);
    remove_file(TMPLOCK);
}

static int file_exists(const char *path) {
    FILE *f = open_file(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* NUL を含むファイルの中を探す。strstr は最初の NUL で止まるので使えません */
static int has_bytes(const char *hay, long n, const char *needle) {
    size_t m = strlen(needle);
    long i;
    if (!hay || m == 0 || n < (long)m) return 0;
    for (i = 0; i + (long)m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ケースの 1 区切り（命令 1 つと、その期待）                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *cmd;
    int   line;
    char *want[MAXROW];
    int   want_kind[MAXROW];
    int   nwant;
    char *want_err; /* 期待するエラー文。無ければ NULL */
    int   want_end; /* 「終わる」が返ることを期待する */
} Blk;

static void blk_free(Blk *b) {
    int i;
    for (i = 0; i < b->nwant; i++) free(b->want[i]);
    free(b->cmd);
    free(b->want_err);
    memset(b, 0, sizeof *b);
}

static void dump(const char *head, char **text, int *kind, int n, const char *err, int end) {
    int i;
    printf("   %s:\n", head);
    for (i = 0; i < n; i++)
        printf("     %s%s\n", kind[i] == YUI_SUMMARY ? "+ " : "", text[i]);
    if (err) printf("     ! %s\n", err);
    if (end) printf("     @\n");
    if (n == 0 && !err && !end) printf("     （何も出ません）\n");
}

/* 1 区切りを実行して突き合わせる。合っていれば 0 */
static int check_blk(yui *db, const char *name, Blk *b) {
    Rows g;
    int rc, i, bad = 0;
    const char *got_err = NULL;
    int got_end = 0;

    memset(&g, 0, sizeof g);
    tried++;
    rc = yui_exec(db, b->cmd, collect, &g);
    if (rc == YUI_ERROR) got_err = yui_errmsg(db);
    if (rc == YUI_END) got_end = 1;

    if (g.over) bad = 1;
    if (!b->want_err != !got_err) bad = 1;
    else if (b->want_err && strcmp(b->want_err, got_err) != 0) bad = 1;
    if (b->want_end != got_end) bad = 1;
    if (g.n != b->nwant) {
        bad = 1;
    } else {
        for (i = 0; i < g.n; i++)
            if (b->want_kind[i] != g.kind[i] || strcmp(b->want[i], g.text[i]) != 0) bad = 1;
    }

    if (bad) {
        ng++;
        printf("NG %s:%d  > %s\n", name, b->line, b->cmd);
        dump("期待", b->want, b->want_kind, b->nwant, b->want_err, b->want_end);
        dump("実際", g.text, g.kind, g.n, got_err, got_end);
        if (g.over) printf("   （%d 行を超えたので打ち切りました）\n", MAXROW);
    }
    rows_free(&g);
    return bad;
}

/* 実際の結果を、そのまま期待の書き方で出す（--record 用） */
static void record_blk(yui *db, Blk *b) {
    Rows g;
    int rc, i;
    memset(&g, 0, sizeof g);
    rc = yui_exec(db, b->cmd, collect, &g);
    for (i = 0; i < g.n; i++)
        printf("%s%s\n", g.kind[i] == YUI_SUMMARY ? "+ " : "", g.text[i]);
    if (rc == YUI_ERROR) printf("! %s\n", yui_errmsg(db));
    if (rc == YUI_END) printf("@\n");
    if (g.over) {
        fprintf(stderr, "%d 行を超えました。ケースを小さくしてください\n", MAXROW);
        ng++;
    }
    rows_free(&g);
}

/* ------------------------------------------------------------------ */
/* ケースファイルを流す                                                */
/* ------------------------------------------------------------------ */

static yui *open_tmp(const char *name) {
    yui *db;
    if (yui_open(TMP, &db) != YUI_OK) {
        printf("NG %s  開けません: %s\n", name, yui_errmsg(db));
        ng++;
        yui_close(db);
        return NULL;
    }
    return db;
}

static int run_case(const char *path, int record) {
    long len;
    char *body = read_file(path, &len);
    char *p;
    int lineno = 0, pending = 0;
    Blk b;
    yui *db;

    if (!body) {
        printf("NG %s  読めません\n", path);
        ng++;
        return 1;
    }
    clean();
    db = open_tmp(path);
    if (!db) {
        free(body);
        return 1;
    }
    memset(&b, 0, sizeof b);

    p = body;
    while (p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        size_t n;
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = NULL;
        }
        lineno++;
        n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = '\0'; /* CRLF で取り出されても動く */

        /* 末尾の改行の後ろ。行ではないので数えない
         * （数えると --record が毎回 空行を 1 行 足していきます） */
        if (p == NULL && n == 0) break;

        /* 命令・指示・コメント・空行が来たら、待っている区切りを先に片付ける */
        if (n == 0 || line[0] == '#' || line[0] == '>' ||
            (line[0] == '=' && line[1] == '=')) {
            if (pending) {
                if (record) record_blk(db, &b);
                else check_blk(db, path, &b);
                blk_free(&b);
                pending = 0;
            }
        }

        if (n == 0 || line[0] == '#') {
            if (record) printf("%s\n", line);
            continue;
        }
        if (line[0] == '=' && line[1] == '=') {
            if (record) printf("%s\n", line);
            if (strstr(line, "開き直す")) {
                if (yui_close(db) != YUI_OK) {
                    printf("NG %s:%d  閉じるときに失敗しました\n", path, lineno);
                    ng++;
                    free(body);
                    return 1;
                }
                db = open_tmp(path);
                if (!db) {
                    free(body);
                    return 1;
                }
            } else {
                printf("NG %s:%d  知らない指示です: %s\n", path, lineno, line);
                ng++;
            }
            continue;
        }
        if (line[0] == '>') {
            if (record) printf("%s\n", line);
            b.cmd = dup_str(line[1] == ' ' ? line + 2 : line + 1);
            b.line = lineno;
            pending = 1;
            continue;
        }
        /* ここから下は期待の行。--record では捨てて、実際の結果で置き換える */
        if (record) continue;
        if (!pending) {
            printf("NG %s:%d  命令より前に期待が書いてあります: %s\n", path, lineno, line);
            ng++;
            continue;
        }
        if (line[0] == '!' && line[1] == ' ') {
            free(b.want_err);
            b.want_err = dup_str(line + 2);
        } else if (line[0] == '@' && n == 1) {
            b.want_end = 1;
        } else if (b.nwant < MAXROW) {
            int sum = (line[0] == '+' && line[1] == ' ');
            b.want_kind[b.nwant] = sum ? YUI_SUMMARY : YUI_ROW;
            b.want[b.nwant] = dup_str(sum ? line + 2 : line);
            b.nwant++;
        }
    }
    if (pending) {
        if (record) record_blk(db, &b);
        else check_blk(db, path, &b);
        blk_free(&b);
    }
    yui_close(db);
    free(body);
    clean();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 問い合わせでは作れない状況を直に確かめる                            */
/* ------------------------------------------------------------------ */

static void ok(const char *name, int cond, const char *got) {
    tried++;
    if (cond) return;
    ng++;
    printf("NG %s\n", name);
    if (got) printf("   実際: %s\n", got);
}

/* この土台では確かめられないもの。黙って飛ばさず、毎回 数に出します */
static void skip(const char *name, const char *why) {
    skipped++;
    printf("-- 見ていません: %s（%s）\n", name, why);
}

static int starts(const char *s, const char *t) {
    return strncmp(s, t, strlen(t)) == 0;
}

/* 点 2 つと線 1 本の小さなファイルを作る */
static void make_small(void) {
    yui *db;
    clean();
    if (yui_open(TMP, &db) != YUI_OK) {
        ok("下ごしらえ（小さなファイルを作る）", 0, yui_errmsg(db));
    } else {
        yui_exec(db, "作る (人:田中 {役職: \"部長\"})", NULL, NULL);
        yui_exec(db, "結ぶ (案件:結)-[担当]->(人:田中)", NULL, NULL);
    }
    yui_close(db);
}

static void direct_tests(void) {
    yui *db, *db2;
    char *body;
    long len;
    FILE *f;
    Rows g;

    /* 1. 同じファイルを 2 つ開けない
     *    ★土台ごとに錠の仕組みが違う（POSIX は flock・Windows は LockFileEx・
     *      wasm は何もしない）。ここが土台をまたいで最もずれる場所です。 */
    make_small();
    if (yui_open(TMP, &db) == YUI_OK) {
        int rc = yui_open(TMP, &db2);
#ifdef __EMSCRIPTEN__
        /* emscripten の flock は何もしない作り（weak stub）なので、
         * この土台では排他を確かめられません。ネイティブと wasm が同じ
         * ファイルを同時に書くと、警告も出ないまま壊れます。 */
        skip("同じファイルを 2 つ開けないこと",
             rc == YUI_OK
                 ? "wasm の flock は何もしないので開けてしまいます。"
                   "塞ぎたいときは yui_open_locked を使ってください（下の錠の検査で見ています）"
                 : "wasm なのに断りました。flock が入ったのかもしれません");
#else
        ok("同じファイルを 2 つ開けないこと", rc == YUI_ERROR,
           rc == YUI_OK ? "開けてしまいました" : NULL);
        if (rc != YUI_OK)
            ok("断る理由が「別のところで開かれています」であること",
               starts(yui_errmsg(db2), "別のところで開かれています"), yui_errmsg(db2));
#endif
        yui_close(db2);
    } else {
        ok("下ごしらえ（1 つめを開く）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 2. 閉じて開き直すと中身が残っている */
    if (yui_open(TMP, &db) == YUI_OK) {
        memset(&g, 0, sizeof g);
        yui_exec(db, "見る (人:田中)", collect, &g);
        ok("開き直しても属性が残ること", g.n >= 1 && strstr(g.text[0], "役職") != NULL,
           g.n ? g.text[0] : "（何も出ません）");
        rows_free(&g);
        memset(&g, 0, sizeof g);
        yui_exec(db, "手引き", collect, &g);
        ok("手引きが 10 行以上 出ること", g.n >= 10, NULL);
        rows_free(&g);
    } else {
        ok("下ごしらえ（開き直す）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 3. 書きかけの末尾は切り捨てて知らせる。捨てた分は残さない
     *    ★以前は <パス>.断片 へ退避していました。消したはずの本文が隣に残るので、
     *      作らないことにしました（yui.c の replay の★）。 */
    make_small();
    f = open_file(TMP, "ab");
    if (f) {
        fwrite("\xff\xff\xff\xff\x01", 1, 5, f);
        fclose(f);
    }
    if (yui_open(TMP, &db) == YUI_OK) {
        ok("書きかけの末尾を切り捨てたら知らせること",
           yui_notice(db) != NULL && strstr(yui_notice(db), "捨てました") != NULL,
           yui_notice(db) ? yui_notice(db) : "（お知らせがありません）");
        memset(&g, 0, sizeof g);
        yui_exec(db, "一覧 ()", collect, &g);
        ok("切り捨てても、その前の記録は読めること", g.n == 3, NULL); /* 点 2 つ + 締め */
        rows_free(&g);
    } else {
        ok("書きかけの末尾があっても開けること", 0, yui_errmsg(db));
    }
    yui_close(db);
    body = read_file(TMPFRAG, &len);
    ok(".断片 を作らないこと", body == NULL, body ? "できてしまいました" : NULL);
    free(body);

    /* 4. 途中が壊れていたら開かない
     *    ★末尾と違って救い出す道がありません（README の「気をつけること」）。 */
    make_small();
    body = read_file(TMP, &len);
    if (body && len > 20) {
        body[14] ^= 0x40; /* 1 件目の本体を 1 ビット書き換える */
        f = open_file(TMP, "wb");
        if (f) {
            fwrite(body, 1, (size_t)len, f);
            fclose(f);
        }
        ok("途中が壊れていたら開かないこと", yui_open(TMP, &db) == YUI_ERROR, NULL);
        ok("壊れている場所を知らせること", strstr(yui_errmsg(db), "途中") != NULL,
           yui_errmsg(db));
        yui_close(db);
    } else {
        ok("下ごしらえ（途中を壊す）", 0, NULL);
    }
    free(body);

    /* 5. 結のファイルでなければ、書き換えずに断る */
    clean();
    f = open_file(TMP, "wb");
    if (f) {
        fwrite("これは結のファイルではありません", 1, 48, f);
        fclose(f);
    }
    ok("結のファイルでなければ開かないこと", yui_open(TMP, &db) == YUI_ERROR, NULL);
    ok("結のファイルでないと言うこと", starts(yui_errmsg(db), "結のファイルではありません"),
       yui_errmsg(db));
    yui_close(db);
    body = read_file(TMP, &len);
    ok("断ったファイルを書き換えないこと", body != NULL && len == 48, NULL);
    free(body);

    /* 6. 開けないとき */
    clean();
    ok("無いディレクトリなら断ること",
       yui_open("この下に置き場はありません/x.yui", &db) == YUI_ERROR, NULL);
    ok("開けない理由を言うこと", yui_errmsg(db)[0] != '\0', NULL);
    yui_close(db);

    ok("空のファイル名を断ること", yui_open("", &db) == YUI_ERROR, NULL);
    yui_close(db);

    /* 7. NULL を閉じても落ちない */
    ok("yui_close(NULL) が落ちないこと", yui_close(NULL) == YUI_OK, NULL);

    /* 8. 詰め直すと、取り消した本文がファイルから無くなる
     *    ★先に「消しただけでは残っている」ことを見ます（陽性対照）。
     *      これが無いと、探し方が悪くて見つからないのと区別が付きません。 */
    clean();
    if (yui_open(TMP, &db) == YUI_OK) {
        yui_exec(db, "作る (メモ:あとで消す {本文: \"ここだけの話\"})", NULL, NULL);
        yui_exec(db, "作る (人:残る人)", NULL, NULL);
        yui_exec(db, "結ぶ (案件:結)-[担当]->(人:残る人)", NULL, NULL);
        yui_exec(db, "消す (メモ:あとで消す)", NULL, NULL);
    } else {
        ok("下ごしらえ（詰め直す 用のファイルを作る）", 0, yui_errmsg(db));
    }
    yui_close(db);
    body = read_file(TMP, &len);
    ok("消しただけでは本文がファイルに残ること（陽性対照）",
       has_bytes(body, len, "ここだけの話"), "残っていません");
    free(body);
    if (yui_open(TMP, &db) == YUI_OK) {
        memset(&g, 0, sizeof g);
        yui_exec(db, "詰め直す", collect, &g);
        ok("詰め直したら知らせること",
           g.n == 1 && strstr(g.text[0], "詰め直しました") != NULL,
           g.n ? g.text[0] : "（何も出ません）");
        rows_free(&g);
        memset(&g, 0, sizeof g);
        yui_exec(db, "一覧 ()", collect, &g);
        ok("詰め直したあとも、残る点が読めること", g.n == 3, NULL); /* 点 2 つ + 締め */
        rows_free(&g);
        memset(&g, 0, sizeof g);
        yui_exec(db, "辿る (案件:結)-[担当]->(?誰か)", collect, &g);
        ok("詰め直したあとも、線が辿れること（id を振り直すので）", g.n == 2,
           g.n ? g.text[0] : "（何も出ません）");
        rows_free(&g);
        memset(&g, 0, sizeof g);
        yui_exec(db, "作る (人:あとから足す人)", collect, &g);
        ok("詰め直したあとに書き足せること", g.n == 2, g.n ? g.text[0] : "（何も出ません）");
        rows_free(&g);
    } else {
        ok("詰め直す ために開けること", 0, yui_errmsg(db));
    }
    yui_close(db);
    body = read_file(TMP, &len);
    ok("詰め直すと取り消した本文が消えること", !has_bytes(body, len, "ここだけの話"),
       "まだ残っています");
    ok("詰め直しても残る点の本文は在ること", has_bytes(body, len, "残る人"), NULL);
    ok("詰め直したあとに書き足した分も在ること", has_bytes(body, len, "あとから足す人"), NULL);
    free(body);
    body = read_file(TMPPACK, &len);
    ok("作業用の .詰 を残さないこと", body == NULL, body ? "残っています" : NULL);
    free(body);

    /* 8-2. 古い版が残した .断片 があったら、詰め直す のときに知らせる
     *      ★今の実装は .断片 を作らないので、ここで手で置いて確かめます。
     *        置かないとこの道は 1 度も通らず、「在る」だけで終わります。 */
    clean();
    if (yui_open(TMP, &db) == YUI_OK) {
        yui_exec(db, "作る (人:田中)", NULL, NULL);
        yui_close(db);
        f = open_file(TMPFRAG, "wb");
        if (f) {
            fwrite("書きかけの末尾", 1, 21, f);
            fclose(f);
        }
        if (yui_open(TMP, &db) == YUI_OK) {
            memset(&g, 0, sizeof g);
            yui_exec(db, "詰め直す", collect, &g);
            ok("古い .断片 が残っていたら知らせること",
               g.n == 2 && strstr(g.text[0], ".断片") != NULL,
               g.n ? g.text[0] : "（何も出ません）");
            rows_free(&g);
        } else {
            ok("下ごしらえ（.断片 を置いて開く）", 0, yui_errmsg(db));
        }
        yui_close(db);
        body = read_file(TMPFRAG, &len);
        ok("知らせるだけで、.断片 を消さないこと", body != NULL && len == 21, NULL);
        free(body);
    } else {
        ok("下ごしらえ（.断片 の試し）", 0, yui_errmsg(db));
    }

    /* 9. 作業用の .詰 が既にあったら、消さずに断る
     *    ★前の詰め直しが途中で終わった跡を、黙って上書きしないためです。 */
    clean();
    if (yui_open(TMP, &db) == YUI_OK) {
        yui_exec(db, "作る (人:田中)", NULL, NULL);
        f = open_file(TMPPACK, "wb");
        if (f) {
            fwrite("人のもの", 1, 12, f);
            fclose(f);
        }
        memset(&g, 0, sizeof g);
        ok(".詰 が既にあったら断ること", yui_exec(db, "詰め直す", collect, &g) == YUI_ERROR,
           NULL);
        ok("断る理由に名前を出すこと", strstr(yui_errmsg(db), TMPPACK) != NULL, yui_errmsg(db));
        rows_free(&g);
    } else {
        ok("下ごしらえ（.詰 を置く）", 0, yui_errmsg(db));
    }
    yui_close(db);
    body = read_file(TMPPACK, &len);
    ok("断ったとき、人が置いた .詰 を消さないこと", body != NULL && len == 12, NULL);
    free(body);

    /* 10. 機械で読む形（yui_exec_json）
     *     ★ケースのファイル（tests/cases の中）は人が読む形を見ているので、
     *       JSON はここで直に見ます。行の数と順は yui_exec と同じはずです。 */
    clean();
    if (yui_open(TMP, &db) == YUI_OK) {
        yui_exec(db,
                 "作る (発言:x1 {日時: \"07:30\", 話者: \"本人\", 順: 1, 重み: 0.5})\n"
                 "作る (発言:x2 {日時: \"08:00\"})\n"
                 "結ぶ (日:2026-11-08)-[含む {順: 1}]->(発言:x1)",
                 NULL, NULL);

        memset(&g, 0, sizeof g);
        yui_exec_json(db, "一覧 (発言:)", collect, &g);
        ok("点が kind=node で出ること", g.n == 3 && strstr(g.text[0], "\"kind\":\"node\"") != NULL,
           g.n ? g.text[0] : NULL);
        ok("ラベルと名前が鍵で取れること",
           g.n == 3 && strstr(g.text[0], "\"label\":\"発言\"") != NULL &&
               strstr(g.text[0], "\"name\":\"x1\"") != NULL,
           g.n ? g.text[0] : NULL);
        ok("数は数のまま出ること（引用符を付けない）",
           g.n == 3 && strstr(g.text[0], "\"順\":1") != NULL &&
               strstr(g.text[0], "\"重み\":0.5") != NULL,
           g.n ? g.text[0] : NULL);
        ok("締めに件数が数で入ること",
           g.n == 3 && strstr(g.text[2], "\"kind\":\"summary\"") != NULL &&
               strstr(g.text[2], "\"count\":2") != NULL,
           g.n == 3 ? g.text[2] : NULL);
        rows_free(&g);

        /* ★人が読む形と行の数が同じこと。片方だけ増えたら、どちらかが壊れています */
        {
            Rows a, b;
            memset(&a, 0, sizeof a);
            memset(&b, 0, sizeof b);
            yui_exec(db, "一覧 (発言:)", collect, &a);
            yui_exec_json(db, "一覧 (発言:)", collect, &b);
            ok("人が読む形と JSON で、行の数が同じこと", a.n == b.n, NULL);
            rows_free(&a);
            rows_free(&b);
        }

        /* ★引用符・改行・バックスラッシュが壊れないこと。
         *   これが壊れていると、読む側が落ちます（しかも値は出ているように見えます）。 */
        yui_exec(db, "作る (メモ:m1 {本文: \"引用\\\"と改行\\nと\\\\ です\"})", NULL, NULL);
        memset(&g, 0, sizeof g);
        yui_exec_json(db, "一覧 (メモ:)", collect, &g);
        ok("引用符を逃がすこと", g.n && strstr(g.text[0], "引用\\\"と") != NULL,
           g.n ? g.text[0] : NULL);
        ok("改行を逃がすこと", g.n && strstr(g.text[0], "と改行\\nと") != NULL,
           g.n ? g.text[0] : NULL);
        ok("バックスラッシュを逃がすこと", g.n && strstr(g.text[0], "\\\\ です") != NULL,
           g.n ? g.text[0] : NULL);
        rows_free(&g);

        /* 辿る の道 */
        memset(&g, 0, sizeof g);
        yui_exec_json(db, "辿る (日:2026-11-08)-[含む]->(?発言)", collect, &g);
        ok("道が kind=path で出ること", g.n == 2 && strstr(g.text[0], "\"kind\":\"path\"") != NULL,
           g.n ? g.text[0] : NULL);
        ok("道に nodes と edges が入ること",
           g.n == 2 && strstr(g.text[0], "\"nodes\":[") != NULL &&
               strstr(g.text[0], "\"edges\":[") != NULL,
           g.n ? g.text[0] : NULL);
        ok("道が二重に包まれないこと（kind=text にならない）",
           g.n == 2 && strstr(g.text[0], "\"kind\":\"text\"") == NULL, g.n ? g.text[0] : NULL);
        rows_free(&g);

        /* 見る のつながり */
        memset(&g, 0, sizeof g);
        yui_exec_json(db, "見る (発言:x1)", collect, &g);
        ok("つながりが kind=link で出ること",
           g.n == 3 && strstr(g.text[1], "\"kind\":\"link\"") != NULL &&
               strstr(g.text[1], "\"dir\":\"in\"") != NULL,
           g.n > 1 ? g.text[1] : NULL);
        rows_free(&g);

        /* 手引き のような人向けの行 */
        memset(&g, 0, sizeof g);
        yui_exec_json(db, "手引き", collect, &g);
        ok("人向けの行が kind=text で出ること",
           g.n > 10 && strstr(g.text[0], "\"kind\":\"text\"") != NULL, g.n ? g.text[0] : NULL);
        rows_free(&g);

        /* ★JSON を頼んだ次の yui_exec は、人が読む形に戻ること */
        memset(&g, 0, sizeof g);
        yui_exec(db, "一覧 (発言:)", collect, &g);
        ok("次の yui_exec は人が読む形に戻ること",
           g.n == 3 && g.text[0][0] == '(' && strstr(g.text[0], "\"kind\"") == NULL,
           g.n ? g.text[0] : NULL);
        rows_free(&g);
    } else {
        ok("下ごしらえ（機械で読む形）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* まだ見ていないもの。黙っていると「見た」と読まれるので、毎回 出します */
    skip("詰め直す が途中まで書けたあとで失敗する道",
         "作業用のファイルを作れないとき（tests/cli.sh）・入れ替えられないとき"
         "（CI の Windows）・.詰 が既にあるとき（ここ）は見ています"
         "。書いている途中で失敗する道（置き場が尽きる等）は、まだ通していません");
    skip("Windows の CLI の一部",
         "CLI の振る舞いは tests/cli.sh が見ていますが、sh が要るので Windows では"
         "動きません。Windows は CI の pwsh の段で、終了コード・行番号・NUL・"
         "詰め直す の入れ替えの失敗だけを見ています（端末として開いたときと --json は"
         "見ていません）");

    clean();
}

/* ------------------------------------------------------------------ */

#ifdef YUI_SELFCHECK
/* 錠を取った直後に、別のところが先に取っていた状況を作ります（本体の差し込み口） */
extern void (*yui_selfcheck_took_lock)(const char *lpath);
static void steal_lock(const char *lpath) {
    FILE *f = open_file(lpath, "wb");
    if (!f) return;
    fputs("yui-lock 1\nname=先に取った誰か\ntag=0000000000000000\n", f);
    fclose(f);
}
#endif

/* 錠の中身から tag= の値を取り出します */
static void tag_of(const char *body, char *buf, size_t n) {
    const char *s = strstr(body, "\ntag=");
    size_t i = 0;
    buf[0] = 0;
    if (!s) return;
    s += 5;
    for (; s[i] && s[i] != '\n' && i + 1 < n; i++) buf[i] = s[i];
    buf[i] = 0;
}

/*
 * 土台をまたいで効く錠（yui_open_locked / yui_lock_owner）。
 * ★ここで見ているのは「同じ土台の中で、錠の決まりどおりに動くか」です。
 *   土台をまたいで本当に排除できるかは、1 つのプロセスからは確かめられません
 *   （別の土台のプロセスを起こす必要があります）。その実測は
 *   docs/デーモンの構想.md の「測った土台の性質」にあります。
 */
static void lock_tests(void) {
    yui *db, *db2;
    char owner[1024];
    char *body;
    long len;
    FILE *f;

    /* 1. 錠を使って開くと錠ができ、中身に名乗りが入る */
    make_small();
    if (yui_open_locked(TMP, "試験 1.0", 0, &db) == YUI_OK) {
        body = read_file(TMPLOCK, &len);
        ok("錠を使って開くと 錠のファイルができること", body != NULL, NULL);
        if (body) {
            ok("錠の 1 行目が yui-lock であること", starts(body, "yui-lock 1\n"), body);
            ok("錠に名乗りが入ること", strstr(body, "name=試験 1.0\n") != NULL, body);
            ok("錠に pid が入ること", strstr(body, "\npid=") != NULL, body);
            ok("錠に since が入ること", strstr(body, "\nsince=") != NULL, body);
            free(body);
        }

        /* 2. 開かずに持ち主を読める */
        owner[0] = 0;
        ok("持ち主を開かずに読めること",
           yui_lock_owner(TMP, owner, sizeof owner) == YUI_OK &&
               strstr(owner, "name=試験 1.0\n") != NULL,
           owner);

        /* 3. 2 つめは断られ、持ち主を名乗る
         *    ★錠を flock より先に当てているので、同じ土台どうしでも
         *      「誰が持っているか」が出ます。順番が逆だと出ません。 */
        if (yui_open_locked(TMP, "試験 2.0", 0, &db2) == YUI_ERROR) {
            ok("断る理由が錠のことであること",
               starts(yui_errmsg(db2), "別のところが錠を持っています"), yui_errmsg(db2));
            ok("断る理由に持ち主の名乗りが入ること",
               strstr(yui_errmsg(db2), "name=試験 1.0") != NULL, yui_errmsg(db2));
        } else {
            ok("錠が塞がれていたら断ること", 0, "開けてしまいました");
        }
        yui_close(db2);
        /* ★断られた側が閉じても、持ち主の錠を消してはいけません */
        ok("断られた側が閉じても錠が残ること", file_exists(TMPLOCK), NULL);

        yui_close(db);
    } else {
        ok("下ごしらえ（錠を使って開く）", 0, yui_errmsg(db));
        yui_close(db);
    }
    ok("閉じると錠が消えること", !file_exists(TMPLOCK), NULL);

    /* 4. 錠が無いのは失敗ではない。持ち主が空で返る */
    owner[0] = 'x';
    owner[1] = 0;
    ok("錠が無ければ持ち主は空で YUI_OK が返ること",
       yui_lock_owner(TMP, owner, sizeof owner) == YUI_OK && owner[0] == 0, owner);

    /* 5. 落ちた持ち主が残した錠（残骸）は、自動では壊さない */
    f = open_file(TMPLOCK, "wb");
    if (f) {
        fputs("yui-lock 1\nname=落ちた誰か\nmachine=どこか\npid=999999\nsince=1\n", f);
        fclose(f);
    }
    ok("残骸があると断ること", yui_open_locked(TMP, "試験 3.0", 0, &db) == YUI_ERROR, NULL);
    ok("断る理由に残骸の持ち主が出ること", strstr(yui_errmsg(db), "name=落ちた誰か") != NULL,
       yui_errmsg(db));
    yui_close(db);
    ok("自動では残骸を壊さないこと", file_exists(TMPLOCK), NULL);

    /* 6. 壊して取る（1 回の呼び出しで） */
    if (yui_open_locked(TMP, "試験 4.0", YUI_LOCK_BREAK, &db) == YUI_OK) {
        owner[0] = 0;
        yui_lock_owner(TMP, owner, sizeof owner);
        ok("壊して取ると錠が自分のものになること", strstr(owner, "name=試験 4.0\n") != NULL,
           owner);
        ok("壊して取ると前の持ち主が消えること", strstr(owner, "落ちた誰か") == NULL, owner);
    } else {
        ok("壊して取れること", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 7. 名乗りの制御文字で錠の形が壊れないこと */
    if (yui_open_locked(TMP, "改行\nを入れた", 0, &db) == YUI_OK) {
        owner[0] = 0;
        yui_lock_owner(TMP, owner, sizeof owner);
        ok("名乗りの改行が空白になること", strstr(owner, "name=改行 を入れた\n") != NULL,
           owner);
    } else {
        ok("下ごしらえ（改行入りの名乗りで開く）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 8. 壊して取り直す隙に、別のところが先に取ったとき
     *    ★この窓は無くせないので、取ったあと読み返して自分のものか確かめます。
     *      1 プロセスからは本物の取り合いを起こせないため、本体の
     *      差し込み口（-DYUI_SELFCHECK のときだけ在る）で状況を作ります。 */
#ifdef YUI_SELFCHECK
    f = open_file(TMPLOCK, "wb");
    if (f) {
        fputs("yui-lock 1\nname=落ちた誰か\n", f);
        fclose(f);
    }
    yui_selfcheck_took_lock = steal_lock;
    ok("取り合いに負けたら断ること",
       yui_open_locked(TMP, "試験 5.0", YUI_LOCK_BREAK, &db) == YUI_ERROR, NULL);
    ok("負けた理由がそう言えること", starts(yui_errmsg(db), "錠を取り合って負けました"),
       yui_errmsg(db));
    yui_selfcheck_took_lock = NULL;
    yui_close(db);
    remove_file(TMPLOCK);
#else
    skip("取り合いに負けたときに断ること",
         "状況を作る差し込み口が -DYUI_SELFCHECK のビルドにしかありません");
#endif

    /* 9. ★錠の印（tag）が毎回 変わること
     *    材料が time() だけだと、同じ秒に 2 つが壊して取り合ったとき
     *    両方が「自分の印」を見てしまいます（wasm で実測されました）。
     *    ここは同じ秒に 2 回 開いて、印が変わることを見ています。 */
    {
        char tag1[32], tag2[32];
        tag1[0] = tag2[0] = 0;
        if (yui_open_locked(TMP, "印の試験", 0, &db) == YUI_OK) {
            owner[0] = 0;
            yui_lock_owner(TMP, owner, sizeof owner);
            tag_of(owner, tag1, sizeof tag1);
        }
        yui_close(db);
        if (yui_open_locked(TMP, "印の試験", 0, &db) == YUI_OK) {
            owner[0] = 0;
            yui_lock_owner(TMP, owner, sizeof owner);
            tag_of(owner, tag2, sizeof tag2);
        }
        yui_close(db);
        ok("錠の印が取れること", tag1[0] != 0 && tag2[0] != 0, tag1);
        ok("錠の印が毎回 変わること", strcmp(tag1, tag2) != 0, tag1);
    }

    /* 10. 読むだけで開くと、ファイルを変える言葉を断ること */
    if (yui_open_locked(TMP, "読むだけの試験", YUI_READONLY, &db) == YUI_OK) {
        int i;
        static const char *const writes[] = {"作る (人:新顔)", "結ぶ (人:田中)-[印]->(人:新顔)",
                                             "消す (人:田中)", "詰め直す", NULL};
        static const char *const reads[] = {"一覧 (人:)", "見る (人:田中)", "探す \"田中\"",
                                            "辿る (人:田中)-[]->()", "手引き", "同期", NULL};
        for (i = 0; writes[i]; i++) {
            char nm[96];
            snprintf(nm, sizeof nm, "読むだけなら断ること: %s", writes[i]);
            ok(nm, yui_exec(db, writes[i], NULL, NULL) == YUI_ERROR, NULL);
        }
        ok("断る理由が「読むだけで開いています」であること",
           starts(yui_errmsg(db), "読むだけで開いています"), yui_errmsg(db));
        for (i = 0; reads[i]; i++) {
            char nm[96];
            snprintf(nm, sizeof nm, "読むだけでも通ること: %s", reads[i]);
            ok(nm, yui_exec(db, reads[i], NULL, NULL) == YUI_OK, yui_errmsg(db));
        }
    } else {
        ok("下ごしらえ（読むだけで開く）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 11. 共有で開くなら、読むだけでなければいけない
     *    ★共有の錠を持ったまま書くと、同時に読んでいる人が壊れたものを読みます。 */
    ok("共有だけでは開けないこと", yui_open_full(TMP, "共有の試験", YUI_SHARED, 0, &db) == YUI_ERROR,
       NULL);
    ok("その理由を言うこと", starts(yui_errmsg(db), "共有で開くなら"), yui_errmsg(db));
    yui_close(db);

    /* 12. 共有で開く人は、排他の錠（<パス>.錠）を取らない */
    if (yui_open_full(TMP, "共有の試験", YUI_READONLY | YUI_SHARED, 0, &db) == YUI_OK) {
        ok("共有で開くと錠のファイルを作らないこと", !file_exists(TMPLOCK), NULL);
        ok("共有でも読めること", yui_exec(db, "一覧 (人:)", NULL, NULL) == YUI_OK, yui_errmsg(db));
        ok("共有でも書けないこと", yui_exec(db, "作る (人:新顔)", NULL, NULL) == YUI_ERROR, NULL);
    } else {
        ok("下ごしらえ（共有で開く）", 0, yui_errmsg(db));
    }
    yui_close(db);

    /* 13. ★既定 off。ふつうに開いたら錠を作らない */
    if (yui_open(TMP, &db) == YUI_OK)
        ok("ふつうに開くと錠を作らないこと", !file_exists(TMPLOCK), NULL);
    else
        ok("下ごしらえ（ふつうに開く）", 0, yui_errmsg(db));
    yui_close(db);

    clean();
}

int main(int argc, char **argv) {
    int i, record = 0, cases = 0;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--record") == 0) record = 1;
#ifdef _WIN32
    /* ★期待の行は LF で出します。text モードのままだと CRLF になり、
     *   ケースファイルの改行が土台によって入れ替わります。 */
    if (record) _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--record") == 0) continue;
        run_case(argv[i], record);
        cases++;
    }
    if (record) return ng ? 1 : 0;

    direct_tests();
    lock_tests();

    if (ng == 0) {
        printf("ok  ケース %d 本 / 確かめた %d 件 / 見ていない %d 件 — 失敗 0 件\n",
               cases, tried, skipped);
        return 0;
    }
    printf("NG  ケース %d 本 / 確かめた %d 件 / 見ていない %d 件 のうち %d 件 失敗\n",
           cases, tried, skipped, ng);
    return 1;
}
