/*
 * 結（yui）本体。構成は上から順に
 *   道具 → 文字の正規化 → 保存（記録の読み書き） → 索引 → 文法 → 実行 → 公開 API
 *
 * 書き込みの流れは 1 本だけです。
 *   記録を作る → ファイルへ追記 → その記録をメモリへ適用
 * 開くときも同じ「適用」を通るので、動いている状態と開き直した状態は同じになります。
 */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "yui.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#  include "yui_win.h" /* Windows 用の受け皿。POSIX の低水準 I/O を置き換えます */
#else
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <sys/random.h> /* getentropy。glibc は unistd.h に在ります */
#  endif
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* ------------------------------------------------------------------ 道具 */

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("結: メモリが足りません\n", stderr);
        abort();
    }
    return q;
}
static void *xmalloc(size_t n) { return xrealloc(NULL, n); }
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

typedef struct {
    u64 *v;
    size_t n, cap;
} Vec;

static void vec_push(Vec *a, u64 x) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->v = xrealloc(a->v, a->cap * sizeof(u64));
    }
    a->v[a->n++] = x;
}
static int cmp_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
static void vec_sort_unique(Vec *a) {
    size_t i, w = 0;
    if (a->n < 2) return;
    qsort(a->v, a->n, sizeof(u64), cmp_u64);
    for (i = 0; i < a->n; i++)
        if (w == 0 || a->v[w - 1] != a->v[i]) a->v[w++] = a->v[i];
    a->n = w;
}

/*
 * 索引が持つ文書番号の上限。
 *
 * ★索引は「どの文書にその 2〜3 文字の並びが在るか」を文書番号で持ちます。
 *   数がいちばん多いのがここなので、8 バイトではなく 4 バイトで持ちます
 *   （本文のある点が何万もあると、索引がメモリの大半を占めます。
 *   README の「何にメモリを使っているか」を見てください）。
 * ★文書番号は doc = id << 1 | 線なら 1 なので、4 バイトに収まるのは
 *   id が 2^31-1 までです。1 点に最低でも数百バイト要るので、そこへ届く前に
 *   メモリが尽きますが、黙って切り捨てると「探しても出てこない」に化けるため、
 *   点と線を作るところで断ります。
 */
#define MAX_ID 0x7fffffffu

typedef struct {
    char *p;
    size_t n, cap;
} Str;

static void str_mem(Str *s, const void *d, size_t n) {
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2;
        s->p = xrealloc(s->p, s->cap);
    }
    if (n) memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void str_add(Str *s, const char *t) { str_mem(s, t, strlen(t)); }
static void str_chr(Str *s, char c) { str_mem(s, &c, 1); }
static void str_clear(Str *s) {
    s->n = 0;
    if (s->p) s->p[0] = 0;
}

/* 文ごとに丸ごと捨てる小さな領域 */
typedef struct Block {
    struct Block *next;
    size_t used, size;
    char data[];
} Block;
typedef struct {
    Block *head;
} Arena;

static void *arena_alloc(Arena *a, size_t n) {
    void *r;
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->size) {
        size_t size = n > 4000 ? n : 4000;
        Block *b = xmalloc(offsetof(Block, data) + size + 16);
        b->next = a->head;
        b->used = 0;
        b->size = size;
        a->head = b;
    }
    /* data の先頭は 16 の倍数とは限らないので、そろえてから渡す */
    {
        uintptr_t base = (uintptr_t)(a->head->data + a->head->used);
        size_t pad = (size_t)((16 - (base & 15)) & 15);
        if (a->head->used + pad + n > a->head->size + 16) {
            a->head->used = a->head->size + 16; /* 次の呼び出しで新しいブロックへ */
            return arena_alloc(a, n);
        }
        r = a->head->data + a->head->used + pad;
        a->head->used += pad + n;
    }
    return r;
}
static char *arena_strn(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static void arena_reset(Arena *a) {
    while (a->head) {
        Block *b = a->head;
        a->head = b->next;
        free(b);
    }
}

/* 文字列 → 値（と id の並び）の表。消すことはなく、値が古くなるだけ。 */
typedef struct Entry {
    struct Entry *next;
    u32 hash, klen;
    u64 val;
    Vec list;
    char key[];
} Entry;
typedef struct {
    Entry **b;
    size_t nb, n;
} Map;

static u32 hash_bytes(const char *k, size_t n) {
    u32 h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (u8)k[i]) * 16777619u;
    return h;
}
static Entry *map_get(Map *m, const char *k, size_t n, int create) {
    u32 h = hash_bytes(k, n);
    Entry *e;
    if (!m->nb && !create) return NULL;
    if (m->nb) {
        for (e = m->b[h & (m->nb - 1)]; e; e = e->next)
            if (e->hash == h && e->klen == n && memcmp(e->key, k, n) == 0) return e;
    }
    if (!create) return NULL;
    if (m->n >= m->nb) {
        size_t nb = m->nb ? m->nb * 2 : 64, i;
        Entry **b = xmalloc(nb * sizeof(Entry *));
        memset(b, 0, nb * sizeof(Entry *));
        for (i = 0; i < m->nb; i++) {
            Entry *x = m->b[i], *nx;
            for (; x; x = nx) {
                nx = x->next;
                x->next = b[x->hash & (nb - 1)];
                b[x->hash & (nb - 1)] = x;
            }
        }
        free(m->b);
        m->b = b;
        m->nb = nb;
    }
    e = xmalloc(offsetof(Entry, key) + n + 1);
    memset(e, 0, offsetof(Entry, key));
    e->hash = h;
    e->klen = (u32)n;
    memcpy(e->key, k, n);
    e->key[n] = 0;
    e->next = m->b[h & (m->nb - 1)];
    m->b[h & (m->nb - 1)] = e;
    m->n++;
    return e;
}
static void map_free(Map *m) {
    size_t i;
    for (i = 0; i < m->nb; i++) {
        Entry *e = m->b[i], *nx;
        for (; e; e = nx) {
            nx = e->next;
            free(e->list.v);
            free(e);
        }
    }
    free(m->b);
    m->b = NULL;
    m->nb = m->n = 0;
}

/* ------------------------------------------------------------ データの形 */

enum { V_STR = 1, V_INT = 2, V_REAL = 3 };

typedef struct {
    int type;
    int64_t i;
    double d;
    char *s;
} Value;

typedef struct {
    char *key;
    Value v;
} Attr;

/*
 * 結果に属性をどう出すか。文の末尾の修飾（parse_mods）が決めます。
 *
 *   既定         all=1              全部 出す
 *   名前だけ     all=0, nkeys=0     1 つも出さない
 *   見せる a,b   all=0, keys={a,b}  並べた順にその属性だけ出す
 *
 * ★keys は文ごとの置き場（db->stmt）に取るので、解放しません。
 */
#define MAX_SHOW 16 /* 見せる に並べられる属性の数 */
typedef struct {
    int all;
    char **keys;
    size_t nkeys;
} Show;
static const Show SHOW_ALL = {1, NULL, 0};
/* 属性を 1 つも出さない指示。線の行の両端の点などに使う */
static const Show NONE = {0, NULL, 0};

/*
 * 線の行（探す）や道の行（辿る）に出てくる点の属性をどう出すか。
 *
 * ★既定では出しません。線や道が主役の行で点の属性まで並べると、
 *   1 行が長くなって形が読めなくなります。
 * ★ただし 見せる で名前を挙げたときは、その属性だけ点にも出します。
 *   「ある日のやり取りを、時刻と話者つきで並べる」が
 *   辿る 1 本で書けるようにするためです。
 * ★名前だけ のときは nkeys=0 なので、やはり何も出ません。
 */
static const Show *node_show(const Show *sh) { return sh->all ? &NONE : sh; }

typedef struct {
    char *label, *name;
    Attr *attrs;
    size_t nattrs;
    Vec out, in; /* 線の id。消えた線も残るので live を見ること */
    int live;
    /* 辿るときの作業用 */
    u32 mark, depth;
    u64 via_edge, via_node;
} Node;

typedef struct {
    u64 from, to;
    char *type;
    Attr *attrs;
    size_t nattrs;
    int live;
    u32 mark;
} Edge;

/*
 * 索引の枠 1 つ。key は 2〜3 文字の並び、中身はその並びを持つ文書番号です。
 *
 * ★2 件までは枠の中に直に持ちます（cap == 0 のとき u.few）。
 *   実測（文字をばらけさせた 2.2MB のファイル・点 104）では、
 *   並びの種類 118 万のうち **95% が 2 件以下**でした。
 *   1 つずつ外に確保すると、1 種類あたり
 *   「確保 1 回ぶんの負担（32 バイトほど）＋ 枠の丸め（4 件ぶん＝16 バイト）」
 *   が乗ります。種類が 118 万あると、それだけで 50MB を超えます。
 *   枠の中に持てば、その 95% は確保が 0 回です。
 * ★3 件以上になったら外に出します（cap > 0 のとき u.v）。
 *   どちらを使っているかは cap で分かります。読むときは slot_items を通してください。
 * ★u.few を 2 件にしたのは、枠の大きさがこれで 24 バイトに収まるからです
 *   （key 8 ＋ n 4 ＋ cap 4 ＋ 入れ物 8）。1 件にしても 24 バイトのままなので、
 *   同じ大きさで 2 件 持てます。
 */
typedef struct {
    u64 key;
    u32 n;   /* 入っている件数 */
    u32 cap; /* 0 なら u.few、1 以上なら u.v の枠の数 */
    union {
        u32 few[2];
        u32 *v;
    } u;
} GramSlot;

struct yui {
    int fd;
    char *path;
    char *lockpath; /* 土台をまたぐ錠。使っていなければ NULL */
    u64 size;
    Node *nodes;
    size_t nn, ncap;
    Edge *edges;
    size_t ne, ecap;
    Map m_key;   /* ラベル\0名前 → 点の id */
    Map m_name;  /* 名前 → 点の id の並び */
    Map m_label; /* ラベル → 点の id の並び */
    Map m_edge;  /* 起点>終点>種類 → 線の id */
    GramSlot *grams;
    size_t ngrams, gramcap;
    Arena stmt;    /* 文ごとの領域 */
    Arena scratch; /* 記録を適用するときの領域 */
    u32 gen;
    u32 crc[256];
    char *err;
    char *notice;
    yui_cb cb;
    void *ctx;
    int stopped;
    int readonly; /* 1 なら書き込む言葉を断る（YUI_READONLY で開いたとき） */
    int shared;   /* 1 なら錠を共有で取る（読む人どうしは待たない） */
    int json;     /* 1 なら 1 行 1 つの JSON で出す（yui_exec_json） */
    int in_replay;
};

static void set_err(yui *db, const char *fmt, ...) {
    va_list ap;
    int n;
    char *p;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    p = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    free(db->err);
    db->err = p;
}

/* ------------------------------------------------------- 文字の正規化 */

