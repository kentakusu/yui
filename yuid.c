/*
 * 結（yui）のデーモン（docs/デーモンの構想.md の段 1〜3）。
 *
 *   yuid ファイル [--write] [--takeover] [--port N] [--idle 秒]
 *                 [--lan <アドレス> --secret-file <パス>]
 *
 * ★何のためか: 結は「1 つのプロセスが開いて、閉じるまで持つ」形なので、
 *   同じファイルを複数のプロセスから使えません。このデーモンが 1 つだけ持ち、
 *   口（127.0.0.1 の TCP）を通して何人にも読ませます。
 *
 * ★既定は読むだけです。--write を付けると書けるようになりますが、
 *   **名乗った接続だけ**です（名乗らない接続は読むだけのまま）。
 *   書き込みが通ると、デーモンが「誰が・いつ」を押します。
 *   設計は docs/デーモンの構想.md が正です。
 *
 * ★帰属の形（段 2）
 *     結ぶ (<ラベル>:<名前>)-[書いた {時: <いま>}]->(機:<名乗り>)   ← どれも引用符で囲みます
 *   ★時刻は**線の属性**に置きます。点の属性に置くと、同じ機が次に書いたときに
 *     上書きされて消えます（実測）。
 *   ★`結ぶ` は (元, 種類, 先) が同じなら 1 本に畳むので、残るのは
 *     **「最後に書いたのは誰か・いつか」**です。**毎回の履歴は残りません。**
 *   ★名前は、返ってきた JSON の中身を**そのまま**引用符の中へ入れます。
 *     結の文の逃がし方（改行・タブ・引用符・逆斜線）は JSON と同じなので、
 *     ほどかずに往復します（実測で確認）。
 *   ★ただし 0x20 未満の文字と復帰（CR）は、JSON が u00xx 形や r 形で逃がすのに
 *     結の文はそれを読めません。**そういう名前には押さず、理由を言います。**
 *     黙って別の点に付けると、`結ぶ` が無い点を作るので静かに増えます。
 *
 * ★既定は 127.0.0.1 にしか出しません。別の筐体から使うときだけ、--lan <アドレス>
 *   で 2 つ目の口を開けます（段 3）。★LAN の口は --secret-file の合言葉が
 *   合うまで、**読むことも通しません**（理由は privacy。結のファイルには
 *   使う人の記録が丸ごと入りえるので、読めるだけで漏れます）。
 *   ★ファイアウォールは開けません。開けるのは使う人です。
 *
 * ★終了コード: 0 正常に終わった / 2 使い方の誤り /
 *   **3 ほかが先に持っている**（口かファイルの錠。あとで建て直せばよい）/
 *   1 それ以外（何回試しても同じ）
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
 * ★まだ無いもの（docs/デーモンの構想.md の段 4 と、段 3 の残り）
 *   名前付きパイプ。公平さ（1 本で順に回します）。
 *   段 3 の残り: ★TLS はありません（同じ LAN の中でも合言葉は平文で流れます）。
 *   ★総当たりの回数も数えていません（間違えた接続を切るだけです）。
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
 *   関所（high water）を超えたら、その接続の新しい問い合わせを受け付けません。
 *   切る線（max out）を超えたら、その接続を切ります（他のクライアントを守るため）。
 *
 * ★切る線を --max-out で変えられるようにしてあります。テストのためです。
 *   既定の 8 MB のままだと、**OS の緩衝がそれだけ飲み込んでしまう土台**があり
 *   （macOS で実測）、溜まりが線を超えないので検査が噛みません。
 *   小さくすれば、緩衝の大きさに関係なく確かめられます。
 */
#define STAMP_MAX 4 /* 1 文で帰属を押す相手の上限（作る=1・結ぶ=2） */
#define DEFAULT_HIGH_WATER (1UL * 1024UL * 1024UL)
#define DEFAULT_MAX_OUT    (8UL * 1024UL * 1024UL)

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
    /*
     * ★段 2: 名乗り。NULL なら名乗っていない ＝ 読むだけです。
     *   接続ごとに持ちます（1 つのデーモンに書く人と読む人が混ざるので）。
     */
    char *me;
    int stamp; /* 1 なら帰属を押す。「印なし」と名乗れば 0（取り込み用） */
    /*
     * ★段 3: LAN の口から来た接続です。合言葉が合うまで、読むことも通しません。
     *   127.0.0.1 から来た接続は lan=0 で、合言葉は要りません
     *   （docs/デーモンの構想.md の「口」の表が正。理由は privacy で、
     *     口が開いていれば同じ LAN に居る誰でも全部 読めるからです）。
     */
    int lan;
    int ok; /* 1 なら合言葉が合った。lan=0 の接続では見ません */
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
/* ------------------------------------------------- 段 2: 帰属を押す道具 */

static char *dup_n(const char *p, size_t n) {
    char *q = (char *)malloc(n + 1);
    if (!q) {
        fputs("yuid: メモリが足りません\n", stderr);
        exit(1);
    }
    memcpy(q, p, n);
    q[n] = 0;
    return q;
}

