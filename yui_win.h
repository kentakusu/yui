/*
 * 結（yui）— Windows 用の受け皿
 *
 * 本体（yui.c）は POSIX の低水準 I/O で書いてあります
 * （open / read / write / lseek / ftruncate / fsync / flock / fstat）。
 * iOS と Android もその系統なので、本体は POSIX のままにして、
 * Windows のときだけこの受け皿を通します。
 *
 * ★一番大事なのは _O_BINARY です。
 *   付けないと MSVC の C ランタイムが改行を勝手に CRLF へ変換し、
 *   結のファイル（バイナリ）が壊れます。open を包んでいるのはこのためです。
 *
 * ★ファイルの大きさは 2 GB までです（MSVC の struct stat の st_size が
 *   32 ビットのため）。結は開くときに中身を全部メモリに読むので、
 *   そもそもそれ以上は現実的ではありません。v0 ではこのままにします。
 *
 * ★ロック解除（LOCK_UN）は作っていません。本体が使っておらず、
 *   閉じるときに自動で外れるためです。
 */
#ifndef YUI_WIN_H
#define YUI_WIN_H
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/*
 * ★windows.h は 16 ビット時代の名残で near と far を「空のマクロ」として
 *   定義します。そのままだと本体の `Str near = {0};` が `Str  = {0};` に
 *   化けてコンパイルが通りません。今は誰も使わない名前なので打ち消します。
 */
#undef near
#undef far

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h> /* _getpid */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* POSIX の ssize_t。MSVC には無いので作ります。 */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long long ssize_t;
#endif

/* flock の種類。本体が使うのはこの 2 つだけです。 */
#define LOCK_EX 2
#define LOCK_NB 4

/*
 * open の代わり。_O_BINARY を必ず足します（上の★）。
 * mode（0644 など）は Windows には読み書きの区別しか無いので、
 * 書き込みビットが立っていれば読み書き可として渡します。
 *
 * ★パスは UTF-16 に直してから開きます。
 *   Windows の narrow な _open はパスを ANSI（日本語環境では cp932）と
 *   解釈するので、UTF-8 のまま渡すと失敗します。
 *   実測（cp932 の PC・測ったパスは「x.断片」）: UTF-8 のまま _open すると
 *   fd=-1 errno=-1、_wopen は成功。日本語を含むパスはこれが無いと開けません
 *   （「知識.yui」も同じです）。
 *
 * ★UTF-8 として読めないパスは、昔どおり ANSI として開きます。
 *   CLI は argv を UTF-8 に直してから渡しますが（yui_cli.c の★）、
 *   ライブラリとして組み込む側が ANSI のパスを渡すことはあるためです。
 */
static wchar_t *yui_win_wide(const char *path) {
    wchar_t *w;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static int yui_win_open(const char *path, int flags, int mode) {
    int pmode = (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD;
    wchar_t *w = yui_win_wide(path);
    int fd;
    flags |= _O_BINARY;
    if (!w) return _open(path, flags, pmode);
    fd = _wopen(w, flags, pmode);
    free(w);
    return fd;
}

/*
 * rename の代わり。MSVC の rename は行き先が在ると失敗するので、
 * 上書きする MoveFileExW を使います（POSIX の rename に合わせます）。
 * ★行き先を開いたままでは入れ替えられません（Windows はファイルを掴みます）。
 *   詰め直す は、入れ替える前に自分の fd を閉じています。
 * ★失敗の本当の理由は GetLastError にしか無いので、errno には
 *   EACCES（「Permission denied」）を入れます。実際いちばん多いのが
 *   「ほかで開かれている」なので、文面としても近い値を選びました。
 */
static int yui_win_rename(const char *from, const char *to) {
    wchar_t *wf = yui_win_wide(from), *wt = yui_win_wide(to);
    int rc = -1;
    if (wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        rc = 0;
    else
        errno = EACCES;
    free(wf);
    free(wt);
    return rc;
}

/* remove の代わり。パスを UTF-16 に直します（上の★）。 */
static int yui_win_remove(const char *path) {
    wchar_t *w = yui_win_wide(path);
    int rc;
    if (!w) return _unlink(path);
    rc = _wunlink(w);
    free(w);
    return rc;
}

/* read の代わり。size_t を Windows の unsigned int 幅に収めます。 */
static ssize_t yui_win_read(int fd, void *buf, size_t n) {
    unsigned int m = (n > 0x7fffffffu) ? 0x7fffffffu : (unsigned int)n;
    return (ssize_t)_read(fd, buf, m);
}

/* write の代わり。本体は書けた分だけ進める作りなので、分割されても平気です。 */
static ssize_t yui_win_write(int fd, const void *buf, size_t n) {
    unsigned int m = (n > 0x7fffffffu) ? 0x7fffffffu : (unsigned int)n;
    return (ssize_t)_write(fd, buf, m);
}

/*
 * ftruncate の代わり。_chsize_s は失敗を戻り値で返すので、
 * POSIX と同じ「-1 を返して errno に入れる」形に直します。
 */
static int yui_win_ftruncate(int fd, long long len) {
    errno_t e = _chsize_s(fd, (__int64)len);
    if (e != 0) {
        errno = e;
        return -1;
    }
    return 0;
}

/*
 * flock の代わり。Windows にはファイル全体を 1 つ押さえる仕組みが無いので、
 * 先頭から取れる限りの範囲（0xFFFFFFFFFFFFFFFF）に排他ロックをかけます。
 * LOCK_NB のときは待たずにすぐ失敗させます。
 */
static int yui_win_flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (op & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    memset(&ov, 0, sizeof ov);
    if (!LockFileEx(h, flags, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, &ov)) {
        errno = EWOULDBLOCK;
        return -1;
    }
    return 0;
}

/*
 * 本体の呼び出しをそのまま使えるように名前を合わせます。
 * ここより上で標準ヘッダを読み終えているので、ヘッダの宣言を
 * 置き換えてしまう心配はありません。
 */
#define open      yui_win_open
#define rename    yui_win_rename
#define remove    yui_win_remove
#define read      yui_win_read
#define write     yui_win_write
#define ftruncate yui_win_ftruncate
#define flock     yui_win_flock
#define fsync     _commit
#define lseek     _lseeki64
#define close     _close
#define isatty    _isatty
#define getpid    _getpid
/*
 * fstat と struct stat は MSVC の <sys/stat.h> がそのまま用意してくれるので、
 * ここでは置き換えません（_fstat に替えると struct の型が合わず警告になります）。
 */

#endif /* _WIN32 */
#endif /* YUI_WIN_H */
