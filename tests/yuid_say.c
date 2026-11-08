/*
 * yuid（デーモン）に話しかけるだけの道具。テスト専用です。
 *
 *   yuid_say <ポート> <問い合わせ> [<問い合わせ> …]   同じ接続で順に投げる
 *   yuid_say <ポート> --file <パス>                   中身を 1 件として投げる
 *   yuid_say <ポート> --raw <パス>                    長さの行ごと そのまま送る
 *   yuid_say <ポート> --deaf <秒> <問い合わせ>        ★投げたまま読まずに居座る
 *
 * 返ってきたものをそのまま標準出力へ出します。
 *   終了コード 0 = 締めの行（end）まで受け取れた
 *              1 = 受け取れなかった（切れた・時間切れ）
 *              2 = 使い方が違う
 *
 * ★なぜ C で書くか: sh から TCP を叩く道具（nc）は土台でフラグが違い、
 *   「手元では通るが別の土台で通らない」を作ります（BSD と GNU の script で
 *   実際に踏みました。tests/README.md）。これなら POSIX と Windows で
 *   同じものを使えるので、同じ検査を 2 回 書かずに済みます。
 * ★--raw は、長さの行がおかしいときの振る舞いを試すためにあります。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* nanosleep */

#include "../yui_argv.h" /* ★日本語の問い合わせを渡すため。詳しくは yui_argv.h */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET sock_t;
#  define SOCK_BAD   INVALID_SOCKET
#  define sock_close closesocket
#  define LEN_T      int
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_BAD   (-1)
#  define sock_close close
#  define LEN_T      size_t
#endif

/* ★「読まない相手」を作るために居座ります。デーモンが詰まらないかを見る用 */
static void nap_s(int sec) {
#ifdef _WIN32
    Sleep((DWORD)sec * 1000);
#else
    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = 0;
    (void)nanosleep(&ts, NULL);
#endif
}

static int send_all(sock_t s, const char *p, size_t n) {
    while (n) {
        int k = (int)send(s, p, (LEN_T)n, 0);
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* 締めの行が来るまで読んで、そのまま出します。0 = 来た、1 = 来なかった */
static int read_until_end(sock_t s) {
    char buf[4096];
    char seen[256];
    size_t sn = 0;
    for (;;) {
        int got = (int)recv(s, buf, (LEN_T)sizeof buf, 0);
        int i;
        if (got <= 0) return 1;
        fwrite(buf, 1, (size_t)got, stdout);
        fflush(stdout);
        /* 締めの行は "kind":"end" を含む行です。跨って届くので窓で見ます */
        for (i = 0; i < got; i++) {
            if (sn + 1 >= sizeof seen) {
                memmove(seen, seen + 1, sn - 1);
                sn--;
            }
            seen[sn++] = buf[i];
            seen[sn] = 0;
            if (buf[i] == '\n' && strstr(seen, "\"kind\":\"end\"")) return 0;
            if (buf[i] == '\n') sn = 0;
        }
    }
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *p;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    p = (char *)malloc((size_t)n + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    p[n] = 0;
    *len = (size_t)n;
    return p;
}

int main(int argc, char **argv) {
    sock_t s;
    struct sockaddr_in a;
    int port, i, rc = 0;

#ifdef _WIN32
    {
        /* ★Windows の argv は cp932 で来ます。直さないと日本語の問い合わせが
         *   「問い合わせが UTF-8 として正しくありません」で弾かれます（実測） */
        int n = 0;
        char **a = yui_argv_utf8(&n);
        if (a) {
            argc = n;
            argv = a;
        }
    }
#endif
    if (argc < 3) {
        fprintf(stderr, "使い方: %s <ポート> <問い合わせ>… | --file <パス> | --raw <パス>\n",
                argv[0]);
        return 2;
    }
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) return 2;

#ifdef _WIN32
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 1;
    }
#endif
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) return 1;
    /*
     * ★--deaf のときは、受け取る側の緩衝を小さくします。★connect より先に
     *   やらないと効きません。これが無いと、loopback の緩衝が 1 MB 以上を
     *   飲み込んでしまい、**デーモンが「待って送る」作りに戻っていても詰まらない**
     *   ので、検査に検出力がありません（実測で分かりました）。
     *   ここは試験用の道具なので、小さくして構いません。
     */
    if (strcmp(argv[2], "--deaf") == 0) {
        int small = 2048;
#ifdef _WIN32
        (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&small, sizeof small);
#else
        (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
#endif
    }
    {
#ifdef _WIN32
        DWORD ms = 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001u); /* ★INADDR_LOOPBACK は macOS で隠れます */
    if (connect(s, (struct sockaddr *)&a, (int)sizeof a) != 0) {
        fprintf(stderr, "繋がりません: 127.0.0.1:%d\n", port);
        sock_close(s);
        return 1;
    }

    if (strcmp(argv[2], "--deaf") == 0) {
        /* ★投げるだけ投げて、読まずに居座ります */
        int sec, j;
        if (argc < 5) {
            sock_close(s);
            return 2;
        }
        sec = atoi(argv[3]);
        for (j = 4; j < argc; j++) {
            char head[32];
            size_t len = strlen(argv[j]);
            snprintf(head, sizeof head, "%lu\n", (unsigned long)len);
            if (send_all(s, head, strlen(head)) != 0 || send_all(s, argv[j], len) != 0) {
                rc = 1;
                break;
            }
        }
        nap_s(sec > 0 ? sec : 1);
        sock_close(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return rc;
    }

    if (strcmp(argv[2], "--raw") == 0 || strcmp(argv[2], "--file") == 0) {
        size_t len = 0;
        char *body;
        if (argc < 4) {
            sock_close(s);
            return 2;
        }
        body = slurp(argv[3], &len);
        if (!body) {
            fprintf(stderr, "読めません: %s\n", argv[3]);
            sock_close(s);
            return 1;
        }
        if (strcmp(argv[2], "--raw") == 0) {
            if (send_all(s, body, len) != 0) rc = 1;
        } else {
            char head[32];
            snprintf(head, sizeof head, "%lu\n", (unsigned long)len);
            if (send_all(s, head, strlen(head)) != 0 || send_all(s, body, len) != 0) rc = 1;
        }
        free(body);
        if (rc == 0) rc = read_until_end(s);
    } else {
        for (i = 2; i < argc; i++) {
            char head[32];
            size_t len = strlen(argv[i]);
            snprintf(head, sizeof head, "%lu\n", (unsigned long)len);
            if (send_all(s, head, strlen(head)) != 0 || send_all(s, argv[i], len) != 0) {
                rc = 1;
                break;
            }
            rc = read_until_end(s);
            if (rc != 0) break;
        }
    }
    sock_close(s);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