/*
 * JSON の object の直下から key を探して、値の先頭を返します。
 * obj は { の次を指します。無ければ NULL です。
 * ★入れ子の中は見ません（属性に name という鍵があっても拾いません）。
 */
static const char *json_value_of(const char *obj, const char *key) {
    int depth = 0;
    size_t kn = strlen(key);
    const char *p = obj;
    while (*p) {
        if (*p == '"') {
            const char *ks = p + 1, *q = ks;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1]) q++;
                q++;
            }
            if (*q != '"') return NULL;
            if (depth == 0 && q[1] == ':' && (size_t)(q - ks) == kn &&
                memcmp(ks, key, kn) == 0)
                return q + 2;
            p = q + 1;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (depth == 0) return NULL; /* この object の終わりまで来ました */
            depth--;
        }
        p++;
    }
    return NULL;
}

/* 文字列の値の中身を、逃がしたまま写します */
static char *json_raw_str(const char *v) {
    const char *q;
    if (!v || *v != '"') return NULL;
    q = v + 1;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) q++;
        q++;
    }
    if (*q != '"') return NULL;
    return dup_n(v + 1, (size_t)(q - v - 1));
}

/*
 * ★そのまま結の文へ差して往復するか。
 *   結の文が読める逃がしは 改行・タブ・引用符・逆斜線 の 4 つだけです。
 *   JSON は 0x20 未満を u00xx 形、復帰を r 形で出すので、入っていたら 0 を返します。
 */
static int stamp_safe(const char *s) {
    for (; *s; s++) {
        if (*s != '\\') continue;
        if (!s[1]) return 0;
        if (s[1] != 'n' && s[1] != 't' && s[1] != '"' && s[1] != '\\') return 0;
        s++; /* 逃がした 1 文字を飛ばします */
    }
    return 1;
}

/* 生の文字列を、結の文の引用符の中に書ける形にして足します */
static void buf_stmt_str(Buf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') buf_mem(b, "\\", 1);
        buf_mem(b, s, 1);
    }
}

/* いまの時刻。RFC 3339 の形（例 2026-11-11T09:12:34+09:00）*/
static void now_iso(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char z[8];
    size_t len;
    if (!lt || strftime(buf, n, "%Y-%m-%dT%H:%M:%S", lt) == 0) {
        snprintf(buf, n, "1970-01-01T00:00:00Z");
        return;
    }
    len = strlen(buf);
    /* ★%z は +0900 の形で返るので、: を入れて RFC 3339 に合わせます */
    if (strftime(z, sizeof z, "%z", lt) == 5 && (z[0] == '+' || z[0] == '-') &&
        len + 6 < n) {
        snprintf(buf + len, n - len, "%c%c%c:%c%c", z[0], z[1], z[2], z[3], z[4]);
        return;
    }
    /*
     * ★ずれが取れないときは UTC で書きます。ずれを書かないでおくと
     *   「何時のことか」が分からない記録になります。
     */
    {
        struct tm *gt = gmtime(&t);
        if (gt && strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", gt) > 0) return;
        snprintf(buf, n, "1970-01-01T00:00:00Z");
    }
}

typedef struct {
    char *label; /* 生の JSON の中身（逃がしたまま） */
    char *name;
} Target;

typedef struct {
    Buf *out;
    long max_rows; /* 0 なら上限なし */
    long rows;
    int cut; /* 1 なら上限で打ち切った */
    /*
     * ★段 2: 帰属を押す相手。この文が出した点を控えます。
     *   読む文でも控えますが、押すのは「書き込みが通ったとき」だけなので
     *   害はありません（見分けるのは yui_writes() で、文面は見ません）。
     */
    Target tgt[STAMP_MAX];
    int ntgt;
    int max_tgt; /* ★押す相手の上限。--max-stamp で小さくできます（検査のため） */
    int tgt_over; /* 1 なら相手が多くて取りきれなかった */
} Sink;

/* 控えた相手を捨てます（1 文ごとに呼びます） */
static void tgt_clear(Sink *k) {
    int i;
    for (i = 0; i < k->ntgt; i++) {
        free(k->tgt[i].label);
        free(k->tgt[i].name);
    }
    k->ntgt = 0;
    k->tgt_over = 0;
}

/* obj（{ の次）から label と name を控えます */
static void tgt_add(Sink *k, const char *obj) {
    char *lb, *nm;
    if (k->ntgt >= k->max_tgt || k->ntgt >= STAMP_MAX) {
        k->tgt_over = 1;
        return;
    }
    lb = json_raw_str(json_value_of(obj, "label"));
    nm = json_raw_str(json_value_of(obj, "name"));
    if (!lb || !nm) {
        free(lb);
        free(nm);
        return;
    }
    k->tgt[k->ntgt].label = lb;
    k->tgt[k->ntgt].name = nm;
    k->ntgt++;
}

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
    /*
     * ★帰属を押す相手を控えます。点の行はその点、線の行は両端です。
     *   ★線そのものには押せません（結の線は点と点を結ぶものなので、
     *     線を端にできません）。だから `結ぶ` では両端の点に押します。
     */
    if (text[0] == '{') {
        const char *obj = text + 1;
        char *kd = json_raw_str(json_value_of(obj, "kind"));
        if (kd && strcmp(kd, "node") == 0) {
            tgt_add(k, obj);
        } else if (kd && strcmp(kd, "edge") == 0) {
            const char *f = json_value_of(obj, "from");
            const char *t = json_value_of(obj, "to");
            if (f && *f == '{') tgt_add(k, f + 1);
            if (t && *t == '{') tgt_add(k, t + 1);
        }
        free(kd);
    }
    return 0;
}