static size_t utf8_decode(const char *s, u32 *cp) {
    const u8 *p = (const u8 *)s;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80 && p[0] >= 0xC2) {
        *cp = ((u32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        u32 c = ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (c >= 0x800 && !(c >= 0xD800 && c <= 0xDFFF)) {
            *cp = c;
            return 3;
        }
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        u32 c = ((u32)(p[0] & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) |
                ((u32)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (c >= 0x10000 && c <= 0x10FFFF) {
            *cp = c;
            return 4;
        }
    }
    *cp = 0xFFFD;
    return 0; /* 不正。呼び出し側で 1 バイト進める */
}

static int utf8_valid(const char *s) {
    while (*s) {
        u32 cp;
        size_t n = utf8_decode(s, &cp);
        if (!n) return 0;
        s += n;
    }
    return 1;
}

/* 半角カナ U+FF66〜U+FF9D → 全角カタカナ */
static const unsigned short HALF_KANA[56] = {
    0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3,
    0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1,
    0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6,
    0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8,
    0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9,
    0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3};

/* 濁点が付くひらがな（か行・さ行・た行・は行は +1） */
static int takes_dakuten(u32 c) {
    if (c >= 0x304B && c <= 0x3062) return ((c - 0x304B) % 2) == 0;
    if (c >= 0x3064 && c <= 0x3069) return ((c - 0x3064) % 2) == 0;
    if (c >= 0x306F && c <= 0x307D) return ((c - 0x306F) % 3) == 0;
    return 0;
}
static int takes_handakuten(u32 c) {
    return c >= 0x306F && c <= 0x307D && ((c - 0x306F) % 3) == 0;
}

/*
 * 検索のための正規化。保存する文字列には触りません。
 * 全角英数→半角／英字→小文字／半角カナ→全角／カタカナ→ひらがな／
 * 分かれた濁点・半濁点の合成（か＋゛→が）
 */
static size_t fold_text(const char *s, u32 **out) {
    size_t cap = strlen(s) + 1, n = 0;
    u32 *r = xmalloc(cap * sizeof(u32));
    while (*s) {
        u32 c;
        size_t len = utf8_decode(s, &c);
        s += len ? len : 1;
        if (c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;
        else if (c == 0x3000) c = ' ';
        else if (c >= 0xFF66 && c <= 0xFF9D) c = HALF_KANA[c - 0xFF66];
        else if (c == 0xFF9E) c = 0x3099;
        else if (c == 0xFF9F) c = 0x309A;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;
        else if (c == 0x30FD || c == 0x30FE) c -= 0x60;
        if ((c == 0x3099 || c == 0x309A) && n > 0) {
            u32 p = r[n - 1];
            if (c == 0x3099 && takes_dakuten(p)) {
                r[n - 1] = p + 1;
                continue;
            }
            if (c == 0x3099 && p == 0x3046) { /* う゛→ゔ */
                r[n - 1] = 0x3094;
                continue;
            }
            if (c == 0x3099 && p == 0x309D) { /* ゝ゛→ゞ */
                r[n - 1] = 0x309E;
                continue;
            }
            if (c == 0x309A && takes_handakuten(p)) {
                r[n - 1] = p + 2;
                continue;
            }
        }
        r[n++] = c;
    }
    *out = r;
    return n;
}

/* ------------------------------------------------------------------ 索引 */

static u64 mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}
static GramSlot *gram_slot(yui *db, u64 key, int create) {
    size_t i;
    if (!db->gramcap) {
        if (!create) return NULL;
    } else {
        for (i = (size_t)mix64(key) & (db->gramcap - 1);; i = (i + 1) & (db->gramcap - 1)) {
            if (db->grams[i].key == key) return &db->grams[i];
            if (!db->grams[i].key) break;
        }
        if (!create) return NULL;
    }
    if ((db->ngrams + 1) * 10 >= db->gramcap * 7) {
        size_t nc = db->gramcap ? db->gramcap * 2 : 1024, j;
        GramSlot *ng = xmalloc(nc * sizeof(GramSlot));
        memset(ng, 0, nc * sizeof(GramSlot));
        for (j = 0; j < db->gramcap; j++) {
            if (db->grams[j].key) {
                size_t k = (size_t)mix64(db->grams[j].key) & (nc - 1);
                while (ng[k].key) k = (k + 1) & (nc - 1);
                ng[k] = db->grams[j];
            }
        }
        free(db->grams);
        db->grams = ng;
        db->gramcap = nc;
    }
    for (i = (size_t)mix64(key) & (db->gramcap - 1); db->grams[i].key;
         i = (i + 1) & (db->gramcap - 1))
        ;
    db->grams[i].key = key;
    db->ngrams++;
    return &db->grams[i];
}
/* 21 ビット × 3 に詰める。2-gram は最上位ビットで区別する */
static u64 gram3(u32 a, u32 b, u32 c) {
    return ((u64)a << 42) | ((u64)b << 21) | c;
}
static u64 gram2(u32 a, u32 b) {
    return (1ULL << 63) | ((u64)a << 21) | b;
}
/* 枠の中身。cap == 0 なら枠に直に入っている（GramSlot の★を見よ） */
static u32 *slot_items(GramSlot *g) { return g->cap ? g->u.v : g->u.few; }

static void gram_add(yui *db, u64 key, u64 doc) {
    GramSlot *g = gram_slot(db, key, 1);
    u32 d = (u32)doc; /* MAX_ID で収まることを確かめてある */
    /* 同じ文書が続いたら 1 つにまとめます（同じ並びが本文に何度も出るため） */
    if (g->n && slot_items(g)[g->n - 1] == d) return;
    if (!g->cap) {
        if (g->n < 2) {
            g->u.few[g->n++] = d;
            return;
        }
        /* 3 件目。ここで初めて外に出します */
        {
            u32 a = g->u.few[0], b = g->u.few[1], *v;
            v = xmalloc(4 * sizeof *v);
            v[0] = a;
            v[1] = b;
            v[2] = d;
            g->u.v = v;
            g->cap = 4;
            g->n = 3;
        }
        return;
    }
    if (g->n == g->cap) {
        g->cap *= 2;
        g->u.v = xrealloc(g->u.v, g->cap * sizeof *g->u.v);
    }
    g->u.v[g->n++] = d;
}
static void index_text(yui *db, u64 doc, const char *s) {
    u32 *c;
    size_t n = fold_text(s, &c), i;
    for (i = 0; i + 1 < n; i++) {
        gram_add(db, gram2(c[i], c[i + 1]), doc);
        if (i + 2 < n) gram_add(db, gram3(c[i], c[i + 1], c[i + 2]), doc);
    }
    free(c);
}

/* 文書番号: (id << 1) | 線なら 1 */
#define DOC_NODE(id) ((u64)(id) << 1)
#define DOC_EDGE(id) (((u64)(id) << 1) | 1)

static int cps_contains(const u32 *h, size_t hn, const u32 *q, size_t qn) {
    size_t i;
    if (qn > hn) return 0;
    for (i = 0; i + qn <= hn; i++)
        if (memcmp(h + i, q, qn * sizeof(u32)) == 0) return 1;
    return 0;
}
static int text_has(const char *s, const u32 *q, size_t qn) {
    u32 *c;
    size_t n = fold_text(s, &c);
    int r = cps_contains(c, n, q, qn);
    free(c);
    return r;
}
static int attrs_have(const Attr *a, size_t n, const u32 *q, size_t qn) {
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i].v.type == V_STR && text_has(a[i].v.s, q, qn)) return 1;
    return 0;
}
/* 探す対象: 点の名前と文字列の属性、線の文字列の属性 */
static int doc_has(yui *db, u64 doc, const u32 *q, size_t qn) {
    size_t id = (size_t)(doc >> 1);
    if (doc & 1) {
        Edge *e = &db->edges[id - 1];
        return e->live && attrs_have(e->attrs, e->nattrs, q, qn);
    } else {
        Node *x = &db->nodes[id - 1];
        return x->live && (text_has(x->name, q, qn) || attrs_have(x->attrs, x->nattrs, q, qn));
    }
}
/* use_index=0 は全件走査。YUI_SELFCHECK を付けたビルドでは、
 * 探すたびに索引の答えと全件走査の答えを突き合わせる（cmd_search を見よ） */
static void search_docs(yui *db, const u32 *q, size_t qn, int use_index, Vec *out) {
    size_t i;
    if (qn >= 2 && use_index) {
        GramSlot *best = NULL;
        const u32 *items;
        if (qn == 2) {
            GramSlot *g = gram_slot(db, gram2(q[0], q[1]), 0);
            if (!g) return;
            best = g;
        } else {
            for (i = 0; i + 2 < qn; i++) {
                GramSlot *g = gram_slot(db, gram3(q[i], q[i + 1], q[i + 2]), 0);
                if (!g) return; /* 1 つでも無ければ、どこにも無い */
                if (!best || g->n < best->n) best = g;
            }
        }
        items = slot_items(best);
        for (i = 0; i < best->n; i++) {
            u64 doc = items[i];
            if (doc_has(db, doc, q, qn)) vec_push(out, doc);
        }
    } else {
        for (i = 1; i <= db->nn; i++)
            if (doc_has(db, DOC_NODE(i), q, qn)) vec_push(out, DOC_NODE(i));
        for (i = 1; i <= db->ne; i++)
            if (doc_has(db, DOC_EDGE(i), q, qn)) vec_push(out, DOC_EDGE(i));
    }
    vec_sort_unique(out);
}

/* ---------------------------------------------------------------- メモリへ適用 */

/*
 * 属性を 1 つの確保にまとめて写す。
 *
 * ★古い版は属性ごとにキーと値を別々に取っていました。点 1 つに属性が 8 個
 *   あれば 16 回の確保で、1 回あたりの管理の負担（malloc が隠れて持つ分）が
 *   そのまま 16 回 乗ります。点が何万もあると、これがメモリの大きな割合に
 *   なります（README の「何にメモリを使っているか」を見てください）。
 * ★ここでは「Attr の並び ＋ 文字列の中身」を 1 つの塊として取り、
 *   キーと値はその中を指します。確保は点 1 つに 1 回です。
 * ★なので attrs_free が free するのは塊の先頭だけです。
 *   キーや値を個別に free してはいけません（塊の途中を指しています）。
 */
static Attr *attrs_dup(const Attr *a, size_t n) {
    size_t i, need = n * sizeof(Attr), len;
    char *p;
    Attr *r;
    if (!n) return NULL;
    for (i = 0; i < n; i++) {
        need += strlen(a[i].key) + 1;
        if (a[i].v.type == V_STR) need += strlen(a[i].v.s) + 1;
    }
    r = xmalloc(need);
    /* ★文字列は Attr の並びの直後に置きます。文字の列なので、
     *   並びの境目で詰め物（alignment）は要りません。 */
    p = (char *)r + n * sizeof(Attr);
    for (i = 0; i < n; i++) {
        r[i] = a[i];
        len = strlen(a[i].key) + 1;
        memcpy(p, a[i].key, len);
        r[i].key = p;
        p += len;
        if (a[i].v.type == V_STR) {
            len = strlen(a[i].v.s) + 1;
            memcpy(p, a[i].v.s, len);
            r[i].v.s = p;
            p += len;
        }
    }
    return r;
}
static void attrs_free(Attr *a, size_t n) {
    (void)n; /* 塊 1 つなので、数は要りません */
    free(a);
}
static void node_key(Str *k, const char *label, const char *name) {
    str_clear(k);
    str_add(k, label);
    str_chr(k, 0);
    str_add(k, name);
}
static void edge_key(Str *k, u64 from, u64 to, const char *type) {
    char b[64];
    str_clear(k);
    snprintf(b, sizeof b, "%llu>%llu>", (unsigned long long)from, (unsigned long long)to);
    str_add(k, b);
    str_add(k, type);
}
static void index_attrs(yui *db, u64 doc, const Attr *a, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i].v.type == V_STR) index_text(db, doc, a[i].v.s);
}

static int apply_node(yui *db, u64 id, const char *label, const char *name, const Attr *a,
                      size_t na) {
    Node *x;
    if (id == db->nn + 1) {
        Str k = {0};
        /* ★索引が文書番号を 4 バイトで持つので、ここが上限です（MAX_ID の★）。
         *   届く前にメモリが尽きますが、黙って切り捨てると
         *   「探しても出てこない」に化けるので断ります。 */
        if (id > MAX_ID) {
            set_err(db, "点が多すぎます（%llu 件まで）", (unsigned long long)MAX_ID);
            return -1;
        }
        if (db->nn == db->ncap) {
            db->ncap = db->ncap ? db->ncap * 2 : 64;
            db->nodes = xrealloc(db->nodes, db->ncap * sizeof(Node));
        }
        x = &db->nodes[db->nn++];
        memset(x, 0, sizeof *x);
        x->label = xstrdup(label);
        x->name = xstrdup(name);
        x->live = 1;
        node_key(&k, label, name);
        map_get(&db->m_key, k.p, k.n, 1)->val = id;
        free(k.p);
        vec_push(&map_get(&db->m_name, name, strlen(name), 1)->list, id);
        vec_push(&map_get(&db->m_label, label, strlen(label), 1)->list, id);
        index_text(db, DOC_NODE(id), name);
    } else if (id >= 1 && id <= db->nn && db->nodes[id - 1].live) {
        x = &db->nodes[id - 1];
        if (strcmp(x->label, label) || strcmp(x->name, name)) return -1;
        attrs_free(x->attrs, x->nattrs);
    } else {
        return -1;
    }
    x->attrs = attrs_dup(a, na);
    x->nattrs = na;
    index_attrs(db, DOC_NODE(id), a, na);
    return 0;
}

static int apply_edge(yui *db, u64 id, u64 from, u64 to, const char *type, const Attr *a,
                      size_t na) {
    Edge *e;
    if (from < 1 || from > db->nn || !db->nodes[from - 1].live) return -1;
    if (to < 1 || to > db->nn || !db->nodes[to - 1].live) return -1;
    if (id == db->ne + 1) {
        Str k = {0};
        if (id > MAX_ID) {
            set_err(db, "線が多すぎます（%llu 本まで）", (unsigned long long)MAX_ID);
            return -1;
        }
        if (db->ne == db->ecap) {
            db->ecap = db->ecap ? db->ecap * 2 : 64;
            db->edges = xrealloc(db->edges, db->ecap * sizeof(Edge));
        }
        e = &db->edges[db->ne++];
        memset(e, 0, sizeof *e);
        e->from = from;
        e->to = to;
        e->type = xstrdup(type);
        e->live = 1;
        edge_key(&k, from, to, type);
        map_get(&db->m_edge, k.p, k.n, 1)->val = id;
        free(k.p);
        vec_push(&db->nodes[from - 1].out, id);
        vec_push(&db->nodes[to - 1].in, id);
    } else if (id >= 1 && id <= db->ne && db->edges[id - 1].live) {
        e = &db->edges[id - 1];
        if (e->from != from || e->to != to || strcmp(e->type, type)) return -1;
        attrs_free(e->attrs, e->nattrs);
    } else {
        return -1;
    }
    e->attrs = attrs_dup(a, na);
    e->nattrs = na;
    index_attrs(db, DOC_EDGE(id), a, na);
    return 0;
}

static void kill_edge(yui *db, u64 id) {
    Edge *e = &db->edges[id - 1];
    e->live = 0;
    attrs_free(e->attrs, e->nattrs);
    e->attrs = NULL;
    e->nattrs = 0;
}
static int apply_del_edge(yui *db, u64 id) {
    if (id < 1 || id > db->ne || !db->edges[id - 1].live) return -1;
    kill_edge(db, id);
    return 0;
}
static int apply_del_node(yui *db, u64 id) {
    Node *x;
    size_t i;
    if (id < 1 || id > db->nn || !db->nodes[id - 1].live) return -1;
    x = &db->nodes[id - 1];
    for (i = 0; i < x->out.n; i++)
        if (db->edges[x->out.v[i] - 1].live) kill_edge(db, x->out.v[i]);
    for (i = 0; i < x->in.n; i++)
        if (db->edges[x->in.v[i] - 1].live) kill_edge(db, x->in.v[i]);
    x->live = 0;
    attrs_free(x->attrs, x->nattrs);
    x->attrs = NULL;
    x->nattrs = 0;
    return 0;
}

/* ------------------------------------------------------------- 記録の形 */

#define MAGIC_LEN 8
static const u8 MAGIC[MAGIC_LEN] = {0xE7, 0xB5, 0x90, 'y', 'u', 'i', 0, 1};
#define MAX_BODY ((u32)1 << 30)

enum { R_NODE = 1, R_EDGE = 2, R_DEL_NODE = 3, R_DEL_EDGE = 4 };

