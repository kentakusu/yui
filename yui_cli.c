/*
 * 結（yui）の CLI。
 *   yui ファイル                対話（端末でなければ、標準入力を 1 行ずつ実行）
 *   yui ファイル "問い合わせ"     1 回だけ実行
 *   yui --json …                1 行を 1 つの JSON で出す（機械で読むとき）
 * 失敗があれば終了コードは 1 です。
 */
#define _POSIX_C_SOURCE 200809L
#include "yui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* ★shellapi.h は yui_win.h より先に読みます。yui_win.h が close や read を
 *   マクロで置き換えるので、後に読むとヘッダの宣言まで書き換わりかねません。 */
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#  include "yui_win.h" /* isatty のため。詳しくは yui_win.h */
#  ifdef _MSC_VER
#    pragma comment(lib, "shell32.lib") /* CommandLineToArgvW */
#  endif
#else
#  include <unistd.h>
#endif

#ifdef _WIN32
/*
 * ★Windows の main が受け取る argv は ANSI（日本語環境では cp932）です。
 *   結は UTF-8 しか受け取らないので、日本語を引数で渡すと弾かれます
 *   （実測: yui 知識.yui "探す 田中" →「問い合わせが UTF-8 として正しくありません」）。
 *   OS からは UTF-16 で取り直せるので、UTF-8 に直してから本体へ渡します。
 *   ファイル名も同じ経路で直るので、yui_win.h の _wopen がそのまま効きます。
 *   取れなかったときは、昔どおり ANSI の argv を使います（英数字だけなら動きます）。
 */
static char **argv_utf8(int *argc) {
    int n = 0, i, j;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **a;
    if (!w) return NULL;
    if (n <= 0) {
        LocalFree(w);
        return NULL;
    }
    a = (char **)malloc((size_t)(n + 1) * sizeof *a);
    if (!a) abort();
    for (i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        if (len <= 0) {
            for (j = 0; j < i; j++) free(a[j]);
            free(a);
            LocalFree(w);
            return NULL;
        }
        a[i] = (char *)malloc((size_t)len);
        if (!a[i]) abort();
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, a[i], len, NULL, NULL);
    }
    a[n] = NULL;
    LocalFree(w);
    *argc = n;
    return a;
}
#endif

static int print_row(void *ctx, int kind, const char *text) {
    (void)ctx;
    (void)kind;
    puts(text);
    return 0;
}

/*
 * --json のときの失敗の出し方。1 行 1 つの JSON で、標準エラーへ出します。
 * ★本体の yui_exec_json は結果だけを JSON にします。失敗の理由は
 *   yui_errmsg() が返す人向けの文なので、包むのは呼ぶ側（ここ）の仕事です。
 * ★包む相手は結の出した文ですが、その中に利用者が書いた名前が入ります。
 *   引用符や改行が混ざっても壊れないように、ここで逃がします。
 */
static void print_json_err(unsigned long lineno, const char *text) {
    fputs("{\"kind\":\"error\"", stderr);
    if (lineno) fprintf(stderr, ",\"line\":%lu", lineno);
    fputs(",\"text\":\"", stderr);
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            fputc('\\', stderr);
            fputc((char)c, stderr);
        } else if (c == '\n') {
            fputs("\\n", stderr);
        } else if (c == '\t') {
            fputs("\\t", stderr);
        } else if (c == '\r') {
            fputs("\\r", stderr);
        } else if (c < 0x20) {
            fprintf(stderr, "\\u%04x", c);
        } else {
            fputc((char)c, stderr);
        }
    }
    fputs("\"}\n", stderr);
}

/*
 * 1 行読む。長さの制限はありません。入力が尽きたら NULL。
 * *len に読んだバイト数を入れます（終わりの改行を含む）。
 *
 * ★NUL（文字コード 0）も読み落としません。1 バイトずつ読んでいるのはこのためです。
 *   古い版は fgets と strlen で組んでいたので、行の途中に NUL があると
 *   「そこまでが読めた長さ」と数えてしまい、次の fgets が NUL の位置から
 *   上書きして、次の行を巻き込んでいました。
 *   実測（古い版・2 行 渡した）:
 *     作る (点:甲 {本文: "x<NUL>y"})
 *     作る (点:乙)
 *   → 「文字列が閉じていません」1 つだけ出て、一覧 は 0 件。
 *     2 行目の 作る が黙って消えていました。
 *   NUL が入っていること自体は呼ぶ側（main）が見ます。長さが分かれば数えられます。
 */