/* 帰属の文の返りは、相手に見せません（頼まれていないので） */
static int out_none(void *ctx, int kind, const char *text) {
    (void)ctx;
    (void)kind;
    (void)text;
    return 0;
}

/*
 * 控えた相手に「誰が・いつ」を押します。
 * 戻り値: 0 = 全部 押せた、1 = 押せなかったものがある（理由は out に足します）
 */
static int stamp_targets(yui *db, Sink *k, const char *me, Buf *out) {
    int i, bad = 0;
    char when[64];
    now_iso(when, sizeof when);
    for (i = 0; i < k->ntgt; i++) {
        Buf q;
        int rc;
        if (!stamp_safe(k->tgt[i].label) || !stamp_safe(k->tgt[i].name)) {
            /*
             * ★押しません。そのまま差すと別の点に付き、`結ぶ` は無い点を
             *   作るので静かに増えます。書き込みそのものは通っています。
             */
            buf_str(out, "{\"kind\":\"error\",\"text\":");
            buf_json_str(out,
                         "帰属を押せませんでした: 点の名前かラベルに、結の文で書けない"
                         "文字（0x20 より小さい字か 復帰）が入っています。"
                         "書き込みそのものは通っています");
            buf_str(out, "}\n");
            bad = 1;
            continue;
        }
        memset(&q, 0, sizeof q);
        buf_str(&q, "結ぶ (\"");
        buf_str(&q, k->tgt[i].label); /* ★逃がしたまま。JSON と同じ形なので往復します */
        buf_str(&q, "\":\"");
        buf_str(&q, k->tgt[i].name);
        buf_str(&q, "\")-[書いた {時: \"");
        buf_str(&q, when);
        buf_str(&q, "\"}]->(機:\"");
        buf_stmt_str(&q, me); /* ★名乗りは生の文字列なので、ここで逃がします */
        buf_str(&q, "\")");
        rc = yui_exec_json(db, q.p ? q.p : "", out_none, NULL);
        if (rc == YUI_ERROR) {
            buf_str(out, "{\"kind\":\"error\",\"text\":");
            buf_json_str(out, yui_errmsg(db));
            buf_str(out, "}\n");
            bad = 1;
        }
        free(q.p);
    }
    if (k->tgt_over) {
        buf_str(out, "{\"kind\":\"error\",\"text\":");
        buf_json_str(out, "帰属を押しきれませんでした: この文が触った点が多すぎます");
        buf_str(out, "}\n");
        bad = 1;
    }
    return bad;
}

/*
 * 名乗りの文かどうか見ます。そうなら 1 を返して、名前と印の有無を入れます。
 *   名乗る <名前>           帰属を押します
 *   名乗る <名前> 印なし    押しません（何万件も書く取り込み用）
 * <名前> は引用符で囲んでも構いません。
 *
 * ★これはデーモンだけの言葉で、結のライブラリには入れていません。
 *   言葉は開けたあとにしか打てないので、接続の性質を決めるのに向きません。
 * ★返り 1 で *name が NULL なら、形が違うということです（*why に理由）。
 */
static int parse_hello(const char *line, char **name, int *stamp, const char **why) {
    static const char *KW = "名乗る";
    const char *p = line, *e;
    size_t kn = strlen(KW), n;
    char *out;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, KW, kn) != 0) return 0;
    if (p[kn] && p[kn] != ' ' && p[kn] != '\t') return 0;
    *name = NULL;
    *stamp = 1;
    *why = NULL;
    p += kn;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    /* ★末尾の「印なし」を先に外します */
    {
        static const char *NO = "印なし";
        size_t nn = strlen(NO);
        if ((size_t)(e - p) > nn && memcmp(e - nn, NO, nn) == 0 &&
            (e[-(int)nn - 1] == ' ' || e[-(int)nn - 1] == '\t')) {
            *stamp = 0;
            e -= nn;
            while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
        }
    }
    if (e == p) {
        *why = "名乗る のあとに名前がありません";
        return 1;
    }
    /* 引用符で囲まれていたら、ほどきます */
    if (*p == '"' && e - p >= 2 && e[-1] == '"') {
        const char *q;
        Buf b;
        memset(&b, 0, sizeof b);
        for (q = p + 1; q < e - 1; q++) {
            if (*q == '\\' && q + 1 < e - 1) {
                char c = q[1];
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c != '"' && c != '\\') {
                    buf_mem(&b, q, 1);
                    continue;
                }
                buf_mem(&b, &c, 1);
                q++;
                continue;
            }
            buf_mem(&b, q, 1);
        }
        out = dup_n(b.p ? b.p : "", b.n);
        free(b.p);
    } else {
        out = dup_n(p, (size_t)(e - p));
    }
    /*
     * ★名乗りに 0x20 未満の字は入れさせません。帰属の文に書けないので、
     *   受け取ってから「押せません」と言うより、ここで断るほうが親切です。
     */
    for (n = 0; out[n]; n++) {
        if ((unsigned char)out[n] < 0x20) {
            free(out);
            *why = "名乗りに 0x20 より小さい文字は使えません";
            return 1;
        }
    }
    *name = out;
    return 1;
}

