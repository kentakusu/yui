/*
 * 結（yui）のデーモン — 段 1（読むだけ）。
 *
 *   yuid ファイル [--port N] [--idle 秒]
 *
 * ★何のためか: 結は「1 つのプロセスが開いて、閉じるまで持つ」形なので、
 *   同じファイルを複数のプロセスから使えません。このデーモンが 1 つだけ持ち、
 *   口（127.0.0.1 の TCP）を通して何人にも読ませます。
 *
 * ★段 1 は読むだけです（YUI_READONLY）。書けるようにするのは段 2 で、そのとき
 *   「誰が・いつ」をデーモンが押します。設計は docs/デーモンの構想.md が正です。
 *
 * ★127.0.0.1 にしか出しません。LAN へ出すのは段 3 で、合言葉が要ります。
 *   `networkingMode=Mirrored` の環境では 127.0.0.1 が Windows と WSL2 で
 *   共有されるので、これだけで両方から使えます（既定の NAT では共有されません）。
 *
 * やりとり（1 往復）
 *   送る:   <バイト数>\n<問い合わせ>
 *   返る:   1 行 1 つの JSON …
 *           {"kind":"end","rc":0}
 *
 * ★締めの行（end）が要るのは、結の出力に「必ず最後に出る行」が無いからです
 *   （`手引き` は summary を出さず、`終わる` は何も出しません）。
 * ★問い合わせは CLI と同じく 1 行ずつ実行します。渡された塊をそのまま
 *   yui_exec へ渡すと、引用符の中の生の改行の扱いが CLI と変わります。
 * ★`終わる` はこの接続を切るだけです。デーモンは止まりません（止めるのは
 *   Ctrl-C か、--idle の空き時間切れ）。
 *
 * ★段 1 で見ていないこと（docs/デーモンの構想.md の段 4）
 *   返りの上限がありません。長い返りは長いまま返ります。
 *   書き出しは待つ作りです。**読まないクライアントが 1 つ居ると、全員が止まります。**
 */
#define _POSIX_C_SOURCE 200809L
#include "yui.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "yui_argv.h" /* ★Windows のヘッダより先。詳しくは yui_argv.h */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET sock_t;
#  define SOCK_BAD   INVALID_SOCKET
#  define sock_close closesocket
#  define SEND_LEN   int
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_BAD   (-1)
#  define sock_close close
#  define SEND_LEN   size_t
#endif

#define DEFAULT_PORT 39200
#define MAX_CONN     16                      /* 同時に繋げる数 */
#define MAX_REQ      (4UL * 1024UL * 1024UL) /* 1 回の問い合わせの上限 */
#define CHUNK        4096
#define DEFAULT_MAX_ROWS 10000 /* 1 回の問い合わせで返す行の上限（0 で外せます） */
/*
 * ★読まないクライアントへの備え。待たずに送る作りにしても、相手が読まなければ
 *   こちらの溜まりが増え続けます。だから 2 段で止めます。
 *   HIGH_WATER を超えたら、その接続の新しい問い合わせを受け付けません。
 *   MAX_OUT を超えたら、その接続を切ります（他のクライアントを守るため）。
 */
#define HIGH_WATER (1UL * 1024UL * 1024UL)
#define MAX_OUT    (8UL * 1024UL * 1024UL)

/* ★Ctrl-C で落ちると錠が残ります。合図を受けて、閉じてから終わります */
static volatile sig_atomic_t stop_now = 0;
static void on_signal(int sig) {
    (void)sig;
    stop_now = 1;
}

/* ------------------------------------------------------------- 伸びる袋 */

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void buf_mem(Buf *b, const void *d, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t want = (b->n + n + 1) * 2;
        char *q = (char *)realloc(b->p, want);
        if (!q) {
            fputs("yuid: メモリが足りません\n", stderr);
            exit(1);
        }
        b->p = q;
        b->cap = want;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_mem(b, s, strlen(s)); }
static void buf_drop(Buf *b, size_t n) {
    if (n >= b->n) {
        b->n = 0;
    } else {
        memmove(b->p, b->p + n, b->n - n);
        b->n -= n;
    }
    if (b->p) b->p[b->n] = 0;
}

/* ------------------------------------------------------------ つなぎ先 */

