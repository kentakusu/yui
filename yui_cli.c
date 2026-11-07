/*
 * 結（yui）の CLI。
 *   yui ファイル                対話（端末でなければ、標準入力を 1 行ずつ実行）
 *   yui ファイル "問い合わせ"     1 回だけ実行
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

/* 1 行読む。長さの制限はありません。入力が尽きたら NULL */
static char *read_line(FILE *f) {
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    if (!b) abort();
    while (fgets(b + n, (int)(cap - n), f)) {
        n += strlen(b + n);
        if (n && b[n - 1] == '\n') return b;
        if (n + 1 >= cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) abort();
        }
    }
    if (n) return b;
    free(b);
    return NULL;
}

int main(int argc, char **argv) {
    yui *db;
    int failed = 0, tty, rc;
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
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "使い方: %s ファイル [問い合わせ]\n", argv[0]);
        return 2;
    }
    if (yui_open(argv[1], &db) != YUI_OK) {
        fprintf(stderr, "エラー: %s\n", yui_errmsg(db));
        yui_close(db);
        return 1;
    }
    if (yui_notice(db)) fprintf(stderr, "お知らせ: %s\n", yui_notice(db));
    if (argc == 3) {
        rc = yui_exec(db, argv[2], print_row, NULL);
        if (rc == YUI_ERROR) {
            fprintf(stderr, "エラー: %s\n", yui_errmsg(db));
            failed = 1;
        }
    } else {
        tty = isatty(0);
        if (tty) printf("結 yui %s（「手引き」で使い方、「終わる」で終了）\n", YUI_VERSION);
        for (;;) {
            if (tty) {
                fputs("yui> ", stdout);
                fflush(stdout);
            }
            line = read_line(stdin);
            if (!line) break;
            rc = yui_exec(db, line, print_row, NULL);
            free(line);
            if (rc == YUI_ERROR) {
                fflush(stdout);
                fprintf(stderr, "エラー: %s\n", yui_errmsg(db));
                failed = 1;
            } else if (rc == YUI_END) {
                break;
            }
            fflush(stdout);
        }
    }
    if (yui_close(db) != YUI_OK) {
        fprintf(stderr, "エラー: 閉じるときの書き出しに失敗しました\n");
        failed = 1;
    }
    return failed;
}