/* --------------------------------------------- 段 3: 合言葉（LAN の口だけ） */

/*
 * 合言葉を比べます。★合っているかも、長さが合っているかも、時間で漏らさない形で
 *   比べます（短絡しない）。LAN に出す口なので、総当たりの足がかりを増やしません。
 *   ★限界: 回す回数は「送られてきた側」の長さで決まるので、こちらの合言葉の
 *     長さは漏れません。送った側の長さは送った本人が知っているので、問題になりません。
 */
static int secret_same(const char *got, const char *want) {
    size_t ng = strlen(got), nw = strlen(want), i;
    unsigned char diff = (unsigned char)(ng != nw);
    for (i = 0; i < ng; i++) diff |= (unsigned char)(got[i] ^ (i < nw ? want[i] : 0));
    return diff == 0;
}

/*
 * 「合言葉 <値>」の行か。1 ならその行で、*val に値（NULL なら値が無い）。
 * ★値は語で切らず、行末まで取ります（合言葉に空白を入れてよいので）。
 *   引用符もほどきません（ほどくと、引用符自体を含む合言葉が書けません）。
 */
static int parse_secret(const char *line, char **val) {
    static const char *KW = "合言葉";
    const char *p = line, *e;
    size_t kn = strlen(KW);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, KW, kn) != 0) return 0;
    if (p[kn] && p[kn] != ' ' && p[kn] != '\t') return 0;
    *val = NULL;
    p += kn;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    /* ★末尾の復帰だけ外します（空白は合言葉の一部かもしれないので外しません） */
    while (e > p && e[-1] == '\r') e--;
    if (e == p) return 1;
    *val = dup_n(p, (size_t)(e - p));
    return 1;
}

/*
 * 合言葉のファイルを読みます。末尾の改行・復帰だけ外します
 * （★ほかの空白は外しません。合言葉の一部かもしれないので）。
 * ★中身は画面にも記録にも出しません。断る理由だけを言います。
 * 戻り値: malloc した文字列か、NULL（理由を *why に）。
 */
static char *read_secret(const char *p, const char **why) {
    FILE *f;
    char buf[4096];
    size_t n;
    char *out;
    *why = NULL;
    f = fopen(p, "rb");
    if (!f) {
        *why = "合言葉のファイルを開けません";
        return NULL;
    }
    n = fread(buf, 1, sizeof buf - 1, f);
    if (ferror(f)) {
        fclose(f);
        *why = "合言葉のファイルを読めません";
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n == 0) {
        *why = "合言葉のファイルが空です";
        return NULL;
    }
    /*
     * ★16 バイト未満は断ります。LAN に出す口なので、総当たりで当たる長さの
     *   合言葉を「設定した気になっている」状態を作らないためです。
     */
    if (n < 16) {
        *why = "合言葉が短すぎます（16 バイト以上にしてください）";
        return NULL;
    }
    out = dup_n(buf, n);
    /*
     * ★読んだ控えをここで消します。ただし**消える保証はありません**
     *   （最適化で落とされることがあります）。気休めであって、守りではありません。
     */
    memset(buf, 0, sizeof buf);
    if (!out) *why = "メモリが足りません";
    return out;
}

/*
 * 問い合わせ 1 件を実行して、返す中身を out に組みます。
 * 戻り値: 0 = 続ける、1 = この接続を切る（終わる を受けた）
 */