typedef struct {
    sock_t s;
    Buf in;  /* まだ 1 件に足りていないぶん */
    Buf out; /* まだ送れていないぶん */
    /*
     * ★「終わる を受けた」「形が違った」を接続に持たせます。回ごとの変数だと、
     *   送り残しがある接続が切られずに残ります（溜まりを送り終えてから切るので）。
     */
    int bye;
} Conn;

/*
 * ★待たずに送ります。読まないクライアントが居ても、ここで止まりません。
 *   0 = 進んだ（残りは out に溜まる）、-1 = 切れた
 */
static int send_some(Conn *c) {
    while (c->out.n) {
        SEND_LEN want = (SEND_LEN)(c->out.n > 65536 ? 65536 : c->out.n);
        int k = (int)send(c->s, c->out.p, want, 0);
        if (k > 0) {
            buf_drop(&c->out, (size_t)k);
            continue;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
#else
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
        return -1;
    }
    return 0;
}

/* 待たずに送る口にします */
static void set_nonblock(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    (void)ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* 文字列を JSON の値として袋へ（引用符ごと） */
static void buf_json_str(Buf *b, const char *text) {
    buf_str(b, "\"");
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == 92) {
            char two[2];
            two[0] = 92;
            two[1] = (char)c;
            buf_mem(b, two, 2);
        } else if (c == '\n') {
            buf_str(b, "\\n");
        } else if (c == '\r') {
            buf_str(b, "\\r");
        } else if (c == '\t') {
            buf_str(b, "\\t");
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            buf_str(b, esc);
        } else {
            buf_mem(b, text, 1);
        }
    }
    buf_str(b, "\"");
}

/*
 * 返す行の受け皿。★上限（max_rows）を超えたら打ち切ります。
 *   上限が無いと、1 回の 探す が 10 万行 返したときに、その接続の
 *   溜まりだけでメモリを食い、他のクライアントも待たされます。
 */
typedef struct {
    Buf *out;
    long max_rows; /* 0 なら上限なし */
    long rows;
    int cut; /* 1 なら上限で打ち切った */
} Sink;

static int out_row(void *ctx, int kind, const char *text) {
    Sink *k = (Sink *)ctx;
    (void)kind;
    if (k->max_rows > 0 && k->rows >= k->max_rows) {
        k->cut = 1;
        /*
         * ★0 以外を返すと、この yui_exec_json に渡した残りの文も実行されません
         *   （yui.h の★）。1 行ずつ渡しているので、止まるのはこの行の中だけです。
         *   そのうえで、この問い合わせの残りの行も進めません（下で見ます）。
         */
        return 1;
    }
    buf_str(k->out, text);
    buf_str(k->out, "\n");
    k->rows++;
    return 0;
}

/*
 * 問い合わせ 1 件を実行して、返す中身を out に組みます。
 * 戻り値: 0 = 続ける、1 = この接続を切る（終わる を受けた）
 */