static void crc_init(yui *db) {
    u32 i;
    for (i = 0; i < 256; i++) {
        u32 c = i;
        int k;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        db->crc[i] = c;
    }
}
static u32 crc_calc(const yui *db, const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++) c = db->crc[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void put32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void enc_var(Str *s, u64 v) {
    while (v >= 0x80) {
        str_chr(s, (char)((v & 0x7F) | 0x80));
        v >>= 7;
    }
    str_chr(s, (char)v);
}
static void enc_str(Str *s, const char *t) {
    size_t n = strlen(t);
    enc_var(s, n);
    str_mem(s, t, n);
}
static void enc_attrs(Str *s, const Attr *a, size_t n) {
    size_t i;
    enc_var(s, n);
    for (i = 0; i < n; i++) {
        enc_str(s, a[i].key);
        str_chr(s, (char)a[i].v.type);
        if (a[i].v.type == V_STR) {
            enc_str(s, a[i].v.s);
        } else if (a[i].v.type == V_INT) {
            u64 u = (u64)a[i].v.i;
            enc_var(s, (u << 1) ^ (a[i].v.i < 0 ? ~(u64)0 : 0));
        } else {
            u64 bits;
            int k;
            memcpy(&bits, &a[i].v.d, 8);
            for (k = 0; k < 8; k++) str_chr(s, (char)(bits >> (8 * k)));
        }
    }
}

typedef struct {
    const u8 *p, *e;
    int bad;
    Arena *a;
} Dec;

static u64 dec_var(Dec *d) {
    u64 v = 0;
    int sh = 0;
    while (1) {
        u8 b;
        if (d->p >= d->e || sh > 63) {
            d->bad = 1;
            return 0;
        }
        b = *d->p++;
        v |= (u64)(b & 0x7F) << sh;
        if (!(b & 0x80)) return v;
        sh += 7;
    }
}
static char *dec_str(Dec *d) {
    u64 n = dec_var(d);
    char *r;
    if (d->bad || n > (u64)(d->e - d->p) || memchr(d->p, 0, (size_t)n)) {
        d->bad = 1;
        return NULL;
    }
    r = arena_strn(d->a, (const char *)d->p, (size_t)n);
    d->p += n;
    return r;
}
static Attr *dec_attrs(Dec *d, size_t *out) {
    u64 n = dec_var(d), i;
    Attr *a;
    *out = 0;
    /* 1 属性は最低でも 3 バイト。壊れた数で巨大な確保をしない */
    if (d->bad || n > (u64)(d->e - d->p) / 3) {
        d->bad = 1;
        return NULL;
    }
    a = arena_alloc(d->a, (size_t)n * sizeof(Attr) + 1);
    for (i = 0; i < n; i++) {
        a[i].key = dec_str(d);
        if (d->bad || d->p >= d->e) {
            d->bad = 1;
            return NULL;
        }
        a[i].v.type = *d->p++;
        a[i].v.s = NULL;
        a[i].v.i = 0;
        a[i].v.d = 0;
        if (a[i].v.type == V_STR) {
            a[i].v.s = dec_str(d);
        } else if (a[i].v.type == V_INT) {
            u64 z = dec_var(d);
            a[i].v.i = (int64_t)((z >> 1) ^ (~(z & 1) + 1));
        } else if (a[i].v.type == V_REAL) {
            u64 bits = 0;
            int k;
            if (d->e - d->p < 8) {
                d->bad = 1;
                return NULL;
            }
            for (k = 0; k < 8; k++) bits |= (u64)d->p[k] << (8 * k);
            d->p += 8;
            memcpy(&a[i].v.d, &bits, 8);
        } else {
            d->bad = 1;
        }
        if (d->bad) return NULL;
    }
    *out = (size_t)n;
    return a;
}

/* 記録 1 件をメモリへ。形が正しくなければ -1 */
static int apply_record(yui *db, int kind, const u8 *body, size_t n) {
    Dec d;
    int rc = -1;
    d.p = body;
    d.e = body + n;
    d.bad = 0;
    d.a = &db->scratch;
    switch (kind) {
    case R_NODE: {
        size_t na;
        u64 id = dec_var(&d);
        char *label = dec_str(&d);
        char *name = dec_str(&d);
        Attr *a = dec_attrs(&d, &na);
        if (!d.bad && d.p == d.e) rc = apply_node(db, id, label, name, a, na);
        break;
    }
    case R_EDGE: {
        size_t na;
        u64 id = dec_var(&d), from = dec_var(&d), to = dec_var(&d);
        char *type = dec_str(&d);
        Attr *a = dec_attrs(&d, &na);
        if (!d.bad && d.p == d.e) rc = apply_edge(db, id, from, to, type, a, na);
        break;
    }
    case R_DEL_NODE: {
        u64 id = dec_var(&d);
        if (!d.bad && d.p == d.e) rc = apply_del_node(db, id);
        break;
    }
    case R_DEL_EDGE: {
        u64 id = dec_var(&d);
        if (!d.bad && d.p == d.e) rc = apply_del_edge(db, id);
        break;
    }
    default:
        break;
    }
    arena_reset(&db->scratch);
    return rc;
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 追記してから適用する。書き込みが中途で失敗したら元の長さへ戻す */
static int commit_record(yui *db, int kind, const Str *body) {
    size_t blen = body->n, total = 4 + 1 + blen + 4;
    u8 *buf;
    int rc;
    if (blen >= MAX_BODY) {
        set_err(db, "データが大きすぎます（1 件 1 GiB まで）");
        return -1;
    }
    buf = xmalloc(total);
    put32(buf, (u32)blen);
    buf[4] = (u8)kind;
    if (blen) memcpy(buf + 5, body->p, blen);
    put32(buf + 5 + blen, crc_calc(db, buf, 5 + blen));
    if (write_all(db->fd, buf, total) != 0) {
        int e = errno;
        if (ftruncate(db->fd, (off_t)db->size) != 0) { /* 戻せなければ次に開くときに切り捨てる */ }
        lseek(db->fd, 0, SEEK_END);
        free(buf);
        set_err(db, "書き込みに失敗しました: %s", strerror(e));
        return -1;
    }
    db->size += total;
    rc = apply_record(db, kind, buf + 5, blen);
    free(buf);
    if (rc != 0) {
        set_err(db, "内部エラー: 書いた記録を適用できません");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 書き込み */

static int value_eq(const Value *a, const Value *b) {
    if (a->type != b->type) return 0;
    if (a->type == V_STR) return strcmp(a->s, b->s) == 0;
    if (a->type == V_INT) return a->i == b->i;
    return memcmp(&a->d, &b->d, sizeof(double)) == 0;
}

/* old に add を重ねる（同じ鍵は上書き、新しい鍵は後ろへ）。変化があれば *changed=1 */
static Attr *attrs_merge(Arena *ar, const Attr *old, size_t no, const Attr *add, size_t na,
                         size_t *out, int *changed) {
    Attr *r = arena_alloc(ar, (no + na + 1) * sizeof(Attr));
    size_t n = no, i, j;
    if (no) memcpy(r, old, no * sizeof(Attr));
    for (i = 0; i < na; i++) {
        for (j = 0; j < n; j++) {
            if (strcmp(r[j].key, add[i].key) == 0) {
                if (!value_eq(&r[j].v, &add[i].v)) *changed = 1;
                r[j].v = add[i].v;
                break;
            }
        }
        if (j == n) {
            r[n++] = add[i];
            *changed = 1;
        }
    }
    *out = n;
    return r;
}

/* 新しい点、または既存の点へ属性を重ねる。status: 0 変化なし 1 作った 2 更新 */
static int put_node(yui *db, u64 id, const char *label, const char *name, const Attr *add,
                    size_t na, u64 *out_id, int *status) {
    Str b = {0};
    size_t n = 0;
    int changed = 0, rc;
    Attr *m;
    if (id) {
        Node *x = &db->nodes[id - 1];
        m = attrs_merge(&db->stmt, x->attrs, x->nattrs, add, na, &n, &changed);
        *out_id = id;
        if (!changed) {
            *status = 0;
            return 0;
        }
        *status = 2;
    } else {
        m = attrs_merge(&db->stmt, NULL, 0, add, na, &n, &changed);
        id = db->nn + 1;
        *out_id = id;
        *status = 1;
    }
    enc_var(&b, id);
    enc_str(&b, label);
    enc_str(&b, name);
    enc_attrs(&b, m, n);
    rc = commit_record(db, R_NODE, &b);
    free(b.p);
    return rc;
}

static int put_edge(yui *db, u64 from, u64 to, const char *type, const Attr *add, size_t na,
                    u64 *out_id, int *status) {
    Str k = {0}, b = {0};
    Entry *en;
    u64 id = 0;
    size_t n = 0;
    int changed = 0, rc;
    Attr *m;
    edge_key(&k, from, to, type);
    en = map_get(&db->m_edge, k.p, k.n, 0);
    free(k.p);
    if (en && db->edges[en->val - 1].live) id = en->val;
    if (id) {
        Edge *e = &db->edges[id - 1];
        m = attrs_merge(&db->stmt, e->attrs, e->nattrs, add, na, &n, &changed);
        *out_id = id;
        if (!changed) {
            *status = 0;
            return 0;
        }
        *status = 2;
    } else {
        m = attrs_merge(&db->stmt, NULL, 0, add, na, &n, &changed);
        id = db->ne + 1;
        *out_id = id;
        *status = 1;
    }
    enc_var(&b, id);
    enc_var(&b, from);
    enc_var(&b, to);
    enc_str(&b, type);
    enc_attrs(&b, m, n);
    rc = commit_record(db, R_EDGE, &b);
    free(b.p);
    return rc;
}

static int del_record(yui *db, int kind, u64 id) {
    Str b = {0};
    int rc;
    enc_var(&b, id);
    rc = commit_record(db, kind, &b);
    free(b.p);
    return rc;
}

/* ------------------------------------------------------------------ 表示 */

static int sym_at(const char *p, int *len);

/* 語のまま書けるか（書けなければ引用符で囲む） */
static int word_safe(const char *s);

static void print_quoted(Str *o, const char *s) {
    str_chr(o, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            str_chr(o, '\\');
            str_chr(o, *s);
        } else if (*s == '\n') {
            str_add(o, "\\n");
        } else if (*s == '\t') {
            str_add(o, "\\t");
        } else {
            str_chr(o, *s);
        }
    }
    str_chr(o, '"');
}
static void print_word(Str *o, const char *s) {
    if (word_safe(s)) str_add(o, s);
    else print_quoted(o, s);
}
static void print_value(Str *o, const Value *v) {
    char b[64];
    if (v->type == V_STR) {
        print_quoted(o, v->s);
    } else if (v->type == V_INT) {
        snprintf(b, sizeof b, "%lld", (long long)v->i);
        str_add(o, b);
    } else {
        snprintf(b, sizeof b, "%.15g", v->d);
        if (strtod(b, NULL) != v->d) snprintf(b, sizeof b, "%.17g", v->d);
        str_add(o, b);
        if (!strpbrk(b, ".e")) str_add(o, ".0");
    }
}
static void print_attrs(Str *o, const Attr *a, size_t n) {
    size_t i;
    if (!n) return;
    str_add(o, " {");
    for (i = 0; i < n; i++) {
        if (i) str_add(o, ", ");
        print_word(o, a[i].key);
        str_add(o, ": ");
        print_value(o, &a[i].v);
    }
    str_chr(o, '}');
}
/* Show の指示どおりに属性を出す。見せる のときは書かれた順に並べる */
static void print_attrs_sel(Str *o, const Attr *a, size_t n, const Show *sh) {
    size_t i, j, out = 0;
    if (sh->all) {
        print_attrs(o, a, n);
        return;
    }
    if (!sh->nkeys) return;
    /* ★無い属性は黙って飛ばします。点ごとに持っている属性が違うのが普通なので、
     *   「この点には 話者 が無い」をいちいちエラーにすると使えません。 */
    for (i = 0; i < sh->nkeys; i++)
        for (j = 0; j < n; j++)
            if (strcmp(a[j].key, sh->keys[i]) == 0) {
                str_add(o, out++ ? ", " : " {");
                print_word(o, a[j].key);
                str_add(o, ": ");
                print_value(o, &a[j].v);
                break;
            }
    if (out) str_chr(o, '}');
}
static void print_node(yui *db, Str *o, u64 id, const Show *sh) {
    Node *x = &db->nodes[id - 1];
    str_chr(o, '(');
    print_word(o, x->label);
    str_chr(o, ':');
    print_word(o, x->name);
    print_attrs_sel(o, x->attrs, x->nattrs, sh);
    str_chr(o, ')');
}
/* 種類の字句。「3段」のような語は段数と読まれるので引用符で囲む */
static int parse_depth(const char *w, long *out);
static void print_type(Str *o, const char *t) {
    long d;
    if (parse_depth(t, &d)) print_quoted(o, t);
    else print_word(o, t);
}
static void print_edge_step(Str *o, const Edge *e, int forward, const Show *sh) {
    str_add(o, forward ? "-[" : "<-[");
    print_type(o, e->type);
    print_attrs_sel(o, e->attrs, e->nattrs, sh);
    str_add(o, forward ? "]->" : "]-");
}
static void print_edge(yui *db, Str *o, u64 id, const Show *sh) {
    Edge *e = &db->edges[id - 1];
    print_node(db, o, e->from, node_show(sh));
    print_edge_step(o, e, 1, sh);
    print_node(db, o, e->to, node_show(sh));
}

/* ---------------------------------------------------------- 機械で読む形
 *
 * 1 行に 1 つの JSON を出します（JSON Lines）。yui_exec_json で切り替えます。
 *
 * ★鍵は ASCII です。値（ラベル・名前・属性）は日本語のまま UTF-8 で出します。
 *   関数名を ASCII にしているのと同じ理由です —— 読むのは機械なので、
 *   どの言語からも書きやすい形にしています。表に出る言葉は日本語のままです。
 * ★行の種類は "kind" で分かります: node / edge / path / link / text / summary。
 *   人が読む形の行と 1 対 1 です（同じ問い合わせなら、行の数も順も同じ）。
 */
static void json_str(Str *o, const char *s) {
    str_chr(o, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            str_chr(o, '\\');
            str_chr(o, (char)c);
        } else if (c == '\n') {
            str_add(o, "\\n");
        } else if (c == '\t') {
            str_add(o, "\\t");
        } else if (c == '\r') {
            str_add(o, "\\r");
        } else if (c < 0x20) {
            /* ★JSON では 0x20 より小さい字をそのまま書けません */
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            str_add(o, b);
        } else {
            str_chr(o, (char)c);
        }
    }
    str_chr(o, '"');
}
static void json_value(Str *o, const Value *v) {
    char b[64];
    if (v->type == V_STR) {
        json_str(o, v->s);
        return;
    }
    if (v->type == V_INT) {
        snprintf(b, sizeof b, "%lld", (long long)v->i);
        str_add(o, b);
        return;
    }
    /* ★JSON に inf と nan は書けないので null にします（読む側が落ちないように）。
     *   いまは問い合わせの側で弾いているので、入ってこない見込みです。 */
    if (!(v->d == v->d) || v->d - v->d != 0.0) {
        str_add(o, "null");
        return;
    }
    snprintf(b, sizeof b, "%.17g", v->d);
    str_add(o, b);
}
/* ,"attrs":{...} を足す。Show の指示に従う（名前だけ・見せる） */
static void json_attrs(Str *o, const Attr *a, size_t n, const Show *sh) {
    size_t i, j, out = 0;
    str_add(o, ",\"attrs\":{");
    if (sh->all) {
        for (i = 0; i < n; i++) {
            if (out++) str_chr(o, ',');
            json_str(o, a[i].key);
            str_chr(o, ':');
            json_value(o, &a[i].v);
        }
    } else {
        for (i = 0; i < sh->nkeys; i++)
            for (j = 0; j < n; j++)
                if (strcmp(a[j].key, sh->keys[i]) == 0) {
                    if (out++) str_chr(o, ',');
                    json_str(o, a[j].key);
                    str_chr(o, ':');
                    json_value(o, &a[j].v);
                    break;
                }
    }
    str_chr(o, '}');
}
/* {"label":…,"name":…,"attrs":{…}} の中身（括弧は呼ぶ側が書く） */
static void json_node_body(yui *db, Str *o, u64 id, const Show *sh) {
    Node *x = &db->nodes[id - 1];
    str_add(o, "\"label\":");
    json_str(o, x->label);
    str_add(o, ",\"name\":");
    json_str(o, x->name);
    json_attrs(o, x->attrs, x->nattrs, sh);
}
static void json_node(yui *db, Str *o, u64 id, const Show *sh) {
    str_add(o, "{\"kind\":\"node\",");
    json_node_body(db, o, id, sh);
    str_chr(o, '}');
}
static void json_edge(yui *db, Str *o, u64 id, const Show *sh) {
    Edge *e = &db->edges[id - 1];
    str_add(o, "{\"kind\":\"edge\",\"type\":");
    json_str(o, e->type);
    str_add(o, ",\"from\":{");
    json_node_body(db, o, e->from, node_show(sh));
    str_add(o, "},\"to\":{");
    json_node_body(db, o, e->to, node_show(sh));
    str_chr(o, '}');
    json_attrs(o, e->attrs, e->nattrs, sh);
    str_chr(o, '}');
}

/* 組み立て済みの 1 行をそのまま渡す（JSON の行はこちらを使います） */
static void emit_raw(yui *db, int kind, const char *text) {
    if (db->stopped || !db->cb) return;
    if (db->cb(db->ctx, kind, text)) db->stopped = 1;
}
/* 人が読む文を渡す。JSON のときは 1 つに包みます */
static void emit(yui *db, int kind, const char *text) {
    if (db->stopped || !db->cb) return;
    if (!db->json) {
        emit_raw(db, kind, text);
        return;
    }
    {
        Str o = {0};
        str_add(&o, kind == YUI_SUMMARY ? "{\"kind\":\"summary\",\"text\":"
                                        : "{\"kind\":\"text\",\"text\":");
        json_str(&o, text);
        str_chr(&o, '}');
        emit_raw(db, kind, o.p);
        free(o.p);
    }
}
/* 組み立てた 1 行をそのまま出す。
 * ★emit ではなく emit_raw を通します。JSON のときに print_path が作った
 *   JSON を、emit がもう一度 包んでしまうためです（実測で二重になりました）。
 *   人が読む形では emit と同じです（emit は包まずに emit_raw へ渡すだけ）。 */
static void emit_str(yui *db, Str *o) {
    emit_raw(db, YUI_ROW, o->p ? o->p : "");
    str_clear(o);
}
static void emit_count(yui *db, size_t n, const char *suffix) {
    /* ★手がかり（label_hint や 辿る の [?]）を添えることがあるので、
     *   短い桁では足りません。日本語は 1 文字 3 バイトです。 */
    char b[512];
    snprintf(b, sizeof b, "%llu 件%s", (unsigned long long)n, suffix);
    if (!db->json) {
        emit(db, YUI_SUMMARY, b);
        return;
    }
    /* ★JSON のときは件数を数として添えます。文を切り分けずに数えられるように */
    {
        Str o = {0};
        char c[40];
        str_add(&o, "{\"kind\":\"summary\",\"text\":");
        json_str(&o, b);
        snprintf(c, sizeof c, ",\"count\":%llu}", (unsigned long long)n);
        str_add(&o, c);
        emit_raw(db, YUI_SUMMARY, o.p);
        free(o.p);
    }
}

/* ------------------------------------------------------------------ 文法 */

/*
 * 記号は全角でも受ける。'\x01' は文の区切り「。」、
 * 'R' は →、'L' は ←、'Q' は 「、'q' は 」。それ以外の多バイト文字は -1。
 */
static const struct {
    const char *u;
    int sym;
} SYMS[] = {{"（", '('},  {"）", ')'}, {"［", '['}, {"］", ']'},  {"｛", '{'},
            {"｝", '}'},  {"：", ':'}, {"，", ','}, {"、", ','},  {"？", '?'},
            {"＃", '#'},  {"－", '-'}, {"＞", '>'}, {"＜", '<'},  {"　", ' '},
            {"。", '\x01'}, {"→", 'R'}, {"←", 'L'}, {"「", 'Q'}, {"」", 'q'},
            {NULL, 0}};

static int sym_at(const char *p, int *len) {
    u8 c = (u8)*p;
    int i;
    if (c < 0x80) {
        *len = c ? 1 : 0;
        /* 英字の R, L, Q, q や制御文字を記号と取り違えないよう別の値に逃がす */
        if (c == 'R' || c == 'L' || c == 'Q' || c == 'q' || c == 1) return -1;
        return c;
    }
    for (i = 0; SYMS[i].u; i++) {
        size_t n = strlen(SYMS[i].u);
        if (strncmp(p, SYMS[i].u, n) == 0) {
            *len = (int)n;
            return SYMS[i].sym;
        }
    }
    {
        u32 cp;
        size_t n = utf8_decode(p, &cp);
        *len = n ? (int)n : 1;
    }
    return -1;
}
static int is_word_stop(int s) {
    switch (s) {
    case 0: case ' ': case '\t': case '\r': case '\n': case '(': case ')': case '[':
    case ']': case '{': case '}': case ':': case ',': case '"': case '\x01': case 'Q':
    case 'q':
        return 1;
    default:
        return 0;
    }
}
static int word_safe(const char *s) {
    int len;
    if (!*s) return 0;
    if (sym_at(s, &len) == '?' || sym_at(s, &len) == '#') return 0;
    while (*s) {
        if (is_word_stop(sym_at(s, &len))) return 0;
        s += len;
    }
    return 1;
}

typedef struct {
    yui *db;
    const char *p;
} P;

static void skip_ws(P *p) {
    for (;;) {
        int len, s = sym_at(p->p, &len);
        if (s == ' ' || s == '\t' || s == '\r') {
            p->p += len;
        } else if (s == '#') {
            while (*p->p && *p->p != '\n') p->p++;
        } else {
            return;
        }
    }
}
static int peek(P *p) {
    int len;
    skip_ws(p);
    return sym_at(p->p, &len);
}
static int accept(P *p, int want) {
    int len, s;
    skip_ws(p);
    s = sym_at(p->p, &len);
    if (s != want) return 0;
    p->p += len;
    return 1;
}

static int perr(P *p, const char *msg) {
    const char *e = p->p;
    Str near = {0};
    int count = 0;
    while (*e && *e != '\n' && count < 16) {
        int len;
        sym_at(e, &len);
        str_mem(&near, e, (size_t)len);
        e += len;
        count++;
    }
    if (near.n) set_err(p->db, "文法エラー: %s（ここ: %s）", msg, near.p);
    else set_err(p->db, "文法エラー: %s（文の終わりで止まりました）", msg);
    free(near.p);
    return -1;
}

/* 語を読む。空なら NULL ではなく "" を返す */
static char *parse_word(P *p) {
    const char *st;
    int len;
    skip_ws(p);
    st = p->p;
    while (!is_word_stop(sym_at(p->p, &len))) p->p += len;
    return arena_strn(&p->db->stmt, st, (size_t)(p->p - st));
}
/* 引用された文字列を読む。引用でなければ *quoted=0 で何も読まない */
static int parse_quoted(P *p, char **out, int *quoted) {
    int len, s;
    Arena *ar = &p->db->stmt;
    skip_ws(p);
    s = sym_at(p->p, &len);
    *quoted = 0;
    *out = NULL;
    if (s == '"') {
        Str b = {0};
        p->p += len;
        for (;;) {
            char c = *p->p;
            if (!c) {
                free(b.p);
                return perr(p, "文字列が閉じていません");
            }
            p->p++;
            if (c == '"') break;
            if (c == '\\' && *p->p) {
                char n = *p->p++;
                if (n == 'n') c = '\n';
                else if (n == 't') c = '\t';
                else if (n == '"' || n == '\\') c = n;
                else {
                    str_chr(&b, '\\');
                    c = n;
                }
            }
            str_chr(&b, c);
        }
        *out = arena_strn(ar, b.p ? b.p : "", b.n);
        free(b.p);
        *quoted = 1;
    } else if (s == 'Q') {
        const char *st;
        p->p += len;
        st = p->p;
        for (;;) {
            int l2, s2 = sym_at(p->p, &l2);
            if (s2 == 0) return perr(p, "「 が閉じていません");
            if (s2 == 'q') {
                *out = arena_strn(ar, st, (size_t)(p->p - st));
                p->p += l2;
                break;
            }
            p->p += l2;
        }
        *quoted = 1;
    }
    return 0;
}
/* 語か引用 */
static int parse_text(P *p, char **out, int *quoted) {
    if (parse_quoted(p, out, quoted) != 0) return -1;
    if (!*quoted) *out = parse_word(p);
    return 0;
}

/* 全角の数字などを半角へ。数らしくなければ 0 */
static int fold_number(const char *w, char *buf, size_t cap) {
    size_t n = 0;
    while (*w) {
        u32 c;
        size_t l = utf8_decode(w, &c);
        if (!l || n + 1 >= cap) return 0;
        if (c >= 0xFF10 && c <= 0xFF19) c = c - 0xFF10 + '0';
        else if (c == 0xFF0D) c = '-';
        else if (c == 0xFF0E) c = '.';
        else if (c == 0xFF0B) c = '+';
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'))
            return 0;
        buf[n++] = (char)c;
        w += l;
    }
    buf[n] = 0;
    return n > 0;
}
static int parse_number(const char *w, Value *v) {
    char b[64], *end;
    const char *d;
    int isint = 1;
    if (!fold_number(w, b, sizeof b)) return 0;
    d = b[0] == '-' ? b + 1 : b;
    if (!(*d >= '0' && *d <= '9')) return 0;
    if (strpbrk(b, ".eE")) isint = 0;
    errno = 0;
    if (isint) {
        long long x = strtoll(b, &end, 10);
        if (*end || errno) return 0;
        v->type = V_INT;
        v->i = x;
        return 1;
    } else {
        double x = strtod(b, &end);
        if (*end || errno || !isfinite(x)) return 0;
        v->type = V_REAL;
        v->d = x;
        return 1;
    }
}
/* 「3段」なら 1。数字は全角でもよい */
static int parse_depth(const char *w, long *out) {
    static const char DAN[] = "段";
    size_t n = strlen(w), dl = strlen(DAN);
    char b[32];
    char *num, *end;
    long v;
    if (n <= dl || strcmp(w + n - dl, DAN) != 0 || n - dl >= sizeof b) return 0;
    memcpy(b, w, n - dl);
    b[n - dl] = 0;
    num = xmalloc(sizeof b);
    if (!fold_number(b, num, sizeof b)) {
        free(num);
        return 0;
    }
    if (!(num[0] >= '0' && num[0] <= '9')) {
        free(num);
        return 0;
    }
    errno = 0;
    v = strtol(num, &end, 10);
    if (*end || errno) {
        free(num);
        return 0;
    }
    free(num);
    *out = v;
    return 1;
}

/*
 * 文の末尾の修飾を読む。いまは 2 つ。順序は問いません。
 *
 *   新しい 20     新しい順に 20 件だけ返す（*lim）
 *   名前だけ      属性を出さず、名前とラベルだけ返す
 *   見せる a,b    その属性だけ、書いた順に出す
 *
 * 無ければ *lim=0（上限なし）・*sh は全部 出す、で 0 を返す。
 * ★名前だけ と 見せる は一緒に書けません（どちらも属性の出し方を決めるので）。
 *
 * ★「新しい」は時刻を見ません。点の id の大きい順です。
 *   結は追記しかせず、id は書いた順に振られるので、id が大きい＝後に書いた、が
 *   常に成り立ちます。詰め直す で振り直しても、生きている点を順に書くので
 *   順序は保たれます。だから時刻の属性が無いファイルでも使えます。
 *   逆に「属性の 時刻 で並べたい」ときには使えません（別の言葉が要ります）。
 *   ★入口が 2 つ以上あって、時刻の順に書いていないファイルでは、
 *     id の順と時刻の順がずれます（README の★を見よ）。
 * ★「新しい20」と続けて書いても読めます（parse_word が数字から先を 1 語にするため）。
 * ★同じ修飾を 2 回 書いたらエラーにします。黙って後ろ勝ちにすると、打ち間違いが
 *   「効かなかった」の形で出て、原因を探すことになります。
 */
static int parse_mods(P *p, long *lim, Show *sh) {
    static const char ATARA[] = "新しい";
    static const char BARE[] = "名前だけ";
    static const char MISERU[] = "見せる";
    char *w, b[32], *end;
    long v;
    int saw_bare = 0, saw_sel = 0;
    *lim = 0;
    *sh = SHOW_ALL;
    for (;;) {
        skip_ws(p);
        if (strncmp(p->p, BARE, strlen(BARE)) == 0) {
            if (saw_bare) return perr(p, "「名前だけ」は 1 回だけ書けます");
            if (saw_sel) return perr(p, "「名前だけ」と「見せる」は一緒に書けません");
            p->p += strlen(BARE);
            saw_bare = 1;
            sh->all = 0;
            continue;
        }
        if (strncmp(p->p, MISERU, strlen(MISERU)) == 0) {
            /* ★上限を決めて素直に取ります。並べるのは「どれを開くか決める」ための
             *   数個なので、伸ばす仕組みは要りません。超えたら理由を言って止めます。 */
            char *tmp[MAX_SHOW];
            size_t nk = 0;
            if (saw_sel) return perr(p, "「見せる」は 1 回だけ書けます");
            if (saw_bare) return perr(p, "「名前だけ」と「見せる」は一緒に書けません");
            p->p += strlen(MISERU);
            for (;;) {
                w = parse_word(p);
                if (!*w)
                    return perr(p,
                                "「見せる」のあとに属性の名前を書いてください"
                                "（例: 見せる 日時,話者）");
                if (nk == MAX_SHOW)
                    return perr(p, "「見せる」に並べられるのは 16 個までです");
                tmp[nk++] = w;
                skip_ws(p);
                if (peek(p) != ',') break;
                p->p++;
            }
            sh->keys = arena_alloc(&p->db->stmt, nk * sizeof *sh->keys);
            memcpy(sh->keys, tmp, nk * sizeof *sh->keys);
            sh->nkeys = nk;
            saw_sel = 1;
            sh->all = 0;
            continue;
        }
        if (strncmp(p->p, ATARA, strlen(ATARA)) != 0) return 0;
        if (*lim) return perr(p, "「新しい N」は 1 回だけ書けます");
        p->p += strlen(ATARA);
        w = parse_word(p);
        if (!*w || !fold_number(w, b, sizeof b) || !(b[0] >= '0' && b[0] <= '9'))
            return perr(p, "「新しい」のあとに件数を書いてください（例: 新しい 20）");
        errno = 0;
        v = strtol(b, &end, 10);
        if (*end || errno || v < 1)
            return perr(p, "件数は 1 以上の数で書いてください（例: 新しい 20）");
        *lim = v;
    }
}

typedef struct {
    Attr *a;
    size_t n;
} AttrList;

static int parse_attrs(P *p, AttrList *out) {
    Attr *a = NULL;
    size_t n = 0, cap = 0;
    out->a = NULL;
    out->n = 0;
    if (!accept(p, '{')) return 0;
    if (accept(p, '}')) return 0;
    for (;;) {
        char *key, *txt;
        int q;
        Value v;
        if (parse_text(p, &key, &q) != 0) return -1;
        if (!*key) return perr(p, "属性の名前が必要です");
        if (!accept(p, ':')) return perr(p, "属性の名前のあとに : が必要です");
        if (parse_text(p, &txt, &q) != 0) return -1;
        memset(&v, 0, sizeof v);
        if (q || !parse_number(txt, &v)) {
            if (!q && !*txt) return perr(p, "属性の値が必要です");
            v.type = V_STR;
            v.s = txt;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            Attr *na = arena_alloc(&p->db->stmt, nc * sizeof(Attr));
            if (n) memcpy(na, a, n * sizeof(Attr));
            a = na;
            cap = nc;
        }
        a[n].key = key;
        a[n].v = v;
        n++;
        if (accept(p, ',')) continue;
        if (accept(p, '}')) break;
        return perr(p, "属性のあとに , か } が必要です");
    }
    out->a = a;
    out->n = n;
    return 0;
}

enum { PAT_ANY, PAT_LABEL, PAT_NAME, PAT_BOTH };
typedef struct {
    int kind;
    char *label, *name;
    AttrList attrs;
} NodePat;

static int parse_node(P *p, NodePat *n, int allow_attrs) {
    char *a;
    int q;
    memset(n, 0, sizeof *n);
    if (!accept(p, '(')) return perr(p, "点は ( で始めます");
    if (accept(p, ')')) return 0;
    if (accept(p, '?')) {
        (void)parse_word(p); /* 変数名は読むだけ。結果は変わらない */
        if (!accept(p, ')')) return perr(p, "? のあとは ) で閉じます");
        return 0;
    }
    if (parse_text(p, &a, &q) != 0) return -1;
    if (!*a) return perr(p, q ? "空の名前は使えません" : "ラベルか名前が必要です");
    if (accept(p, ':')) {
        char *b;
        n->label = a;
        if (peek(p) == ')' || peek(p) == '{') {
            n->kind = PAT_LABEL;
        } else {
            if (parse_text(p, &b, &q) != 0) return -1;
            if (!*b) return perr(p, "空の名前は使えません");
            n->name = b;
            n->kind = PAT_BOTH;
        }
    } else {
        n->name = a;
        n->kind = PAT_NAME;
    }
    if (peek(p) == '{') {
        if (!allow_attrs) return perr(p, "属性を書けるのは 作る と 結ぶ だけです");
        if (parse_attrs(p, &n->attrs) != 0) return -1;
    }
    if (!accept(p, ')')) return perr(p, "点は ) で閉じます");
    return 0;
}

enum { DIR_RIGHT, DIR_LEFT, DIR_BOTH };
typedef struct {
    int dir;
    char *type; /* NULL は種類を問わない */
    long depth; /* 0 は書かれていない */
    AttrList attrs;
} EdgePat;

/* 線の - は、日本語入力で出やすい長音の ー も受ける */
static int accept_dash(P *p) {
    skip_ws(p);
    if (accept(p, '-')) return 1;
    if (strncmp(p->p, "ー", strlen("ー")) == 0) {
        p->p += strlen("ー");
        return 1;
    }
    return 0;
}

static int parse_edge(P *p, EdgePat *e, int allow_attrs, int allow_depth) {
    int left = 0, right = 0, q;
    char *w;
    memset(e, 0, sizeof *e);
    if (accept(p, 'L')) {
        left = 1;
    } else {
        if (accept(p, '<')) left = 1;
        if (!accept_dash(p)) return perr(p, "線は -[ か <-[ で始めます");
    }
    if (!accept(p, '[')) return perr(p, "線の種類は [ ] で囲みます");
    if (parse_quoted(p, &w, &q) != 0) return -1;
    if (q) {
        if (!*w) return perr(p, "空の種類は使えません");
        e->type = w;
    } else {
        w = parse_word(p);
        if (*w) {
            if (parse_depth(w, &e->depth)) {
                /* 段数だけ書いた形 */
            } else {
                e->type = w;
            }
        }
    }
    if (!e->depth && peek(p) != ']' && peek(p) != '{') {
        w = parse_word(p);
        if (!*w || !parse_depth(w, &e->depth)) return perr(p, "種類のあとに書けるのは「3段」か属性です");
    }
    if (e->depth || (w && 0)) {
        if (!allow_depth) return perr(p, "段数を書けるのは 辿る だけです");
        if (e->depth < 1 || e->depth > 1000000) return perr(p, "段数は 1 以上にしてください");
    }
    if (peek(p) == '{') {
        if (!allow_attrs) return perr(p, "属性を書けるのは 作る と 結ぶ だけです");
        if (parse_attrs(p, &e->attrs) != 0) return -1;
    }
    if (!accept(p, ']')) return perr(p, "線は ] で閉じます");
    if (accept(p, 'R')) {
        right = 1;
    } else {
        if (!accept_dash(p)) return perr(p, "] のあとに - か -> が必要です");
        if (accept(p, '>')) right = 1;
    }
    if (left && right) return perr(p, "向きはどちらか片方にしてください（両方なら - だけ）");
    e->dir = left ? DIR_LEFT : right ? DIR_RIGHT : DIR_BOTH;
    return 0;
}

/* ---------------------------------------------------------------- 点の解決 */

static int node_matches(const Node *x, const NodePat *pt) {
    if (!x->live) return 0;
    if (pt->label && strcmp(x->label, pt->label)) return 0;
    if (pt->name && strcmp(x->name, pt->name)) return 0;
    return 1;
}
static void collect_nodes(yui *db, const NodePat *pt, Vec *out) {
    size_t i;
    if (pt->kind == PAT_ANY) {
        for (i = 1; i <= db->nn; i++)
            if (db->nodes[i - 1].live) vec_push(out, i);
    } else {
        Entry *en;
        if (pt->kind == PAT_LABEL) en = map_get(&db->m_label, pt->label, strlen(pt->label), 0);
        else en = map_get(&db->m_name, pt->name, strlen(pt->name), 0);
        if (en)
            for (i = 0; i < en->list.n; i++)
                if (node_matches(&db->nodes[en->list.v[i] - 1], pt)) vec_push(out, en->list.v[i]);
    }
    vec_sort_unique(out);
}
/*
 * 名前で 1 つに決まる点を探す。無ければ *id=0 で 0 を返す。
 * 名前が無い形や、同名が複数あるときは失敗。
 */
static int find_one(yui *db, const NodePat *pt, u64 *id) {
    Vec v = {0};
    *id = 0;
    if (pt->kind == PAT_ANY || pt->kind == PAT_LABEL) {
        set_err(db, "どの点か決められません。名前を書いてください（例: (人:田中)）");
        return -1;
    }
    collect_nodes(db, pt, &v);
    if (v.n > 1) {
        Str o = {0};
        size_t i;
        for (i = 0; i < v.n && i < 5; i++) {
            if (i) str_add(&o, "、");
            print_node(db, &o, v.v[i], &NONE);
        }
        if (v.n > 5) str_add(&o, " ほか");
        set_err(db, "「%s」は %llu 件あり、どちらか分かりません: %s。ラベルを付けてください",
                pt->name, (unsigned long long)v.n, o.p);
        free(o.p);
        free(v.v);
        return -1;
    }
    if (v.n == 1) *id = v.v[0];
    free(v.v);
    return 0;
}
#define DEFAULT_LABEL "もの"

static int ensure_node(yui *db, const NodePat *pt, u64 *id, int *status) {
    u64 cur;
    if (find_one(db, pt, &cur) != 0) return -1;
    return put_node(db, cur, pt->label ? pt->label : DEFAULT_LABEL, pt->name, pt->attrs.a,
                    pt->attrs.n, id, status);
}

/* ------------------------------------------------------------------ 実行 */

static const char *status_word(int status) {
    return status == 1 ? "作りました" : status == 2 ? "更新しました" : "変わりません";
}

/* 1 行 出す。本体は下の「結果を出す」の節にあります（JSON もそこで分かれます） */
static void emit_doc(yui *db, u64 doc, const Show *sh);

static int cmd_make(P *p) {
    yui *db = p->db;
    NodePat n;
    u64 id;
    int st;
    if (parse_node(p, &n, 1) != 0) return -1;
    if (ensure_node(db, &n, &id, &st) != 0) return -1;
    /* ★emit_doc を通します。人が読む形は今までと同じで、JSON もここで効きます */
    emit_doc(db, DOC_NODE(id), &SHOW_ALL);
    emit(db, YUI_SUMMARY, status_word(st));
    return 0;
}

static int cmd_link(P *p) {
    yui *db = p->db;
    NodePat a, b;
    EdgePat e;
    u64 ia, ib, ie, cur;
    int sa, sb, st;
    if (parse_node(p, &a, 1) != 0) return -1;
    if (parse_edge(p, &e, 1, 0) != 0) return -1;
    if (parse_node(p, &b, 1) != 0) return -1;
    if (!e.type) {
        set_err(db, "結ぶ には線の種類が必要です（例: -[担当]->）");
        return -1;
    }
    if (e.dir == DIR_BOTH) {
        set_err(db, "結ぶ には向きが必要です（-> か <-）");
        return -1;
    }
    /* 片方だけ作ってから失敗しないよう、先に両方を確かめる */
    if (find_one(db, &a, &cur) != 0 || find_one(db, &b, &cur) != 0) return -1;
    if (ensure_node(db, &a, &ia, &sa) != 0) return -1;
    if (ensure_node(db, &b, &ib, &sb) != 0) return -1;
    if (e.dir == DIR_LEFT) {
        u64 t = ia;
        ia = ib;
        ib = t;
    }
    if (put_edge(db, ia, ib, e.type, e.attrs.a, e.attrs.n, &ie, &st) != 0) return -1;
    emit_doc(db, DOC_EDGE(ie), &SHOW_ALL);
    /*
     * ★端の点を作ったときは、そう言います。
     *   結ぶ は点が無ければ作る言葉ですが（README に書いてあります）、
     *   出力には出ていませんでした。流し込みで名前を打ち間違えると、
     *   属性の無い点が静かに増えます。実際に 2 人の利用者が踏んでいます
     *   （「リンク先の無い 11 件が空の点として増えていた」
     *     「点の数が SQLite より 2 件 多い」）。
     *   数えれば分かるものを、数えなくても分かるようにします。
     */
    {
        int made = (sa == 1) + (sb == 1);
        if (!made) {
            emit(db, YUI_SUMMARY, status_word(st));
        } else {
            char msg[80];
            snprintf(msg, sizeof msg, "%s（端の点も %d つ 作りました）", status_word(st), made);
            emit(db, YUI_SUMMARY, msg);
        }
    }
    return 0;
}


/* 属性の出し方は Show が決める（名前だけ・見せる。parse_mods を見よ） */
static void emit_doc(yui *db, u64 doc, const Show *sh) {
    Str o = {0};
    if (db->json) {
        if (doc & 1) json_edge(db, &o, doc >> 1, sh);
        else json_node(db, &o, doc >> 1, sh);
        emit_raw(db, YUI_ROW, o.p ? o.p : "");
        free(o.p);
        return;
    }
    if (doc & 1) print_edge(db, &o, doc >> 1, sh);
    else print_node(db, &o, doc >> 1, sh);
    emit_str(db, &o);
    free(o.p);
}

/*
 * 見る の「つながり」の 1 行。forward は線を out 向きに見ているかどうか。
 * ★人が読む形では 2 文字 下げた 1 行、JSON では kind=link の 1 つになります。
 */
static void emit_link(yui *db, Str *o, const Edge *e, int forward, u64 other) {
    if (db->json) {
        str_add(o, "{\"kind\":\"link\",\"dir\":");
        str_add(o, forward ? "\"out\"" : "\"in\"");
        str_add(o, ",\"type\":");
        json_str(o, e->type);
        json_attrs(o, e->attrs, e->nattrs, &SHOW_ALL);
        str_add(o, ",\"other\":{");
        json_node_body(db, o, other, &NONE);
        str_add(o, "}}");
        emit_raw(db, YUI_ROW, o->p ? o->p : "");
        str_clear(o);
        return;
    }
    str_add(o, "  ");
    print_edge_step(o, e, forward, &SHOW_ALL);
    print_node(db, o, other, &NONE);
    emit_str(db, o);
}

/*
 * 0 件だったときの手がかり。探した文字が点のラベルに当たるなら、その旨を out に書く。
 * 書いたら 1、当たらなければ 0 を返す。
 *
 * ★当たったラベルは 1 つだけ出します（たいてい 1 つで、並べると締めの行が読めなくなります）。
 * ★線の種類は見ません。ラベルは種類ごとに 1 つ覚えているので数が少なく、
 *   走るのが安いのですが、線の種類は覚えていないので線を全部 見ることになります。
 *   線が何万本もあるファイルでは、外した 探す だけが遅くなります。
 *   同じ迷いは線でも起きるので、覚える形を変えるなら、そのとき一緒に直します。
 */
static int label_hint(yui *db, const char *q, char *out, size_t outn) {
    u32 *qc;
    size_t qn = fold_text(q, &qc), i;
    int found = 0;
    for (i = 0; i < db->m_label.nb && !found; i++) {
        Entry *e;
        for (e = db->m_label.b[i]; e; e = e->next) {
            size_t j;
            int live = 0;
            if (!text_has(e->key, qc, qn)) continue;
            /* ★全部 消えたラベルでは知らせません。一覧 を薦めても 0 件になるので */
            for (j = 0; j < e->list.n; j++)
                if (db->nodes[e->list.v[j] - 1].live) {
                    live = 1;
                    break;
                }
            if (!live) continue;
            snprintf(out, outn, "（「%s」は点の種類です。一覧 (%s:) で引けます）", e->key,
                     e->key);
            found = 1;
            break;
        }
    }
    free(qc);
    return found;
}

static int cmd_search(P *p) {
    yui *db = p->db;
    char *q;
    int quoted;
    u32 *c;
    size_t n, i, count = 0;
    Vec hits = {0};
    NodePat f;
    int has_filter = 0;
    long lim;
    Show sh;
    char hint[192];
    if (parse_text(p, &q, &quoted) != 0) return -1;
    if (!*q) return perr(p, "探す文字を書いてください（例: 探す \"既読\"）");
    if (peek(p) == '(') {
        if (parse_node(p, &f, 0) != 0) return -1;
        has_filter = 1;
    }
    if (parse_mods(p, &lim, &sh) != 0) return -1;
    /* ★絞らない 探す は点と線の両方を返します。点の id と線の id は別の数直線なので、
     *   混ざったものを「新しい順」に並べられません（点を 10 個 作ったあとの線 1 本は、
     *   線の id が 1 なので、どう並べても点より古い側に来ます）。
     *   黙って怪しい順に並べるより、点の形を書いてもらうことにしました。 */
    if (lim && !has_filter)
        return perr(p,
                    "「新しい N」を付けるときは点の形も書いてください"
                    "（例: 探す \"見積\" (便:) 新しい 20）。絞らないと線も混ざり、新しい順に"
                    "並べられません");
    n = fold_text(q, &c);
    search_docs(db, c, n, 1, &hits);
#ifdef YUI_SELFCHECK
    /* 索引の答えと全件走査の答えを突き合わせる。
     * ★索引の取りこぼしは「結果が少ない」形で出るので、答えだけ見ても気づけません。
     *   テストのビルドだけで走ります（Makefile / build.bat が -DYUI_SELFCHECK を付ける）。 */
    {
        Vec all = {0};
        search_docs(db, c, n, 0, &all);
        if (all.n != hits.n ||
            (hits.n && memcmp(all.v, hits.v, hits.n * sizeof *hits.v) != 0)) {
            fprintf(stderr,
                    "自己検査に失敗: 索引 %llu 件 / 全件走査 %llu 件（探した文字: %s）\n",
                    (unsigned long long)hits.n, (unsigned long long)all.n, q);
            abort();
        }
        free(all.v);
    }
#endif
    free(c);
    for (i = 0; i < hits.n && !db->stopped; i++) {
        /* ★hits は doc の昇順（search_docs の最後に vec_sort_unique を通る）。
         *   「新しい N 件」のときは後ろから見ます。ここに来るのは点だけなので、
         *   doc の降順＝点の id の降順＝新しい順です。 */
        u64 doc = hits.v[lim ? hits.n - 1 - i : i];
        if (has_filter) {
            /* ★点の形を書いたら、返すのは点だけです。
             *   古い版は線も残していました（線の端の点が絞りに当たれば通す作り）。
             *   それだと (便)-[…]->(人) のような線は (便:) でも (人:) でも端が当たるので、
             *   どう絞っても落ちません。「絞ったのに件数が減らない」形で静かに狂います。
             *   書いたのは点の形なので、点だけ返すほうが読みどおりです。
             *   線の属性を探したいときは、絞りを書かずに 探す "文字" とします。 */
            if (doc & 1) continue;
            if (!node_matches(&db->nodes[(doc >> 1) - 1], &f)) continue;
        }
        emit_doc(db, doc, &sh);
        count++;
        if (lim && count == (size_t)lim) break;
    }
    free(hits.v);
    /* ★0 件のときだけ、探した文字がラベルに当たらないか見ます。
     *   探す が見るのは「点の名前・点の属性・線の属性」で、ラベルは見ません。
     *   ラベルを索引に入れれば当たるようになりますが、同じラベルの点が
     *   何万と並ぶファイルでは索引がその分だけ太ります。
     *   メモリを増やさずに迷いを解くため、入れずに「知らせる」ことにしました。
     *   0 件のときだけ走るので、当たっているときの速さには影響しません。 */
    if (count == 0 && label_hint(db, q, hint, sizeof hint))
        emit_count(db, count, hint);
    else
        emit_count(db, count, "");
    return 0;
}

/* 起点から終点までの道を、たどった順に書く */
static void print_path(yui *db, Str *o, u64 start, u64 last_node, u64 edge, u64 next,
                       const Show *sh) {
    Vec nodes = {0}, edges = {0};
    u64 cur = last_node;
    size_t i;
    vec_push(&nodes, next);
    vec_push(&edges, edge);
    while (cur != start) {
        vec_push(&nodes, cur);
        vec_push(&edges, db->nodes[cur - 1].via_edge);
        cur = db->nodes[cur - 1].via_node;
    }
    if (db->json) {
        /* ★道は点と線の並びとして出します。nodes は edges より 1 つ多いです。
         *   forward は、その線をどちら向きに通ったかです（人が読む形の -> と <- に当たります）。 */
        str_add(o, "{\"kind\":\"path\",\"nodes\":[{");
        json_node_body(db, o, start, node_show(sh));
        str_chr(o, '}');
        for (i = nodes.n; i > 0; i--) {
            str_add(o, ",{");
            json_node_body(db, o, nodes.v[i - 1], node_show(sh));
            str_chr(o, '}');
        }
        str_add(o, "],\"edges\":[");
        cur = start;
        for (i = nodes.n; i > 0; i--) {
            Edge *e = &db->edges[edges.v[i - 1] - 1];
            u64 nx = nodes.v[i - 1];
            if (i != nodes.n) str_chr(o, ',');
            str_add(o, "{\"type\":");
            json_str(o, e->type);
            str_add(o, e->from == cur && e->to == nx ? ",\"forward\":true" : ",\"forward\":false");
            json_attrs(o, e->attrs, e->nattrs, sh);
            str_chr(o, '}');
            cur = nx;
        }
        str_add(o, "]}");
        free(nodes.v);
        free(edges.v);
        return;
    }
    print_node(db, o, start, node_show(sh));
    cur = start;
    for (i = nodes.n; i > 0; i--) {
        Edge *e = &db->edges[edges.v[i - 1] - 1];
        u64 nx = nodes.v[i - 1];
        print_edge_step(o, e, e->from == cur && e->to == nx, sh);
        print_node(db, o, nx, node_show(sh));
        cur = nx;
    }
    free(nodes.v);
    free(edges.v);
}

/*
 * 「新しい N 件」のための受け皿。key（辿り着いた点の id）の大きい順に N 件だけ残す。
 *
 * ★辿る は歩きながら出す作りなので、全部 溜めてから並べ直すと、
 *   当たりが数万件あるときに置き場がその分 要ります。ここは N 件だけ持ちます。
 * ★道の文字列はその場で作って持ちます。辿る は歩きながら via_edge / via_node を
 *   上書きしていくので、あとから道を作り直せません（print_path を見よ）。
 * ★歩くのは全部 歩きます。N は「返す数」で、「見る数」ではありません。
 *   先に打ち切ると、あとで見つかるはずの新しい点を落とします。
 */
typedef struct {
    u64 key;
    char *s;
} TopItem;
typedef struct {
    TopItem *v;
    size_t n, cap, alloc;
} Top;

static void top_init(Top *t, size_t cap) {
    t->v = NULL;
    t->n = 0;
    t->cap = cap;
    t->alloc = 0;
}
static void top_push(Top *t, u64 key, const char *s) {
    size_t i;
    if (t->n == t->cap) {
        if (key <= t->v[t->cap - 1].key) return; /* いま持っている最小より古い */
        free(t->v[t->cap - 1].s);                /* いちばん古いものを捨てる */
    } else {
        /* ★人が書いた N をそのまま確保しません。「新しい 1000000000」のような数でも、
         *   当たりが 3 件なら 3 件ぶんしか取りません。要るぶんだけ伸ばします。 */
        if (t->n == t->alloc) {
            t->alloc = t->alloc ? t->alloc * 2 : 16;
            if (t->alloc > t->cap) t->alloc = t->cap;
            t->v = xrealloc(t->v, t->alloc * sizeof *t->v);
        }
        t->n++;
    }
    for (i = t->n - 1; i > 0 && t->v[i - 1].key < key; i--) t->v[i] = t->v[i - 1];
    t->v[i].key = key;
    t->v[i].s = xstrdup(s);
}
static void top_free(Top *t) {
    size_t i;
    for (i = 0; i < t->n; i++) free(t->v[i].s);
    free(t->v);
}

static int cmd_walk(P *p) {
    yui *db = p->db;
    NodePat a, b;
    EdgePat e;
    Vec starts = {0}, queue = {0};
    Str o = {0};
    /* ★{0} で初期化します。使うのは lim があるときだけですが、
     *   -O2 の gcc は「使われるかもしれない」と見ます（実際には通りません）。
     *   警告を消すためだけでなく、読む人が追わなくて済むようにもなります。 */
    Top top = {0};
    size_t s, count = 0;
    long maxd, lim;
    Show sh;
    if (parse_node(p, &a, 0) != 0) return -1;
    if (parse_edge(p, &e, 0, 1) != 0) return -1;
    if (parse_node(p, &b, 0) != 0) return -1;
    if (parse_mods(p, &lim, &sh) != 0) return -1;
    if (lim) top_init(&top, (size_t)lim);
    maxd = e.depth ? e.depth : 1;
    collect_nodes(db, &a, &starts);
    for (s = 0; s < starts.n && !db->stopped; s++) {
        size_t head = 0;
        u32 gen = ++db->gen;
        u64 st = starts.v[s];
        queue.n = 0;
        vec_push(&queue, st);
        db->nodes[st - 1].mark = gen;
        db->nodes[st - 1].depth = 0;
        while (head < queue.n && !db->stopped) {
            u64 u = queue.v[head++];
            int side;
            if ((long)db->nodes[u - 1].depth >= maxd) continue;
            for (side = 0; side < 2; side++) {
                const Vec *adj;
                size_t i;
                if (side == 0 && e.dir == DIR_LEFT) continue;
                if (side == 1 && e.dir == DIR_RIGHT) continue;
                adj = side == 0 ? &db->nodes[u - 1].out : &db->nodes[u - 1].in;
                for (i = 0; i < adj->n && !db->stopped; i++) {
                    u64 eid = adj->v[i], v;
                    Edge *ed = &db->edges[eid - 1];
                    if (!ed->live || ed->mark == gen) continue;
                    if (e.type && strcmp(ed->type, e.type)) continue;
                    ed->mark = gen;
                    v = side == 0 ? ed->to : ed->from;
                    if (node_matches(&db->nodes[v - 1], &b)) {
                        print_path(db, &o, st, u, eid, v, &sh);
                        if (lim) {
                            top_push(&top, v, o.p ? o.p : "");
                            str_clear(&o);
                        } else {
                            emit_str(db, &o);
                            count++;
                        }
                    }
                    if (db->nodes[v - 1].mark != gen) {
                        Node *nv = &db->nodes[v - 1];
                        nv->mark = gen;
                        nv->depth = db->nodes[u - 1].depth + 1;
                        nv->via_edge = eid;
                        nv->via_node = u;
                        vec_push(&queue, v);
                    }
                }
            }
        }
    }
    if (lim) {
        for (s = 0; s < top.n && !db->stopped; s++) emit_raw(db, YUI_ROW, top.v[s].s);
        count = top.n;
        top_free(&top);
    }
    free(starts.v);
    free(queue.v);
    free(o.p);
    /*
     * ★0 件 のときだけ、線の種類が「?」で始まっていないか見ます。
     *   点の側の (?何) は「何でもよい」の印ですが、**線の側に変数はありません**。
     *   [?] は「種類が『?』という名前の線」を探してしまい、0 件で黙ります。
     *   実際に利用者が踏みました（点の側と同じだと思って打った）。
     *   探す がラベルで同じことをしているので、並びとしても自然です。
     */
    if (count == 0 && e.type && e.type[0] == '?') {
        /* ★日本語の文面は 1 文字 3 バイトなので、桁を小さく見積もると切れます
         *   （160 バイトで組んだら -O2 の gcc が切り詰めの警告を出しました） */
        char h[256];
        snprintf(h, sizeof h, "（「[%.40s]」は種類が「%.40s」の線です。種類を問わないなら [] です）",
                 e.type, e.type);
        emit_count(db, count, h);
    } else {
        emit_count(db, count, "");
    }
    return 0;
}

static int cmd_show(P *p) {
    yui *db = p->db;
    NodePat n;
    Vec v = {0};
    Str o = {0};
    size_t i, j;
    if (parse_node(p, &n, 0) != 0) return -1;
    collect_nodes(db, &n, &v);
    for (i = 0; i < v.n && !db->stopped; i++) {
        Node *x = &db->nodes[v.v[i] - 1];
        emit_doc(db, DOC_NODE(v.v[i]), &SHOW_ALL);
        for (j = 0; j < x->out.n; j++) {
            Edge *e = &db->edges[x->out.v[j] - 1];
            if (!e->live) continue;
            emit_link(db, &o, e, 1, e->to);
        }
        for (j = 0; j < x->in.n; j++) {
            Edge *e = &db->edges[x->in.v[j] - 1];
            if (!e->live || e->from == e->to) continue;
            emit_link(db, &o, e, 0, e->from);
        }
    }
    emit_count(db, v.n, "");
    free(v.v);
    free(o.p);
    return 0;
}

static int cmd_list(P *p) {
    yui *db = p->db;
    NodePat n;
    Vec v = {0};
    size_t i, count;
    long lim;
    Show sh;
    memset(&n, 0, sizeof n);
    /*
     * ★括弧を省けません。
     *   古い版は省けて、そのとき 0 件 を返していました。ゼロで埋めた形が
     *   どの点にも当たらないためで、意図した振る舞いではありません。
     *   「データが 0 件」と「書き方を間違えた」が同じ見た目になるので、
     *   断ります。すべてを出したいなら 一覧 () と書けます。
     *   辿る と 見る も、点の形が無ければ断ります（同じ扱いにそろえました）。
     */
    if (peek(p) != '(') return perr(p, "一覧 には点の形が必要です（すべてなら 一覧 ()）");
    if (parse_node(p, &n, 0) != 0) return -1;
    if (parse_mods(p, &lim, &sh) != 0) return -1;
    collect_nodes(db, &n, &v);
    /* ★collect_nodes は id の昇順で返します（最後に vec_sort_unique を通るため）。
     *   なので「新しい N 件」は、後ろから N 個 取るだけで出せます。
     *   並べ直しも、余分な置き場も要りません。 */
    count = (lim && (size_t)lim < v.n) ? (size_t)lim : v.n;
    for (i = 0; i < count && !db->stopped; i++)
        emit_doc(db, DOC_NODE(lim ? v.v[v.n - 1 - i] : v.v[i]), &sh);
    emit_count(db, count, "");
    free(v.v);
    return 0;
}

static int cmd_delete(P *p) {
    yui *db = p->db;
    NodePat a, b;
    EdgePat e;
    u64 ia, ib;
    char msg[120];
    if (parse_node(p, &a, 0) != 0) return -1;
    {
        int len, s;
        skip_ws(p);
        s = sym_at(p->p, &len);
        if (s == '-' || s == '<' || s == 'L' || strncmp(p->p, "ー", strlen("ー")) == 0) {
            Str k = {0};
            Entry *en;
            if (parse_edge(p, &e, 0, 0) != 0) return -1;
            if (parse_node(p, &b, 0) != 0) return -1;
            if (!e.type) {
                set_err(db, "消す には線の種類が必要です");
                return -1;
            }
            if (e.dir == DIR_BOTH) {
                set_err(db, "消す には向きが必要です（-> か <-）");
                return -1;
            }
            if (find_one(db, &a, &ia) != 0 || find_one(db, &b, &ib) != 0) return -1;
            if (e.dir == DIR_LEFT) {
                u64 t = ia;
                ia = ib;
                ib = t;
            }
            if (!ia || !ib) {
                emit(db, YUI_SUMMARY, "消すものはありません");
                return 0;
            }
            edge_key(&k, ia, ib, e.type);
            en = map_get(&db->m_edge, k.p, k.n, 0);
            free(k.p);
            if (!en || !db->edges[en->val - 1].live) {
                emit(db, YUI_SUMMARY, "消すものはありません");
                return 0;
            }
            if (del_record(db, R_DEL_EDGE, en->val) != 0) return -1;
            emit(db, YUI_SUMMARY, "線を 1 本 消しました");
            return 0;
        }
    }
    if (find_one(db, &a, &ia) != 0) return -1;
    if (!ia) {
        emit(db, YUI_SUMMARY, "消すものはありません");
        return 0;
    }
    {
        Node *x = &db->nodes[ia - 1];
        size_t i, ne = 0;
        for (i = 0; i < x->out.n; i++)
            if (db->edges[x->out.v[i] - 1].live) ne++;
        for (i = 0; i < x->in.n; i++) {
            Edge *ed = &db->edges[x->in.v[i] - 1];
            if (ed->live && ed->from != ed->to) ne++;
        }
        if (del_record(db, R_DEL_NODE, ia) != 0) return -1;
        snprintf(msg, sizeof msg, "点を 1 件と、つながる線を %llu 本 消しました",
                 (unsigned long long)ne);
        emit(db, YUI_SUMMARY, msg);
    }
    return 0;
}

/* 詰め直す の本体は「開く」の節にあります（開き直す道具と同じ場所なので） */
static int repack(yui *db, u64 *nnode, u64 *nedge, u64 *before, u64 *after);
static int file_exists(const char *path);

static int cmd_repack(P *p) {
    yui *db = p->db;
    u64 nn = 0, ne = 0, before = 0, after = 0;
    Str frag = {0};
    char msg[256];
    if (repack(db, &nn, &ne, &before, &after) != 0) return -1;
    /* ★古い版は <パス>.断片 を作っていました。残っていると、詰め直しても
     *   取り消した本文がそちらに居ます。消すかどうかは持ち主の判断なので、
     *   こちらからは消さずに知らせます。 */
    str_add(&frag, db->path);
    str_add(&frag, ".断片");
    if (file_exists(frag.p)) {
        char w[512];
        snprintf(w, sizeof w,
                 "★%s が残っています（古い版が書きかけの末尾を退避したもの）。"
                 "取り消した本文が入っていることがあるので、中を見てから消してください",
                 frag.p);
        emit(db, YUI_ROW, w);
    }
    free(frag.p);
    snprintf(msg, sizeof msg, "詰め直しました: 点 %llu 件 / 線 %llu 本 / %llu → %llu バイト",
             (unsigned long long)nn, (unsigned long long)ne, (unsigned long long)before,
             (unsigned long long)after);
    emit(db, YUI_SUMMARY, msg);
    return 0;
}

static const char *const HELP[] = {
    "作る (人:田中 {役職: \"部長\", 年齢: 42})          点を作る。あれば属性を重ねる",
    "結ぶ (案件:結の開発)-[担当]->(人:田中)            線を引く。無い点は作る",
    "探す \"既読\"                                      文字列の部分一致",
    "探す \"既読\" (メモ:)                               ラベルで絞る",
    "辿る (人:田中)<-[担当]-(?案件)                    向きは ->  <-  -（両方）",
    "辿る (私)-[返信 3段]->(?相手)                     3 段先まで",
    "辿る (私)-[]->()                                  種類も相手も問わない",
    "見る (人:田中)                                    属性とつながり",
    "一覧 (人:)                                        そのラベルの点すべて",
    "一覧 (人:) 新しい 20                              新しい順に 20 件（探す 辿る にも）",
    "一覧 (人:) 名前だけ                               属性を出さない（探す 辿る にも）",
    "一覧 (人:) 見せる 役職,年齢                       その属性だけ出す（探す 辿る にも）",
    "消す (人:田中)                                    点とその線を消す",
    "消す (案件:結の開発)-[担当]->(人:田中)            線を消す",
    "詰め直す                                          取り消した記録をファイルから落とす",
    "同期                                              ファイルへ確実に書き出す",
    "終わる                                            終了",
    "点の書き方: (名前)  (ラベル:名前)  (ラベル:)  (?なんでも)  ()",
    NULL};

static int cmd_sync(P *p) {
    yui *db = p->db;
    if (fsync(db->fd) != 0) {
        set_err(db, "同期に失敗しました: %s", strerror(errno));
        return 1;
    }
    emit(db, YUI_SUMMARY, "同期しました");
    return 0;
}

static int cmd_help(P *p) {
    int i;
    for (i = 0; HELP[i]; i++) emit(p->db, YUI_ROW, HELP[i]);
    return 0;
}

/*
 * 言葉の振り分け表。
 * ★writes は「ファイルを変えるか」です。読むだけで開いたときに断る判定が
 *   ここだけに在るので、言葉を足す人は必ず writes を書くことになります。
 *   デーモンのような外側に動詞の一覧を持たせると、言葉が増えたとき黙ってズレます。
 */
typedef struct {
    const char *kw;
    int (*fn)(P *p);
    int writes;
} Cmd;

static const Cmd CMDS[] = {{"作る", cmd_make, 1},    {"結ぶ", cmd_link, 1},
                           {"探す", cmd_search, 0},  {"辿る", cmd_walk, 0},
                           {"見る", cmd_show, 0},    {"一覧", cmd_list, 0},
                           {"消す", cmd_delete, 1},  {"詰め直す", cmd_repack, 1},
                           {"同期", cmd_sync, 0},    {"手引き", cmd_help, 0},
                           {NULL, NULL, 0}};

/* 1 文を実行。空の文なら何もしない。戻り値は YUI_* */
static int exec_stmt(P *p) {
    yui *db = p->db;
    char *kw;
    int rc = 0, s, i;
    s = peek(p);
    if (s == 0) return YUI_OK;
    if (s == '\n' || s == '\x01') {
        int len;
        sym_at(p->p, &len);
        p->p += len;
        return YUI_OK;
    }
    kw = parse_word(p);
    if (!*kw) {
        perr(p, "文の始まりが分かりません");
        return YUI_ERROR;
    }
    if (strcmp(kw, "終わる") == 0) return YUI_END;
    for (i = 0; CMDS[i].kw; i++)
        if (strcmp(kw, CMDS[i].kw) == 0) break;
    if (!CMDS[i].kw) {
        set_err(db, "知らない言葉です: %s（「手引き」で一覧を出せます）", kw);
        return YUI_ERROR;
    }
    if (CMDS[i].writes && db->readonly) {
        set_err(db, "読むだけで開いています。%s は使えません", kw);
        return YUI_ERROR;
    }
    rc = CMDS[i].fn(p);
    if (rc != 0) return YUI_ERROR;
    s = peek(p);
    if (s != 0 && s != '\n' && s != '\x01') {
        perr(p, "文のあとに余分な文字があります");
        return YUI_ERROR;
    }
    return YUI_OK;
}

/*
 * yui_exec と yui_exec_json の本体。違いは出し方（json）だけです。
 * ★json はこの呼び出しの間だけ立てます。次の yui_exec が人が読む形に戻るように。
 */
static int exec_all(yui *db, const char *query, yui_cb cb, void *ctx, int json) {
    P p;
    int rc = YUI_OK;
    if (!db) return YUI_ERROR;
    if (db->fd < 0) {
        /* ★前の失敗の文を返さずに、いまの状態を言います。
         *   閉じたままのときに前の理由を返すと、2 行目以降が
         *   「入れ替えられません」のまま並び、何が起きているか読めません
         *   （陽性対照を取っていて気づきました）。 */
        set_err(db, "ファイルが開かれていません");
        return YUI_ERROR;
    }
    db->json = json;
    free(db->err);
    db->err = NULL;
    if (!query) query = "";
    if (!utf8_valid(query)) {
        set_err(db, "問い合わせが UTF-8 として正しくありません");
        db->json = 0;
        return YUI_ERROR;
    }
    db->cb = cb;
    db->ctx = ctx;
    db->stopped = 0;
    p.db = db;
    p.p = query;
    while (*p.p && rc == YUI_OK && !db->stopped) {
        rc = exec_stmt(&p);
        arena_reset(&db->stmt);
    }
    db->cb = NULL;
    db->ctx = NULL;
    db->json = 0;
    return rc;
}

int yui_exec(yui *db, const char *query, yui_cb cb, void *ctx) {
    return exec_all(db, query, cb, ctx, 0);
}
int yui_exec_json(yui *db, const char *query, yui_cb cb, void *ctx) {
    return exec_all(db, query, cb, ctx, 1);
}

/* ------------------------------------------------------------------ 開く */

/* 点・線・索引・地図を解放して、開く前の空の状態へ戻します。
 * 閉じるときと、詰め直して読み直すときの両方で通ります。 */
static void free_state(yui *db) {
    size_t i;
    for (i = 0; i < db->nn; i++) {
        Node *x = &db->nodes[i];
        free(x->label);
        free(x->name);
        attrs_free(x->attrs, x->nattrs);
        free(x->out.v);
        free(x->in.v);
    }
    for (i = 0; i < db->ne; i++) {
        free(db->edges[i].type);
        attrs_free(db->edges[i].attrs, db->edges[i].nattrs);
    }
    free(db->nodes);
    free(db->edges);
    map_free(&db->m_key);
    map_free(&db->m_name);
    map_free(&db->m_label);
    map_free(&db->m_edge);
    /* ★枠の中に直に入っているもの（cap == 0）は free しません。GramSlot の★を見よ */
    for (i = 0; i < db->gramcap; i++)
        if (db->grams[i].cap) free(db->grams[i].u.v);
    free(db->grams);
    arena_reset(&db->stmt);
    arena_reset(&db->scratch);
    db->nodes = NULL;
    db->nn = db->ncap = 0;
    db->edges = NULL;
    db->ne = db->ecap = 0;
    db->grams = NULL;
    db->ngrams = db->gramcap = 0;
    db->gen = 0;
    db->size = 0;
}

static int replay(yui *db, u8 *d, size_t size) {
    size_t pos = MAGIC_LEN, torn = 0;
    int has_torn = 0;
    if (size < MAGIC_LEN || memcmp(d, MAGIC, MAGIC_LEN) != 0) {
        size_t cmp = size < MAGIC_LEN ? size : MAGIC_LEN - 1;
        if (size >= MAGIC_LEN && memcmp(d, MAGIC, MAGIC_LEN - 1) == 0) {
            set_err(db, "版 %d のファイルは読めません（読めるのは版 %d）", d[MAGIC_LEN - 1],
                    MAGIC[MAGIC_LEN - 1]);
            return -1;
        }
        if (size >= MAGIC_LEN || memcmp(d, MAGIC, cmp) != 0) {
            set_err(db, "結のファイルではありません: %s", db->path);
            return -1;
        }
        /* 先頭を書いている途中で落ちた。中身はまだ無い */
        if (ftruncate(db->fd, 0) != 0 || lseek(db->fd, 0, SEEK_SET) < 0 ||
            write_all(db->fd, MAGIC, MAGIC_LEN) != 0) {
            set_err(db, "先頭を書き直せません: %s", strerror(errno));
            return -1;
        }
        db->size = MAGIC_LEN;
        return 0;
    }
    while (pos < size) {
        u32 len;
        size_t total;
        if (size - pos < 9) {
            has_torn = 1;
            torn = pos;
            break;
        }
        len = get32(d + pos);
        if (len >= MAX_BODY || (u64)len + 9 > (u64)(size - pos)) {
            has_torn = 1;
            torn = pos;
            break;
        }
        total = (size_t)len + 9;
        if (crc_calc(db, d + pos, 5 + (size_t)len) != get32(d + pos + 5 + len)) {
            if (pos + total == size) {
                has_torn = 1;
                torn = pos;
                break;
            }
            set_err(db, "ファイルの途中が壊れています（%llu バイト目）。開くのをやめました",
                    (unsigned long long)pos);
            return -1;
        }
        if (apply_record(db, d[pos + 4], d + pos + 5, len) != 0) {
            set_err(db, "ファイルの途中の記録が読めません（%llu バイト目）。開くのをやめました",
                    (unsigned long long)pos);
            return -1;
        }
        pos += total;
    }
    if (has_torn) {
        size_t cut = size - torn;
        char b[256];
        /* ★捨てます。<パス>.断片 のような別ファイルには残しません。
         *   残すと「消したはずの本文が隣のファイルに居る」ことになり、
         *   詰め直す で取り消した記録を落としても、そちらに残ります。
         *   捨てるのは「crc の合わない最後の記録から末尾まで」で、書き終わった
         *   記録（crc の合う記録）は入りません（追記しかしないため）。
         *   ただし落ちた拍子に途中が飛んでいれば、その先も一緒に捨てます。 */
        if (ftruncate(db->fd, (off_t)torn) != 0) {
            set_err(db, "書きかけの末尾を切り捨てられません: %s", strerror(errno));
            return -1;
        }
        snprintf(b, sizeof b,
                 "書きかけの末尾 %llu バイトを捨てました（最後の書き込みが途中で終わっています）",
                 (unsigned long long)cut);
        db->notice = xstrdup(b);
        size = torn;
    }
    db->size = size;
    return 0;
}

/* db->fd を頭から読んで、記録を順に適用します。
 * 開くときと、詰め直して入れ替えたあとの読み直しで通ります。 */
static int load_file(yui *db) {
    struct stat st;
    u8 *data;
    size_t got = 0;
    if (lseek(db->fd, 0, SEEK_SET) < 0) {
        set_err(db, "位置を動かせません: %s", strerror(errno));
        return -1;
    }
    if (fstat(db->fd, &st) != 0) {
        set_err(db, "大きさを読めません: %s", strerror(errno));
        return -1;
    }
    if (st.st_size == 0) {
        char b[512];
        if (write_all(db->fd, MAGIC, MAGIC_LEN) != 0) {
            set_err(db, "書けません: %s", strerror(errno));
            return -1;
        }
        /*
         * ★中身が無かったことを知らせます。
         *   結は無いファイルを作って開きます。それは便利ですが、
         *   **置き場を取り違えたときに「0 件」と同じ見た目になります**。
         *   実際に、作業場所を間違えて 探す を打ち、空のファイルが新しくできて
         *   「0 件」が返り、grep の件数と合わないので気づいた人がいます。
         *   合わせる相手が無ければ気づけないので、こちらから言います。
         *   エラーにはしません（作って開くのは正しい振る舞いなので）。
         */
        snprintf(b, sizeof b, "新しいファイルとして始めました: %s（中身がありませんでした）",
                 db->path);
        free(db->notice);
        db->notice = xstrdup(b);
        db->size = MAGIC_LEN;
        return 0;
    }
    data = xmalloc((size_t)st.st_size);
    while (got < (size_t)st.st_size) {
        ssize_t r = read(db->fd, data + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            free(data);
            set_err(db, "読めません: %s", r < 0 ? strerror(errno) : "途中で終わりました");
            return -1;
        }
        got += (size_t)r;
    }
    if (replay(db, data, got) != 0) {
        free(data);
        return -1;
    }
    free(data);
    if (lseek(db->fd, 0, SEEK_END) < 0) {
        set_err(db, "位置を動かせません: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- 詰め直す */

/* 作業用のファイルの名前。詰め直しが終われば消えます */
#define PACK_SUFFIX ".詰"

static int file_exists(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* 記録 1 件を、長さ・種別・CRC で包んで out へ足します（commit_record と同じ形） */
static int pack_record(yui *db, Str *out, int kind, const Str *body) {
    size_t blen = body->n, total = 5 + blen + 4;
    u8 *buf;
    if (blen >= MAX_BODY) {
        set_err(db, "データが大きすぎます（1 件 1 GiB まで）");
        return -1;
    }
    buf = xmalloc(total);
    put32(buf, (u32)blen);
    buf[4] = (u8)kind;
    if (blen) memcpy(buf + 5, body->p, blen);
    put32(buf + 5 + blen, crc_calc(db, buf, 5 + blen));
    str_mem(out, buf, total);
    free(buf);
    return 0;
}

/*
 * 生きている点と線だけを並べた、新しい中身を作ります。
 * ★id は 1 から振り直します（消えた点の分が空くため）。
 *   id はファイルの中だけの番号で、外へは出していません。
 */
static int build_packed(yui *db, Str *out, u64 *out_nn, u64 *out_ne) {
    u64 *newid = NULL;
    size_t i;
    u64 nn = 0, ne = 0;
    int rc = 0;
    str_mem(out, MAGIC, MAGIC_LEN);
    if (db->nn) newid = xmalloc(db->nn * sizeof *newid);
    for (i = 0; i < db->nn; i++) {
        Node *x = &db->nodes[i];
        Str b = {0};
        newid[i] = 0;
        if (!x->live) continue;
        newid[i] = ++nn;
        enc_var(&b, newid[i]);
        enc_str(&b, x->label);
        enc_str(&b, x->name);
        enc_attrs(&b, x->attrs, x->nattrs);
        rc = pack_record(db, out, R_NODE, &b);
        free(b.p);
        if (rc != 0) goto done;
    }
    for (i = 0; i < db->ne; i++) {
        Edge *e = &db->edges[i];
        Str b = {0};
        if (!e->live) continue;
        /* 点を消すときに線も消しているので、ここは起きないはずです。
         * 起きたら索引の作りが壊れているので、入れ替えずに止めます。 */
        if (!newid[e->from - 1] || !newid[e->to - 1]) {
            set_err(db, "内部エラー: 生きている線の端が消えています");
            rc = -1;
            goto done;
        }
        enc_var(&b, ++ne);
        enc_var(&b, newid[e->from - 1]);
        enc_var(&b, newid[e->to - 1]);
        enc_str(&b, e->type);
        enc_attrs(&b, e->attrs, e->nattrs);
        rc = pack_record(db, out, R_EDGE, &b);
        free(b.p);
        if (rc != 0) goto done;
    }
done:
    free(newid);
    *out_nn = nn;
    *out_ne = ne;
    return rc;
}

/*
 * 取り消した記録を、ファイルから本当に落とします。
 * 追記しかしない作りなので、消した点の本文もファイルには残り続けます。
 *
 * 流れは「作業用のファイルへ書く → 開いて読めることを確かめる →
 * 入れ替える → 開き直す」。
 * ★確かめてから入れ替えます。逆にすると、壊れた中身で元を上書きします。
 * ★途中で失敗したら、元のファイルはそのままです（作業用の方を消します）。
 * ★入れ替えの直前に自分の fd を閉じます（Windows は開いたままでは入れ替え
 *   られません）。閉じている間だけ錠が外れるので、ほかから開ける窓が
 *   わずかに空きます。v0 では塞いでいません。
 */
static int repack(yui *db, u64 *nnode, u64 *nedge, u64 *before, u64 *after) {
    Str img = {0}, tmp = {0};
    yui *chk = NULL;
    u64 cn = 0, ce = 0;
    size_t i;
    int fd;

    *before = db->size;
    if (build_packed(db, &img, nnode, nedge) != 0) goto fail;
    str_add(&tmp, db->path);
    str_add(&tmp, PACK_SUFFIX);

    /* 1. 作業用のファイルへ書く。★既に在ったら、消さずに断ります */
    fd = open(tmp.p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            set_err(db,
                    "%s が既にあります。前の詰め直しが途中で終わったのかもしれません。"
                    "中を見てから消してください",
                    tmp.p);
        else
            set_err(db, "詰め直し用のファイルを作れません: %s: %s", tmp.p, strerror(errno));
        goto fail;
    }
    if (write_all(fd, img.p, img.n) != 0) {
        set_err(db, "詰め直し用のファイルに書けません: %s", strerror(errno));
        close(fd);
        goto fail_rm;
    }
    if (fsync(fd) != 0) {
        set_err(db, "詰め直し用のファイルを同期できません: %s", strerror(errno));
        close(fd);
        goto fail_rm;
    }
    if (close(fd) != 0) {
        set_err(db, "詰め直し用のファイルを閉じられません: %s", strerror(errno));
        goto fail_rm;
    }

    /* 2. 入れ替える前に、書いたものが読めるか確かめる */
    if (yui_open(tmp.p, &chk) != YUI_OK) {
        set_err(db, "詰め直したものが読めないので、入れ替えません: %s", yui_errmsg(chk));
        yui_close(chk);
        goto fail_rm;
    }
    for (i = 0; i < chk->nn; i++)
        if (chk->nodes[i].live) cn++;
    for (i = 0; i < chk->ne; i++)
        if (chk->edges[i].live) ce++;
    yui_close(chk);
    if (cn != *nnode || ce != *nedge) {
        set_err(db,
                "詰め直したものの数が合わないので、入れ替えません"
                "（点 %llu のはずが %llu・線 %llu のはずが %llu）",
                (unsigned long long)*nnode, (unsigned long long)cn, (unsigned long long)*nedge,
                (unsigned long long)ce);
        goto fail_rm;
    }

    /* 3. 入れ替える。ここで錠をいったん手放します */
    if (fsync(db->fd) != 0) {
        set_err(db, "元のファイルを同期できません: %s", strerror(errno));
        goto fail_rm;
    }
    close(db->fd);
    db->fd = -1;
    if (rename(tmp.p, db->path) != 0) {
        set_err(db, "入れ替えられません: %s: %s", db->path, strerror(errno));
        /* 元のファイルはそのままなので、開き直して使えるようにします */
        db->fd = open(db->path, O_RDWR, 0644);
        if (db->fd >= 0) {
            flock(db->fd, LOCK_EX | LOCK_NB);
            lseek(db->fd, 0, SEEK_END);
        }
        goto fail_rm;
    }

    /* 4. 新しい中身で、索引ごと作り直す */
    free_state(db);
    db->fd = open(db->path, O_RDWR, 0644);
    if (db->fd < 0) {
        set_err(db, "詰め直したファイルを開き直せません（中身は入れ替わっています）: %s",
                strerror(errno));
        goto fail;
    }
    if (flock(db->fd, LOCK_EX | LOCK_NB) != 0) {
        set_err(db, "詰め直したファイルが別のところで開かれています: %s", db->path);
        close(db->fd);
        db->fd = -1;
        goto fail;
    }
    if (load_file(db) != 0) {
        close(db->fd);
        db->fd = -1;
        goto fail;
    }
    *after = db->size;
    free(img.p);
    free(tmp.p);
    return 0;

fail_rm:
    remove(tmp.p);
fail:
    free(img.p);
    free(tmp.p);
    return -1;
}

/* --------------------------------------------- 土台をまたいで効く錠 */

/*
 * flock（POSIX）と LockFileEx（Windows）は互いを排除せず、wasm の flock は
 * 何もしません。だから土台をまたいで 1 つのファイルを使うときは、flock だけでは
 * 足りません。ここでは「<パス>.錠 を O_EXCL で作る」形を使います。
 * O_EXCL は土台をまたいでも原子的で、同時に試しても 1 つしか成功しません。
 *
 * ★代わりに、持ち主が落ちても外れません。だから結は「持ち主を名乗って断る」
 *   ところまでをやり、壊すかどうかは呼び出した側が決めます。
 *   理由と設計は docs/デーモンの構想.md の「段 0 の決着」に書いてあります。
 */
#define LOCK_SUFFIX ".錠"
#define LOCK_READ_MAX 1024 /* 錠を読むときの上限。これより長い分は見ません */

static char *lock_path_of(const char *path) {
    size_t n = strlen(path);
    char *p = xmalloc(n + sizeof LOCK_SUFFIX);
    memcpy(p, path, n);
    memcpy(p + n, LOCK_SUFFIX, sizeof LOCK_SUFFIX);
    return p;
}

/*
 * 機械の名前。取れなければ "?" にします。
 * ★wasm では意味のある名前が出ません。そのための名乗りです。
 */
static void lock_machine(char *buf, size_t n) {
    buf[0] = 0;
#ifdef _WIN32
    {
        const char *e = getenv("COMPUTERNAME");
        if (e && *e) {
            size_t k = strlen(e);
            if (k >= n) k = n - 1;
            memcpy(buf, e, k);
            buf[k] = 0;
        }
    }
#elif defined(__EMSCRIPTEN__)
    /*
     * ★wasm では機械の名前が取れません（取れても仮想の値です）。
     *   当てにならない値を書くより、当てにならないと書きます。
     *   どのプログラムかは name（呼び出し側の名乗り）で見てください。
     */
    if (n > sizeof "wasm") memcpy(buf, "wasm", sizeof "wasm");
#else
    if (gethostname(buf, n) != 0) buf[0] = 0;
    buf[n - 1] = 0;
#endif
    if (!buf[0]) {
        buf[0] = '?';
        buf[1] = 0;
    }
}

/*
 * 名乗りから制御文字を抜きます。
 * ★改行が入ると錠の形が壊れるので、必ずここを通します。
 */
static void lock_clean(const char *src, char *buf, size_t n) {
    size_t i = 0;
    if (!src) src = "";
    for (; src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        buf[i] = (c < 0x20 || c == 0x7f) ? ' ' : src[i];
    }
    buf[i] = 0;
    if (!buf[0]) {
        buf[0] = '-';
        buf[1] = 0;
    }
}

/*
 * 壊して取ったあと「自分のものか」を読み返して確かめるための印。
 *
 * ★土台によって材料が足りません。wasm では getpid() が 42 固定で、
 *   スタックの番地もインスタンスの間で同じなので、time() だけが動きます。
 *   それだけだと、同じ秒に 2 つが壊して取り合ったとき、両方が
 *   「自分の印が入っている」を見てしまいます（実測で見つかりました）。
 *   そこで、秒より細かい時計と乱数を混ぜます。
 */
/*
 * 少し眠る。★C99 に眠る関数が無いので、土台ごとに用意します。
 */
static void nap_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
#endif
}

/*
 * 次に待つ長さを返します（0 なら、もう待たない）。
 * ★flock の LOCK_NB を外して待つ形にはしません。それだと待ち時間に上限を
 *   作れず、いつまでも返ってこない道ができます。少しずつ間を広げて試します。
 */
static int next_nap(int wait_ms, int *slept, int *step) {
    int s;
    if (*slept >= wait_ms) return 0;
    s = *step;
    if (s > wait_ms - *slept) s = wait_ms - *slept;
    if (s <= 0) return 0;
    *slept += s;
    if (*step < 200) *step *= 2; /* だんだん間を広げます */
    return s;
}

static u64 lock_tag(void) {
    static u64 seq = 0;
    u64 t = (u64)time(NULL);
    u64 p = (u64)(unsigned long)getpid();
    u64 a = (u64)(uintptr_t)&t;
    u64 fine = 0, r = 0;
#ifdef _WIN32
    {
        FILETIME ft; /* 100 ナノ秒きざみ */
        GetSystemTimeAsFileTime(&ft);
        fine = ((u64)ft.dwHighDateTime << 32) | (u64)ft.dwLowDateTime;
    }
#else
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
            fine = (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
        /* ★wasm で効くのはここです。番地も pid も動かないので、乱数だけが頼りです */
        if (getentropy(&r, sizeof r) != 0) r = 0;
    }
#endif
    return r ^ fine ^ (t << 20) ^ (p << 7) ^ a ^ (a >> 19) ^ (++seq << 40);
}

/* 錠の中身。★キーは ASCII です（機械で読む側のため） */
static void lock_body(Str *s, const char *name, u64 tag) {
    char mach[256], nm[256], line[600], when[64];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    lock_machine(mach, sizeof mach);
    lock_clean(name, nm, sizeof nm);
    str_add(s, "yui-lock 1\n");
    snprintf(line, sizeof line, "name=%s\n", nm);
    str_add(s, line);
    snprintf(line, sizeof line, "machine=%s\n", mach);
    str_add(s, line);
    snprintf(line, sizeof line, "pid=%lu\n", (unsigned long)getpid());
    str_add(s, line);
    snprintf(line, sizeof line, "since=%lld\n", (long long)now);
    str_add(s, line);
    if (lt && strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S%z", lt) > 0) {
        snprintf(line, sizeof line, "since-local=%s\n", when);
        str_add(s, line);
    }
    snprintf(line, sizeof line, "tag=%016llx\n", (unsigned long long)tag);
    str_add(s, line);
}

/* 0 = 取れた、1 = 塞がれていた、-1 = 作れなかった */
static int lock_take(yui *db, const char *lpath, const char *name, u64 tag) {
    Str s;
    int fd, rc = 0;
    memset(&s, 0, sizeof s);
    fd = open(lpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return 1;
        set_err(db, "錠を作れません: %s: %s", lpath, strerror(errno));
        return -1;
    }
    lock_body(&s, name, tag);
    if (write_all(fd, s.p, s.n) != 0) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc != 0) {
        set_err(db, "錠に書けません: %s: %s", lpath, strerror(errno));
        remove(lpath); /* 中身の無い錠を残さない */
    }
    free(s.p);
    return rc;
}

#ifdef YUI_SELFCHECK
/*
 * ★試験だけの差し込み口。-DYUI_SELFCHECK はテストのビルドにしか付きません。
 *   錠を取ったあとに別のところが先に取っていた状況を作って、
 *   「読み返して確かめる」が効いているかを見るために使います。
 *   1 プロセスからは本物の取り合いを起こせないので、これが無いと
 *   読み返しの確かめは誰にも守られません。
 */
void (*yui_selfcheck_took_lock)(const char *lpath) = NULL;
#endif

/* 0 = 読めた、1 = 錠が無い、-1 = 読めなかった */
static int lock_slurp(const char *lpath, char *buf, size_t bufn) {
    int fd;
    ssize_t got;
    size_t cap = bufn < LOCK_READ_MAX ? bufn : LOCK_READ_MAX;
    buf[0] = 0;
    if (cap < 2) return -1;
    fd = open(lpath, O_RDONLY, 0644);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    got = read(fd, buf, cap - 1);
    close(fd);
    if (got < 0) {
        buf[0] = 0;
        return -1;
    }
    buf[got] = 0;
    return 0;
}

int yui_lock_owner(const char *path, char *buf, size_t bufn) {
    char *lpath;
    int rc;
    if (!buf || bufn < 2) return YUI_ERROR;
    buf[0] = 0;
    if (!path || !*path) return YUI_ERROR;
    lpath = lock_path_of(path);
    rc = lock_slurp(lpath, buf, bufn);
    free(lpath);
    if (rc < 0) return YUI_ERROR;
    return YUI_OK; /* 錠が無ければ buf は空のまま */
}

/* 器を作るところだけ。開くのは open_body です */
static yui *db_new(yui **out) {
    yui *db = xmalloc(sizeof *db);
    memset(db, 0, sizeof *db);
    db->fd = -1;
    *out = db;
    crc_init(db);
    return db;
}

/*
 * db->path を開いて flock して読み込みます。
 * wait_ms まで待ってから諦めます（0 なら待ちません）。
 * db->shared なら共有の錠を取るので、読む人どうしは待ち合いません。
 */
static int open_body(yui *db, int wait_ms) {
    int op = db->shared ? LOCK_SH : LOCK_EX;
    int slept = 0, step = 20, nap;
    db->fd = open(db->path, O_RDWR | O_CREAT, 0644);
    if (db->fd < 0) {
        set_err(db, "開けません: %s: %s", db->path, strerror(errno));
        return YUI_ERROR;
    }
    while (flock(db->fd, op | LOCK_NB) != 0) {
        nap = next_nap(wait_ms, &slept, &step);
        if (!nap) {
            set_err(db, "別のところで開かれています: %s", db->path);
            goto fail;
        }
        nap_ms(nap);
    }
    if (load_file(db) != 0) goto fail;
    return YUI_OK;
fail:
    close(db->fd);
    db->fd = -1;
    return YUI_ERROR;
}

int yui_open_full(const char *path, const char *name, int flags, int wait_ms, yui **out) {
    yui *db = db_new(out);
    char *lpath;
    char got[LOCK_READ_MAX];
    char want[32];
    u64 tag;
    int rc, slept = 0, step = 20, nap;

    if (!path || !*path) {
        set_err(db, "ファイル名がありません");
        return YUI_ERROR;
    }
    if (wait_ms < 0) wait_ms = 0;
    db->path = xstrdup(path);
    db->readonly = (flags & YUI_READONLY) ? 1 : 0;
    db->shared = (flags & YUI_SHARED) ? 1 : 0;

    /*
     * ★共有で開くなら、読むだけでなければいけません。
     *   共有の錠を持ったまま書くと、同時に読んでいる人が壊れたものを読みます。
     */
    if (db->shared && !db->readonly) {
        set_err(db, "共有で開くなら、読むだけ（YUI_READONLY）も付けてください");
        return YUI_ERROR;
    }

    /*
     * ★<パス>.錠 を使わない道が 2 つあります。
     *   ・YUI_NO_LOCKFILE … 呼ぶ側が外した（短命なプロセスなど。下記）
     *   ・YUI_SHARED      … 共有で開く人が、排他の錠を取るのはおかしいので
     */
    if ((flags & YUI_NO_LOCKFILE) || db->shared) return open_body(db, wait_ms);

    /*
     * ★錠を flock より先に当てます。順番が逆だと、同じ土台どうしの衝突が
     *   flock に先に弾かれて、「持ち主を名乗る」が働きません。
     *   錠を先に取れば、どの土台から来た相手でも持ち主を名乗って断れます。
     */
    lpath = lock_path_of(path);
    tag = lock_tag();
    /* 塞がれていたら、wait_ms まで待って試し直します */
    for (;;) {
        rc = lock_take(db, lpath, name, tag);
        if (rc != 1) break;
        nap = next_nap(wait_ms, &slept, &step);
        if (!nap) break;
        nap_ms(nap);
    }

    if (rc == 1 && (flags & YUI_LOCK_BREAK)) {
        /*
         * ★消してから作り直すまでを、この 1 回の呼び出しの中でやります。
         *   別々の呼び出しにすると、間に他が入れます。
         *   それでも窓は無くせないので、下で読み返して確かめます。
         */
        remove(lpath);
        rc = lock_take(db, lpath, name, tag);
        if (rc == 0) {
#ifdef YUI_SELFCHECK
            if (yui_selfcheck_took_lock) yui_selfcheck_took_lock(lpath);
#endif
            snprintf(want, sizeof want, "tag=%016llx", (unsigned long long)tag);
            if (lock_slurp(lpath, got, sizeof got) != 0 || !strstr(got, want))
                rc = 2; /* 壊して取り直す間に、別のところが先に取った */
        }
    }

    if (rc != 0) {
        if (rc == 1) {
            got[0] = 0;
            lock_slurp(lpath, got, sizeof got);
            set_err(db, "別のところが錠を持っています: %s\n%s", lpath, got);
        } else if (rc == 2) {
            set_err(db, "錠を取り合って負けました: %s", lpath);
        }
        free(lpath);
        return YUI_ERROR;
    }

    /* ★ここから先で失敗しても、yui_close が錠を外します */
    db->lockpath = lpath;
    return open_body(db, wait_ms);
}

/* 下の 2 つは yui_open_full の短い書き方です（中身は 1 つだけ） */
int yui_open_locked(const char *path, const char *name, int flags, yui **out) {
    return yui_open_full(path, name, flags, 0, out);
}
int yui_open(const char *path, yui **out) {
    return yui_open_full(path, NULL, YUI_NO_LOCKFILE, 0, out);
}

const char *yui_errmsg(yui *db) {
    if (!db) return "開かれていません";
    return db->err ? db->err : "";
}
const char *yui_notice(yui *db) { return db ? db->notice : NULL; }

int yui_close(yui *db) {
    int rc = YUI_OK;
    if (!db) return YUI_OK;
    if (db->fd >= 0) {
        if (fsync(db->fd) != 0) rc = YUI_ERROR;
        if (close(db->fd) != 0) rc = YUI_ERROR;
    }
    /* ★錠は閉じるときに必ず外します（落ちたときは残ります。そのための名乗りです） */
    if (db->lockpath) {
        remove(db->lockpath);
        free(db->lockpath);
        db->lockpath = NULL;
    }
    free_state(db);
    free(db->err);
    free(db->notice);
    free(db->path);
    free(db);
    return rc;
}