static int run_req(yui *db, Conn *c, const char *body, size_t blen, long max_rows,
                   int writable, int max_stamp, const char *secret) {
    size_t i = 0;
    unsigned long lineno = 0;
    int failed = 0, bye = 0;
    Buf *out = &c->out;
    Sink sink;
    memset(&sink, 0, sizeof sink);
    sink.out = out;
    sink.max_rows = max_rows;
    sink.max_tgt = max_stamp;

    while (i <= blen && !bye && !sink.cut) {
            size_t j = i;
        char *line;
        int rc;
        char *hello_name = NULL;
        char *lo_got = NULL; /* loopback に来た「合言葉 …」の値 */
        int hello_stamp = 1;
        const char *hello_why = NULL;
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
        } else if (c->lan && !c->ok) {
            /*
             * ★段 3: LAN の口は、合言葉が合うまで何も通しません（読むことも）。
             *   ここを「書くときだけ」にすると、privacy が守れません——結の
             *   ファイルには使う人の記録が丸ごと入りえるので、読めるだけで漏れます。
             */
            char *got = NULL;
            int form = parse_secret(line, &got);
            if (form && got && secret && secret_same(got, secret)) {
                c->ok = 1;
                buf_str(out, "{\"kind\":\"pass\",\"ok\":true}\n");
            } else {
                /*
                 * ★合言葉そのものは返しません・記録にも出しません。
                 * ★「送り方が違う」と「値が合わない」は言い分けます。
                 *   言い分けないと、★値を間違えた人に書式を案内することになり、
                 *   間違っていない所を読み直させます（直すべきは値なのに）。
                 *   隠す意味もありません——書式は README で公開していて、
                 *   ★ここで返す案内そのものが書式を印字しています。
                 * ★言い分けないもの: 値のどこが違うか・何回 間違えたか。
                 *   比べるのは定数時間のままです（secret_same）。
                 * ★1 本の接続で何度も試させないので、ここで切ります（bye）。
                 *   繋ぎ直させるぶんだけ、総当たりが遅くなります。
                 */
                const char *why;
                if (!form) {
                    why = "合言葉が要ります（LAN の口です）。"
                          "最初の 1 行で「合言葉 <値>」を送ってください";
                } else if (!got) {
                    why = "「合言葉」の後ろに値がありません。"
                          "「合言葉 <値>」の形で送ってください";
                } else {
                    why = "合言葉が合いません。値を確かめてください";
                }
                buf_str(out, "{\"kind\":\"error\",\"text\":");
                buf_json_str(out, why);
                buf_str(out, "}\n");
                failed = 1;
                bye = 1;
            }
            free(got);
        } else if (!c->lan && parse_secret(line, &lo_got)) {
            /*
             * ★127.0.0.1 の口は合言葉を見ません。でも「合言葉 …」が来たら受け取ります。
             *   ★使う側が「この口は合言葉が要るか」を知らないと動かないのは、良い作りでは
             *   ありません（2026-11-22: 紡が loopback に合言葉を送り、「知らない言葉です」で
             *   止まりました。★設定に合言葉を書いたせいで動かない形は、いちばん分かりにくい）。
             * ★合言葉が設定されていれば照合します。黙って通すと「確かめたつもり」になるためです。
             * ★ただし合わなくても切りません。この口はもともと合言葉を要らないので、
             *   切ると「今まで通っていた人」が止まります。文面にそう書きます。
             */
            if (!secret || (lo_got && secret_same(lo_got, secret))) {
                buf_str(out, "{\"kind\":\"pass\",\"ok\":true}\n");
            } else {
                buf_str(out, "{\"kind\":\"error\",\"text\":");
                buf_json_str(out, "合言葉が合いません。"
                                  "（★この口は 127.0.0.1 なので合言葉は要りません。このまま続けられます）");
                buf_str(out, "}\n");
                failed = 1;
            }
            free(lo_got);
        } else if (parse_hello(line, &hello_name, &hello_stamp, &hello_why)) {
            /* ★名乗りはデーモンが受け取ります。結には渡しません */
            if (!hello_name) {
                buf_str(out, "{\"kind\":\"error\",\"line\":");
                {
                    char nb[32];
                    snprintf(nb, sizeof nb, "%lu", lineno);
                    buf_str(out, nb);
                }
                buf_str(out, ",\"text\":");
                buf_json_str(out, hello_why ? hello_why : "名乗りの形が違います");
                buf_str(out, "}\n");
                failed = 1;
            } else {
                free(c->me);
                c->me = hello_name;
                c->stamp = hello_stamp;
                buf_str(out, "{\"kind\":\"hello\",\"name\":");
                buf_json_str(out, c->me);
                buf_str(out, ",\"write\":");
                buf_str(out, writable ? "true" : "false");
                buf_str(out, ",\"stamp\":");
                buf_str(out, c->stamp ? "true" : "false");
                buf_str(out, "}\n");
                /*
                 * ★デーモンが --write 無しで立っているなら、名乗っても読むだけです。
                 *   黙って読むだけにすると「書いたつもり」が生まれるので、
                 *   write:false を返して相手に分かるようにしています。
                 */
            }
        } else {
            unsigned long w0;
            /*
             * ★相手ごとに書けるかを切り替えます。名乗っていない接続は読むだけです。
             *   動詞の一覧はこちらに持ちません（結の表が正。増えた日にズレます）。
             */
            if (writable && yui_set_readonly(db, (c->me ? 0 : 1)) != YUI_OK) {
                buf_str(out, "{\"kind\":\"error\",\"text\":");
                buf_json_str(out, yui_errmsg(db));
                buf_str(out, "}\n");
                failed = 1;
                free(line);
                break;
            }
            tgt_clear(&sink);
            w0 = yui_writes(db);
            rc = yui_exec_json(db, line, out_row, &sink);
            /*
             * ★書き込みが通ったときだけ帰属を押します。見分けるのは
             *   yui_writes() の増減で、締めの行の文面は見ません。
             */
            if (yui_writes(db) > w0 && c->me && c->stamp) {
                if (stamp_targets(db, &sink, c->me, out)) failed = 1;
            }
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
    tgt_clear(&sink);
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
            "使い方: %s ファイル [--write] [--takeover] [--port N] [--idle 秒] [--max-rows N]\n"
            "  127.0.0.1 で待ち受けて、問い合わせに答えます。\n"
            "  --write     書けるようにします（既定は読むだけ）。\n"
            "              ★書けるのは「名乗る <名前>」を送った接続だけです。\n"
            "              ★書き込みが通ると、デーモンが「誰が・いつ」を押します\n"
            "              （結ぶ (点)-[書いた {時}]->(機:名乗り)）。\n"
            "              押さずに書くなら「名乗る <名前> 印なし」。\n"
            "  --max-stamp 1 つの文で帰属を押す相手の上限（既定 %d・検査のため）\n"
            "  --takeover  残骸の錠を片付けて建て直します（常駐させるとき）。\n"
            "              ★片付けるのは「この機械・この土台が書いた錠で、pid が居ない」\n"
            "              ときだけ。生きている錠・別の機械の錠・別の土台（Windows と\n"
            "              WSL など）の錠・os= が無い錠は片付けません（3 で断ります）。\n"
            "  --lan       127.0.0.1 のほかに、このアドレスでも待ちます（段 3）。\n"
            "              ★--secret-file が無いと立ちません（後ろに何も居ない穴を\n"
            "              開けて忘れるのを防ぐため）。0.0.0.0 は受け付けません\n"
            "              （127.0.0.1 の口と重なります）。例 --lan 192.168.1.23\n"
            "  --secret-file 合言葉を書いた 1 ファイル（★リポジトリに入れない所に置く）。\n"
            "              LAN の口から来た接続は、最初の 1 行で「合言葉 <値>」を\n"
            "              送るまで、読むことも通しません。\n"
            "              ★中身は画面にも記録にも出しません。16 バイト未満は断ります\n"
            "  --port      既定 %d\n"
            "  --idle      誰も繋いでいない状態がこの秒数 続いたら終わります（既定 0＝終わらない）\n"
            "  --max-rows  1 回の問い合わせで返す行の上限（既定 %d・0 で外す）\n"
            "              ★超えたら打ち切り、end に cut を付けて言います\n"
            "  --max-out   受け取らない接続を切るまでの溜まり（バイト・既定 %lu）\n",
            me, STAMP_MAX, DEFAULT_PORT, DEFAULT_MAX_ROWS, (unsigned long)DEFAULT_MAX_OUT);
}