static int run_req(yui *db, const char *body, size_t blen, Buf *out, long max_rows) {
    size_t i = 0;
    unsigned long lineno = 0;
    int failed = 0, bye = 0;
    Sink sink;
    sink.out = out;
    sink.max_rows = max_rows;
    sink.rows = 0;
    sink.cut = 0;

    while (i <= blen && !bye && !sink.cut) {
        size_t j = i;
        char *line;
        int rc;
        while (j < blen && body[j] != '\n') j++;
        if (i == blen && blen > 0) break; /* 末尾の改行のあと */
        line = (char *)malloc(j - i + 1);
        if (!line) {
            fputs("yuid: メモリが足りません\n", stderr);
            exit(1);
        }
        memcpy(line, body + i, j - i);
        line[j - i] = 0;
        lineno++;
        /*
         * ★NUL が入っていたら、ここで止めて位置を言います。yui_exec は
         *   C の文字列を取るので、渡しても NUL までしか見てもらえません。
         *   どこに NUL があるかを知っているのは、バイト数で読んだここだけです。
         *   CLI（yui_cli.c）が同じことをしています。
         */
        if (strlen(line) != j - i) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "%lu バイト目に NUL（文字コード 0）が入っています。"
                     "問い合わせに NUL は書けません",
                     (unsigned long)strlen(line) + 1);
            buf_str(out, "{\"kind\":\"error\",\"line\":");
            {
                char nb[32];
                snprintf(nb, sizeof nb, "%lu", lineno);
                buf_str(out, nb);
            }
            buf_str(out, ",\"text\":");
            buf_json_str(out, msg);
            buf_str(out, "}\n");
            failed = 1;
        } else {
            rc = yui_exec_json(db, line, out_row, &sink);
            if (rc == YUI_ERROR) {
                char nb[32];
                buf_str(out, "{\"kind\":\"error\",\"line\":");
                snprintf(nb, sizeof nb, "%lu", lineno);
                buf_str(out, nb);
                buf_str(out, ",\"text\":");
                buf_json_str(out, yui_errmsg(db));
                buf_str(out, "}\n");
                failed = 1;
                /* ★止めません。次の行へ進みます（CLI と同じ）。
                 *   300 行 流して 1 行 間違っていたときに、残りを全部 捨てる
                 *   ほうが困るためです。どの行かは line で分かります。 */
            } else if (rc == YUI_END) {
                bye = 1; /* ★この接続を切るだけ。デーモンは止めません */
            }
        }
        free(line);
        if (j >= blen) break;
        i = j + 1;
    }

    /*
     * ★上限で打ち切ったことは、必ず言います。黙って切ると、
     *   受け取った側は「これで全部」と読みます（気づけない欠配）。
     */
    if (sink.cut) {
        char msg[200];
        snprintf(msg, sizeof msg,
                 "返りが %ld 行を超えたので打ち切りました。"
                 "この問い合わせの残りの行も実行していません。"
                 "「新しい N」で絞ってください",
                 max_rows);
        buf_str(out, "{\"kind\":\"error\",\"text\":");
        buf_json_str(out, msg);
        buf_str(out, "}\n");
        failed = 1;
    }
    buf_str(out, failed ? "{\"kind\":\"end\",\"rc\":1" : "{\"kind\":\"end\",\"rc\":0");
    if (bye) buf_str(out, ",\"bye\":true");
    if (sink.cut) buf_str(out, ",\"cut\":true");
    buf_str(out, "}\n");
    return bye;
}

/*
 * 溜まったバイトから問い合わせ 1 件を取り出します。
 * 戻り値: 1 = 取れた、0 = まだ足りない、-1 = 形が違う（切る）
 */
static int take_req(Conn *c, char **body, size_t *blen, const char **why) {
    size_t i = 0, len = 0;
    if (c->in.n == 0) return 0;
    while (i < c->in.n && c->in.p[i] != '\n') {
        if (i > 20) {
            *why = "長さの行が長すぎます";
            return -1;
        }
        i++;
    }
    if (i >= c->in.n) {
        if (c->in.n > 21) {
            *why = "長さの行が見つかりません";
            return -1;
        }
        return 0; /* まだ改行が来ていない */
    }
    if (i == 0) {
        *why = "長さがありません";
        return -1;
    }
    {
        size_t k;
        for (k = 0; k < i; k++) {
            if (c->in.p[k] < '0' || c->in.p[k] > '9') {
                *why = "長さが数字ではありません";
                return -1;
            }
            len = len * 10 + (size_t)(c->in.p[k] - '0');
            if (len > MAX_REQ) {
                *why = "問い合わせが大きすぎます（4 MB まで）";
                return -1;
            }
        }
    }
    if (c->in.n < i + 1 + len) return 0; /* 中身がまだ揃っていない */
    *body = c->in.p + i + 1;
    *blen = len;
    return 1;
}

/* -------------------------------------------------------------- 本体 */

static void usage(const char *me) {
    fprintf(stderr,
            "使い方: %s ファイル [--port N] [--idle 秒] [--max-rows N]\n"
            "  127.0.0.1 で待ち受けて、読むだけの問い合わせに答えます。\n"
            "  --port      既定 %d\n"
            "  --idle      誰も繋いでいない状態がこの秒数 続いたら終わります（既定 0＝終わらない）\n"
            "  --max-rows  1 回の問い合わせで返す行の上限（既定 %d・0 で外す）\n"
            "              ★超えたら打ち切り、end に cut を付けて言います\n",
            me, DEFAULT_PORT, DEFAULT_MAX_ROWS);
}

