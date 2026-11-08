/*
 * Windows の argv を UTF-8 で取り直す受け皿。
 *
 * ★Windows の main が受け取る argv は ANSI（日本語環境では cp932）です。
 *   結は UTF-8 しか受け取らないので、日本語を引数で渡すと弾かれます
 *   （実測: yui 知識.yui "探す 田中" →「問い合わせが UTF-8 として正しくありません」）。
 *   OS からは UTF-16 で取り直せるので、UTF-8 に直してから本体へ渡します。
 *   ファイル名も同じ経路で直るので、yui_win.h の _wopen がそのまま効きます。
 *   取れなかったときは、昔どおり ANSI の argv を使います（英数字だけなら動きます）。
 *
 * ★yui_cli.c と yuid.c の両方が使います。写して 2 箇所に置くと、片方だけ直って
 *   もう片方が古くなります。しかも**日本語のパスでしか現れない**ので、
 *   英数字で試している間は気づけません。だからここに 1 つだけ置いてあります。
 *
 * ★この受け皿は yui_win.h より先に読んでください。yui_win.h は close や read を
 *   マクロで置き換えるので、後に読むとヘッダの宣言まで書き換わりかねません。
 */
#ifndef YUI_ARGV_H
#define YUI_ARGV_H

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <stdlib.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "shell32.lib") /* CommandLineToArgvW */
#  endif

/* 取れたら UTF-8 の argv を返します（プロセスが終わるまで使う前提で、解放しません）。
   取れなければ NULL。呼ぶ側は元の argv をそのまま使ってください。 */
static char **yui_argv_utf8(int *argc) {
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

#endif /* _WIN32 */

#endif /* YUI_ARGV_H */