/*
 * 錠で断ったときの助言。★見立てごとに別の文にします。
 *   どの見立てでも「--takeover を付けて」と言うと、付けても断られた人が
 *   **錠のファイルを手で消す**ほうへ進みます。そこがいちばん危ない道です
 *   （向こうで生きている持ち主の錠が消え、2 つが同じファイルに書きます）。
 */
static void lock_advice(int look, int takeover, const char *path) {
    char body[1024];
    if (look == YUI_LOOK_STALE) {
        /* ★ここだけが --takeover で片付く見立てです */
        if (!takeover)
            fputs("yuid: ★残骸かもしれません。常駐させるなら --takeover を"
                  "付けてください（生きている錠は片付けません）\n", stderr);
        return;
    }
    if (look == YUI_LOOK_LIVE) {
        fputs("yuid: ★持ち主がまだ居ます。--takeover でも片付きません。"
              "先に持ち主を止めてください（下の pid）\n", stderr);
    } else if (look == YUI_LOOK_OTHER) {
        fputs("yuid: ★この錠は別の機械か別の土台（Windows と WSL など）のものです。"
              "--takeover でも片付きません\n"
              "yuid: ★手で消さないでください。向こうで生きているかもしれず、"
              "ここからは測れません。止めるのは持ち主の側です\n", stderr);
    } else if (look == YUI_LOOK_OLD) {
        fputs("yuid: ★この錠には os= が在りません（os= を書く前の版が書いた錠です）。"
              "どの土台のものか分からないので片付けません\n"
              "yuid: ★持ち主が居ないことを確かめてから、錠のファイルを消してください\n",
              stderr);
    } else {
        return;
    }
    /* ★誰のものかを出します。消すかを決めるのは人なので、材料を渡します */
    if (yui_lock_owner(path, body, sizeof body) == YUI_OK && body[0]) fputs(body, stderr);
}

