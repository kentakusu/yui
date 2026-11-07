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
#  include "yui_win.h" /* isatty のため。詳しくは yui_win.h */
#else
#  include <unistd.h>
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