static char *read_line(FILE *f, size_t *len) {
    size_t cap = 256, n = 0;
    char *b = malloc(cap);
    int c;
    if (!b) abort();
    *len = 0;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) abort();
        }
        b[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n) {
        free(b);
        return NULL;
    }
    b[n] = 0;
    *len = n;
    return b;
}

/* 失敗を 1 行 出す。--json なら JSON で */
static void say_err(int json, unsigned long lineno, const char *text) {
    fflush(stdout);
    if (json) {
        print_json_err(lineno, text);
    } else if (lineno) {
        fprintf(stderr, "エラー: %lu 行目: %s\n", lineno, text);
    } else {
        fprintf(stderr, "エラー: %s\n", text);
    }
}

int main(int argc, char **argv) {
    yui *db;
    int failed = 0, tty, rc, json = 0, ai = 1;
    const char *path, *query = NULL;
    char *line;
#ifdef _WIN32
    {
        int n = 0;
        char **a = argv_utf8(&n);
        if (a) {
            argc = n;
            argv = a; /* プロセスが終わるまで使うので、解放しません */
        }
    }
#endif
    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        json = 1;
        ai = 2;
    }
    if (argc - ai < 1 || argc - ai > 2) {
        fprintf(stderr, "使い方: %s [--json] ファイル [問い合わせ]\n", argv[0]);
        return 2;
    }
    path = argv[ai];
    if (argc - ai == 2) query = argv[ai + 1];
    if (yui_open(path, &db) != YUI_OK) {
        say_err(json, 0, yui_errmsg(db));
        yui_close(db);
        return 1;
    }
    /* ★お知らせは、--json でも人向けの文のまま標準エラーへ出します。
     *   結果（標準出力）に混ぜないので、機械で読む側の邪魔にはなりません。 */
    if (yui_notice(db)) fprintf(stderr, "お知らせ: %s\n", yui_notice(db));
    if (query) {
        rc = json ? yui_exec_json(db, query, print_row, NULL)
                  : yui_exec(db, query, print_row, NULL);
        if (rc == YUI_ERROR) {
            say_err(json, 0, yui_errmsg(db));
            failed = 1;
        }
    } else {
        size_t len;
        unsigned long lineno = 0;
        tty = isatty(0);
        if (tty) printf("結 yui %s（「手引き」で使い方、「終わる」で終了）\n", YUI_VERSION);
        for (;;) {
            if (tty) {
                fputs("yui> ", stdout);
                fflush(stdout);
            }
            line = read_line(stdin, &len);
            if (!line) break;
            lineno++;
            /*
             * ★NUL が入っていたら、ここで止めて行とバイトの位置を言います。
             *   yui_exec は C の文字列（NUL で終わる）を取るので、渡しても
             *   NUL までしか見てもらえません。本体からは「引用符が閉じないまま
             *   入力が尽きた」としか見えず、理由の分からないエラーになります。
             *   どこに NUL があるかを知っているのは、バイト数で読んだここだけです。
             */
            if (strlen(line) != len) {
                char b[160];
                snprintf(b, sizeof b,
                         "%lu バイト目に NUL（文字コード 0）が入っています。"
                         "問い合わせに NUL は書けません",
                         (unsigned long)strlen(line) + 1);
                say_err(json, lineno, b);
                failed = 1;
                free(line);
                continue; /* この行だけ飛ばして、次の行へ進みます */
            }
            rc = json ? yui_exec_json(db, line, print_row, NULL)
                      : yui_exec(db, line, print_row, NULL);
            free(line);
            if (rc == YUI_ERROR) {
                /* ★行番号は、流し込みのときだけ出します。
                 *   対話では「1 行目」と言われても手がかりになりません。 */
                say_err(json, tty ? 0 : lineno, yui_errmsg(db));
                failed = 1;
                /* ★止めません。次の行へ進みます。300 行 流して 1 行 間違っていた
                 *   ときに、残りを全部 捨てるほうが困るためです。
                 *   どこで何が起きたかは、上の行番号で分かります。
                 *   終了コードは 1 になるので、通ったかどうかは呼ぶ側で見られます。 */
            } else if (rc == YUI_END) {
                break;
            }
            fflush(stdout);
        }
    }
    if (yui_close(db) != YUI_OK) {
        say_err(json, 0, "閉じるときの書き出しに失敗しました");
        failed = 1;
    }
    return failed;
}