int main(int argc, char **argv) {
    yui *db = NULL;
    Conn conns[MAX_CONN];
    sock_t lis = SOCK_BAD;
    struct sockaddr_in addr;
    int i, port = DEFAULT_PORT, idle = 0, nconn = 0, rc = 0, yes = 1;
    long max_rows = DEFAULT_MAX_ROWS;
    const char *path = NULL;
    time_t last = time(NULL);
    char name[64];

#ifdef _WIN32
    {
        int n = 0;
        char **a = yui_argv_utf8(&n);
        if (a) {
            argc = n;
            argv = a; /* プロセスが終わるまで使うので、解放しません */
        }
    }
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fputs("yuid: Winsock を始められません\n", stderr);
            return 1;
        }
    }
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--idle") == 0 && i + 1 < argc) {
            idle = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-rows") == 0 && i + 1 < argc) {
            max_rows = atol(argv[++i]);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!path || port <= 0 || port > 65535 || idle < 0 || max_rows < 0) {
        usage(argv[0]);
        return 2;
    }

    memset(conns, 0, sizeof conns);
    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    /*
     * ★先に口を取ります。デーモンが 2 つ立たないことは bind が保証します。
     *   ファイルの錠を先に取っても同じ結果になりますが、口のほうが
     *   原因を名乗りやすいのでこちらを先にします。
     */
    lis = socket(AF_INET, SOCK_STREAM, 0);
    if (lis == SOCK_BAD) {
        fputs("yuid: 口を作れません\n", stderr);
        return 1;
    }
    /*
     * ★ここは土台で意味が逆になります。横展開しないでください。
     *
     *   POSIX の SO_REUSEADDR … TIME_WAIT で残っているポートを使えるようにする
     *                            だけで、生きている listen を 2 つは作れません
     *                            （それは SO_REUSEPORT）。付けないと、止めた
     *                            直後に建て直せません（実測で踏みました）。
     *   Windows の SO_REUSEADDR … **2 つ目の bind も通ってしまいます**（横取り）。
     *                            だから付けず、SO_EXCLUSIVEADDRUSE を付けます。
     */
#ifdef _WIN32
    (void)setsockopt(lis, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof yes);