int main(int argc, char **argv) {
    yui *db = NULL;
    Conn conns[MAX_CONN];
    sock_t lis = SOCK_BAD;
    struct sockaddr_in addr;
    int i, li, port = DEFAULT_PORT, idle = 0, nconn = 0, rc = 0, yes = 1;
    long max_rows = DEFAULT_MAX_ROWS;
    int writable = 0; /* --write で 1。既定は読むだけです */
    int takeover = 0; /* --takeover で 1。残骸の錠を片付けます */
    /*
     * ★押す相手の上限。既定のままでは、いまの言葉では超えません
     *   （作る=1・結ぶ=2）。小さくできるようにしてあるのは検査のためで、
     *   「押しきれなかったときに黙らないこと」を確かめる道がほかにありません。
     */
    int max_stamp = STAMP_MAX;
    unsigned long max_out = DEFAULT_MAX_OUT, high_water = DEFAULT_HIGH_WATER;
    const char *path = NULL;
    time_t last = time(NULL);
    char name[64];
    /* ★段 3: LAN の口。既定は閉じています（--lan と --secret-file の両方で開きます） */
    sock_t lan_lis = SOCK_BAD;
    const char *lan_addr = NULL;
    const char *secret_path = NULL;
    char *secret = NULL;

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
        } else if (strcmp(argv[i], "--write") == 0) {
            writable = 1;
        } else if (strcmp(argv[i], "--takeover") == 0) {
            takeover = 1;
        } else if (strcmp(argv[i], "--max-stamp") == 0 && i + 1 < argc) {
            max_stamp = atoi(argv[++i]);
            if (max_stamp < 1) max_stamp = 1;
        } else if (strcmp(argv[i], "--max-out") == 0 && i + 1 < argc) {
            max_out = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--lan") == 0 && i + 1 < argc) {
            lan_addr = argv[++i];
        } else if (strcmp(argv[i], "--secret-file") == 0 && i + 1 < argc) {
            secret_path = argv[++i];
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
    if (!path || port <= 0 || port > 65535 || idle < 0 || max_rows < 0 || max_out < 4096) {
        usage(argv[0]);
        return 2;
    }
    /* ★関所は切る線より小さくします（読むのをやめる前に切る形にしないため） */
    if (high_water > max_out / 2) high_water = max_out / 2;

    /*
     * ★段 3 の入口は、揃っていないときに「開かない」側へ倒します。
     *   docs/デーモンの構想.md:「後ろに何も居ない穴を開けて忘れる方が危ない」。
     */
    if (lan_addr && !secret_path) {
        fputs("yuid: --lan には --secret-file が要ります"
              "（合言葉を持たない口は開けません）\n", stderr);
        return 2;
    }
    if (secret_path && !lan_addr) {
        /* ★効かない設定を黙って受けると、「合言葉を付けた」と思い込ませます */
        fputs("yuid: --secret-file は --lan と一緒に使います"
              "（127.0.0.1 の口では合言葉を見ません）\n", stderr);
        return 2;
    }
    if (lan_addr) {
        const char *why = NULL;
        if (strcmp(lan_addr, "0.0.0.0") == 0 || strcmp(lan_addr, "*") == 0 ||
            strcmp(lan_addr, "::") == 0) {
            fprintf(stderr,
                    "yuid: --lan %s は受け付けません。"
                    "どのアドレスに出すかを書いてください"
                    "（127.0.0.1 の口と重なります）\n",
                    lan_addr);
            return 2;
        }
        /*
         * ★inet_addr ではなく inet_pton を使います。MSVC が inet_addr を
         *   deprecated として警告するので（C4996・実測）、黙らせるのではなく
         *   新しいほうに寄せます。どちらの土台にも在ります。
         */
        {
            struct in_addr tmp;
            if (inet_pton(AF_INET, lan_addr, &tmp) != 1) {
                fprintf(stderr, "yuid: --lan %s はアドレスとして読めません\n", lan_addr);
                return 2;
            }
        }
        secret = read_secret(secret_path, &why);
        if (!secret) {
            /* ★言うのは理由だけ。中身は出しません */
            fprintf(stderr, "yuid: %s: %s\n", why ? why : "合言葉を読めません", secret_path);
            return 2;
        }
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
        return 3; /* ★ほかが先に持っている。あとで建て直せばよい */
    }
    if (listen(lis, 8) != 0) {
        fputs("yuid: 待ち受けを始められません\n", stderr);
        sock_close(lis);
        return 1;
    }

    snprintf(name, sizeof name, "yuid %s port=%d%s", YUI_VERSION, port,
             writable ? " write" : "");
    /*
     * ★--write のときは YUI_READONLY を付けずに開き、そのうえで
     *   すぐ「読むだけ」に戻します。名乗った接続のときだけ外します。
     *   ★YUI_READONLY はファイルの開き方を変えず「断る」だけなので、
     *     あとから切り替えられます（yui.h に書いてあります）。
     */
    if (yui_open_locked(path, name, writable ? 0 : YUI_READONLY, &db) != YUI_OK) {
        /* ★錠で断られたときは 3。CLI と揃えます
         *   （見張る仕組みが「あとで建て直せばよい」と判できるように） */
        int busy = yui_busy(db);
        int stale = 0;
        /*
         * ★--takeover のときだけ、残骸の錠を片付けて 1 回 建て直します。
         *   常駐させると、丁寧でない止め方（タスクの終了・ログオフ・停電）の
         *   あとに錠が残り、**次の自動起動が黙って止まり続けます**。
         *   そこを人の手で消させる形にしないための口です。
         *   ★残骸と言えるのは yui_lock_stale が 1 のときだけで、
         *     生きている錠と別の機械の錠は片付けません（yui.h に条件が在ります）。
         */
        if (busy && takeover && yui_lock_stale(path, &stale) == YUI_OK && stale) {
            char was[1024];
            if (yui_lock_owner(path, was, sizeof was) != YUI_OK) was[0] = 0;
            fputs("yuid: 残骸の錠を片付けて建て直します"
                  "（この機械が書いた錠で、pid が居ません）\n", stderr);
            fputs(was, stderr);
            yui_close(db);
            db = NULL;
            if (yui_lock_clear(path) != YUI_OK) {
                fputs("yuid: 錠を片付けられませんでした\n", stderr);
                sock_close(lis);
                return 1;
            }
            if (yui_open_locked(path, name, writable ? 0 : YUI_READONLY, &db) != YUI_OK) {
                int busy2 = yui_busy(db);
                fprintf(stderr, "yuid: %s\n", yui_errmsg(db));
                yui_close(db);
                sock_close(lis);
                return busy2 ? 3 : 1;
            }
        } else {
            int look = YUI_LOOK_NONE;
            fprintf(stderr, "yuid: %s\n", yui_errmsg(db));
            /* ★助言は見立てごとに分けます（lock_advice に理由） */
            if (busy && yui_lock_look(path, &look) == YUI_OK)
                lock_advice(look, takeover, path);
            yui_close(db);
            sock_close(lis);
            return busy ? 3 : 1;
        }
    }
    if (writable && yui_set_readonly(db, 1) != YUI_OK) {
        fprintf(stderr, "yuid: %s\n", yui_errmsg(db));
        yui_close(db);
        sock_close(lis);
        return 1;
    }
    /*
     * ★段 3 の口は、ファイルの錠を取ったあとに開きます。錠で断られる筋のほうが
     *   よく起きるので、先にそこを通してから穴を開けます（開けたあとに失敗して
     *   閉じ忘れる道を作らないため）。
     */
    if (lan_addr) {
        struct sockaddr_in la;
        lan_lis = socket(AF_INET, SOCK_STREAM, 0);
        if (lan_lis == SOCK_BAD) {
            fputs("yuid: LAN の口を作れません\n", stderr);
            yui_close(db);
            sock_close(lis);
            return 1;
        }
#ifdef _WIN32
        (void)setsockopt(lan_lis, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes,
                         sizeof yes);
#else
        (void)setsockopt(lan_lis, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
        memset(&la, 0, sizeof la);
        la.sin_family = AF_INET;
        la.sin_port = htons((unsigned short)port);
        /* ★ここへ来る前に inet_pton で読めることを確かめてあります */
        (void)inet_pton(AF_INET, lan_addr, &la.sin_addr);
        if (bind(lan_lis, (struct sockaddr *)&la, (int)sizeof la) != 0 ||
            listen(lan_lis, 8) != 0) {
            fprintf(stderr,
                    "yuid: %s:%d を使えません。そのアドレスがこの機械のものか、"
                    "もう 1 つ立っていないかを見てください\n",
                    lan_addr, port);
            sock_close(lan_lis);
            yui_close(db);
            sock_close(lis);
            return 3;
        }
    }

    if (yui_notice(db)) fprintf(stderr, "yuid: お知らせ: %s\n", yui_notice(db));
    fprintf(stderr,
            "yuid: 127.0.0.1:%d で待っています（%s・返りは %ld 行まで）: %s\n", port,
            writable ? "名乗れば書けます" : "読むだけ", max_rows, path);
    if (lan_addr)
        fprintf(stderr,
                "yuid: %s:%d でも待っています（★合言葉が要ります。"
                "ファイアウォールを開けるのは使う人です）\n",
                lan_addr, port);

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
        if (lan_lis != SOCK_BAD) {
            FD_SET(lan_lis, &rd);
#ifndef _WIN32
            if ((int)lan_lis > maxfd) maxfd = (int)lan_lis;
#endif
        }
        for (i = 0; i < nconn; i++) {
            /*
             * ★溜まりが多い接続からは、新しい問い合わせを読みません。
             *   読むと、相手が読まないまま仕事が積み増されます。
             */
            if (conns[i].out.n < high_water) FD_SET(conns[i].s, &rd);
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

        /* ★口は 2 つ（127.0.0.1 と LAN）。来た口を接続に覚えさせます（lan） */
        for (li = 0; li < 2; li++) {
            sock_t from = li ? lan_lis : lis;
            sock_t s;
            if (from == SOCK_BAD || !FD_ISSET(from, &rd)) continue;
            s = accept(from, NULL, NULL);
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
                    conns[nconn].lan = li; /* ★LAN の口から来たら 1（合言葉が要る） */
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
                while (!drop && !conns[i].bye && conns[i].out.n < high_water) {
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
                    conns[i].bye =
                        run_req(db, &conns[i], body, blen, max_rows, writable,
                                max_stamp, secret);
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
            if (!drop && conns[i].out.n > max_out) {
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
                free(conns[i].me);
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
        free(conns[i].me);
    }
    sock_close(lis);
    if (lan_lis != SOCK_BAD) sock_close(lan_lis);
    if (secret) {
        /* ★記憶から消します（消える保証はありません。read_secret に理由） */
        memset(secret, 0, strlen(secret));
        free(secret);
    }
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