#else
    (void)setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /*
     * ★127.0.0.1 だけ。INADDR_LOOPBACK を使わないのは、macOS では
     *   _POSIX_C_SOURCE を厳密に指定すると隠れるためです（CI で実測）。
     *   土台ごとの旗（_DARWIN_C_SOURCE）を増やすより、数値で書くほうが portable です。
     */
    addr.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(lis, (struct sockaddr *)&addr, (int)sizeof addr) != 0) {
        fprintf(stderr,
                "yuid: 127.0.0.1:%d を使えません。"
                "もう 1 つ立っているか、別のものが使っています\n",
                port);
        sock_close(lis);
        return 1;
    }
    if (listen(lis, 8) != 0) {
        fputs("yuid: 待ち受けを始められません\n", stderr);
        sock_close(lis);
        return 1;
    }

    snprintf(name, sizeof name, "yuid %s port=%d", YUI_VERSION, port);
    if (yui_open_locked(path, name, YUI_READONLY, &db) != YUI_OK) {
        fprintf(stderr, "yuid: %s\n", yui_errmsg(db));
        yui_close(db);
        sock_close(lis);
        return 1;
    }
    if (yui_notice(db)) fprintf(stderr, "yuid: お知らせ: %s\n", yui_notice(db));
    fprintf(stderr, "yuid: 127.0.0.1:%d で待っています（読むだけ・返りは %ld 行まで）: %s\n", port,
            max_rows, path);

    while (!stop_now) {
        fd_set rd, wr;
        struct timeval tv;
        int ready, maxfd = 0, nwr = 0;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(lis, &rd);
#ifndef _WIN32
        maxfd = (int)lis;
#endif
        for (i = 0; i < nconn; i++) {
            /*
             * ★溜まりが多い接続からは、新しい問い合わせを読みません。
             *   読むと、相手が読まないまま仕事が積み増されます。
             */
            if (conns[i].out.n < HIGH_WATER) FD_SET(conns[i].s, &rd);
            if (conns[i].out.n) {
                FD_SET(conns[i].s, &wr);
                nwr++;
            }
#ifndef _WIN32
            if ((int)conns[i].s > maxfd) maxfd = (int)conns[i].s;
#endif
        }
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        /* ★Windows の select は「空の集合」を渡せないので、必要なときだけ渡します */
        ready = select(maxfd + 1, &rd, nwr ? &wr : NULL, NULL, &tv);
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        if (ready == 0) {
            if (idle > 0 && nconn == 0 && (long)(time(NULL) - last) >= idle) {
                fprintf(stderr, "yuid: %d 秒 空いたので終わります\n", idle);
                break;
            }
            continue;
        }

        if (FD_ISSET(lis, &rd)) {
            sock_t s = accept(lis, NULL, NULL);
            if (s != SOCK_BAD) {
                if (nconn >= MAX_CONN) {
                    /* ★ここだけは待って送ります（1 行だけなので詰まりません）。
                     *   断る理由を言わずに切ると、相手は「落ちている」と読みます。 */
                    const char *full = "{\"kind\":\"error\",\"text\":"
                                       "\"つなぎ先がいっぱいです\"}\n"
                                       "{\"kind\":\"end\",\"rc\":1}\n";
                    (void)send(s, full, (SEND_LEN)strlen(full), 0);
                    sock_close(s);
                } else {
                    memset(&conns[nconn], 0, sizeof conns[nconn]);
                    conns[nconn].s = s;
                    set_nonblock(s);
                    nconn++;
                    last = time(NULL);
                }
            }
        }

        for (i = 0; i < nconn;) {
            int drop = 0;
            /* 送れる分を先に送ります（溜まりを減らしてから新しい問い合わせを読む） */
            if (FD_ISSET(conns[i].s, &wr) && send_some(&conns[i]) != 0) drop = 1;
            if (!drop && FD_ISSET(conns[i].s, &rd)) {
                char tmp[CHUNK];
                int got = (int)recv(conns[i].s, tmp, (SEND_LEN)sizeof tmp, 0);
                last = time(NULL);
                if (got > 0) {
                    buf_mem(&conns[i].in, tmp, (size_t)got);
                } else if (got == 0) {
                    drop = 1; /* 向こうが閉じた */
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK) drop = 1;
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop = 1;
#endif
                }
                /* ★溜まりが多くなったら、そこで読むのをやめます */
                while (!drop && !conns[i].bye && conns[i].out.n < HIGH_WATER) {
                    char *body = NULL;
                    size_t blen = 0, used;
                    const char *why = NULL;
                    int t = take_req(&conns[i], &body, &blen, &why);
                    if (t == 0) break;
                    if (t < 0) {
                        buf_str(&conns[i].out, "{\"kind\":\"error\",\"text\":");
                        buf_json_str(&conns[i].out, why ? why : "形が違います");
                        buf_str(&conns[i].out, "}\n{\"kind\":\"end\",\"rc\":1}\n");
                        conns[i].bye = 1; /* 形が違う相手とは、送り終えたら切ります */
                        break;
                    }
                    conns[i].bye = run_req(db, body, blen, &conns[i].out, max_rows);
                    used = (size_t)(body - conns[i].in.p) + blen;
                    buf_drop(&conns[i].in, used);
                }
                /* 組んだ分を、送れるだけ送ります */
                if (!drop && send_some(&conns[i]) != 0) drop = 1;
            }
            /*
             * ★読まない相手で溜まりが膨らんだら、その接続を切ります。
             *   切らないと、1 本のために他の全員が待たされます。
             */
            if (!drop && conns[i].out.n > MAX_OUT) {
                fprintf(stderr, "yuid: 受け取らない接続を切りました（溜まり %lu バイト）\n",
                        (unsigned long)conns[i].out.n);
                drop = 1;
            }
            /* 終わる／形の誤りのときは、送り終えてから切ります */
            if (!drop && conns[i].bye && conns[i].out.n == 0) drop = 1;
            if (drop) {
                sock_close(conns[i].s);
                free(conns[i].in.p);
                free(conns[i].out.p);
                conns[i] = conns[nconn - 1];
                nconn--;
            } else {
                i++;
            }
        }
    }

    for (i = 0; i < nconn; i++) {
        sock_close(conns[i].s);
        free(conns[i].in.p);
        free(conns[i].out.p);
    }
    sock_close(lis);
    if (yui_close(db) != YUI_OK) {
        fputs("yuid: 閉じるときの書き出しに失敗しました\n", stderr);
        rc = 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    fputs("yuid: 終わりました（錠を外しました）\n", stderr);
    return rc;
}
