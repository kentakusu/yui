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
#ifdef _WIN32
#  include "yui_win.h" /* Windows 用の受け皿。POSIX の低水準 I/O を置き換えます */
#else
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
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

typedef struct {
    u64 key;
    Vec list;
} GramSlot;

struct yui {
    int fd;
    char *path;
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
static void gram_add(yui *db, u64 key, u64 doc) {
    GramSlot *g = gram_slot(db, key, 1);
    if (g->list.n == 0 || g->list.v[g->list.n - 1] != doc) vec_push(&g->list, doc);
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
        const Vec *best = NULL;
        if (qn == 2) {
            GramSlot *g = gram_slot(db, gram2(q[0], q[1]), 0);
            if (!g) return;
            best = &g->list;
        } else {
            for (i = 0; i + 2 < qn; i++) {
                GramSlot *g = gram_slot(db, gram3(q[i], q[i + 1], q[i + 2]), 0);
                if (!g) return; /* 1 つでも無ければ、どこにも無い */
                if (!best || g->list.n < best->n) best = &g->list;
            }
        }
        for (i = 0; i < best->n; i++)
            if (doc_has(db, best->v[i], q, qn)) vec_push(out, best->v[i]);
    } else {
        for (i = 1; i <= db->nn; i++)
            if (doc_has(db, DOC_NODE(i), q, qn)) vec_push(out, DOC_NODE(i));
        for (i = 1; i <= db->ne; i++)
            if (doc_has(db, DOC_EDGE(i), q, qn)) vec_push(out, DOC_EDGE(i));
    }
    vec_sort_unique(out);
}

/* ---------------------------------------------------------------- メモリへ適用 */

static Attr *attrs_dup(const Attr *a, size_t n) {
    Attr *r = xmalloc(n * sizeof(Attr));
    size_t i;
    for (i = 0; i < n; i++) {
        r[i] = a[i];
        r[i].key = xstrdup(a[i].key);
        if (a[i].v.type == V_STR) r[i].v.s = xstrdup(a[i].v.s);
    }
    return r;
}
static void attrs_free(Attr *a, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        free(a[i].key);
        if (a[i].v.type == V_STR) free(a[i].v.s);
    }
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
static void print_node(yui *db, Str *o, u64 id, int with_attrs) {
    Node *x = &db->nodes[id - 1];
    str_chr(o, '(');
    print_word(o, x->label);
    str_chr(o, ':');
    print_word(o, x->name);
    if (with_attrs) print_attrs(o, x->attrs, x->nattrs);
    str_chr(o, ')');
}
/* 種類の字句。「3段」のような語は段数と読まれるので引用符で囲む */
static int parse_depth(const char *w, long *out);
static void print_type(Str *o, const char *t) {
    long d;
    if (parse_depth(t, &d)) print_quoted(o, t);
    else print_word(o, t);
}
static void print_edge_step(Str *o, const Edge *e, int forward) {
    str_add(o, forward ? "-[" : "<-[");
    print_type(o, e->type);
    print_attrs(o, e->attrs, e->nattrs);
    str_add(o, forward ? "]->" : "]-");
}
static void print_edge(yui *db, Str *o, u64 id) {
    Edge *e = &db->edges[id - 1];
    print_node(db, o, e->from, 0);
    print_edge_step(o, e, 1);
    print_node(db, o, e->to, 0);
}

static void emit(yui *db, int kind, const char *text) {
    if (db->stopped || !db->cb) return;
    if (db->cb(db->ctx, kind, text)) db->stopped = 1;
}
static void emit_str(yui *db, Str *o) {
    emit(db, YUI_ROW, o->p ? o->p : "");
    str_clear(o);
}
static void emit_count(yui *db, size_t n, const char *suffix) {
    char b[80];
    snprintf(b, sizeof b, "%llu 件%s", (unsigned long long)n, suffix);
    emit(db, YUI_SUMMARY, b);
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
            print_node(db, &o, v.v[i], 0);
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

static int cmd_make(P *p) {
    yui *db = p->db;
    NodePat n;
    u64 id;
    int st;
    Str o = {0};
    if (parse_node(p, &n, 1) != 0) return -1;
    if (ensure_node(db, &n, &id, &st) != 0) return -1;
    print_node(db, &o, id, 1);
    emit_str(db, &o);
    free(o.p);
    emit(db, YUI_SUMMARY, status_word(st));
    return 0;
}

static int cmd_link(P *p) {
    yui *db = p->db;
    NodePat a, b;
    EdgePat e;
    u64 ia, ib, ie, cur;
    int sa, sb, st;
    Str o = {0};
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
    print_edge(db, &o, ie);
    emit_str(db, &o);
    free(o.p);
    emit(db, YUI_SUMMARY, status_word(st));
    return 0;
}

static void emit_doc(yui *db, u64 doc) {
    Str o = {0};
    if (doc & 1) print_edge(db, &o, doc >> 1);
    else print_node(db, &o, doc >> 1, 1);
    emit_str(db, &o);
    free(o.p);
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
    if (parse_text(p, &q, &quoted) != 0) return -1;
    if (!*q) return perr(p, "探す文字を書いてください（例: 探す \"既読\"）");
    if (peek(p) == '(') {
        if (parse_node(p, &f, 0) != 0) return -1;
        has_filter = 1;
    }
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
        u64 doc = hits.v[i];
        if (has_filter) {
            if (doc & 1) {
                Edge *e = &db->edges[(doc >> 1) - 1];
                if (!node_matches(&db->nodes[e->from - 1], &f) &&
                    !node_matches(&db->nodes[e->to - 1], &f))
                    continue;
            } else if (!node_matches(&db->nodes[(doc >> 1) - 1], &f)) {
                continue;
            }
        }
        emit_doc(db, doc);
        count++;
    }
    free(hits.v);
    emit_count(db, count, "");
    return 0;
}

/* 起点から終点までの道を、たどった順に書く */
static void print_path(yui *db, Str *o, u64 start, u64 last_node, u64 edge, u64 next) {
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
    print_node(db, o, start, 0);
    cur = start;
    for (i = nodes.n; i > 0; i--) {
        Edge *e = &db->edges[edges.v[i - 1] - 1];
        u64 nx = nodes.v[i - 1];
        print_edge_step(o, e, e->from == cur && e->to == nx);
        print_node(db, o, nx, 0);
        cur = nx;
    }
    free(nodes.v);
    free(edges.v);
}

static int cmd_walk(P *p) {
    yui *db = p->db;
    NodePat a, b;
    EdgePat e;
    Vec starts = {0}, queue = {0};
    Str o = {0};
    size_t s, count = 0;
    long maxd;
    if (parse_node(p, &a, 0) != 0) return -1;
    if (parse_edge(p, &e, 0, 1) != 0) return -1;
    if (parse_node(p, &b, 0) != 0) return -1;
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
                        print_path(db, &o, st, u, eid, v);
                        emit_str(db, &o);
                        count++;
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
    free(starts.v);
    free(queue.v);
    free(o.p);
    emit_count(db, count, "");
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
        print_node(db, &o, v.v[i], 1);
        emit_str(db, &o);
        for (j = 0; j < x->out.n; j++) {
            Edge *e = &db->edges[x->out.v[j] - 1];
            if (!e->live) continue;
            str_add(&o, "  ");
            print_edge_step(&o, e, 1);
            print_node(db, &o, e->to, 0);
            emit_str(db, &o);
        }
        for (j = 0; j < x->in.n; j++) {
            Edge *e = &db->edges[x->in.v[j] - 1];
            if (!e->live || e->from == e->to) continue;
            str_add(&o, "  ");
            print_edge_step(&o, e, 0);
            print_node(db, &o, e->from, 0);
            emit_str(db, &o);
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
    size_t i;
    memset(&n, 0, sizeof n);
    if (peek(p) == '(' && parse_node(p, &n, 0) != 0) return -1;
    collect_nodes(db, &n, &v);
    for (i = 0; i < v.n && !db->stopped; i++) emit_doc(db, DOC_NODE(v.v[i]));
    emit_count(db, v.n, "");
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
    "消す (人:田中)                                    点とその線を消す",
    "消す (案件:結の開発)-[担当]->(人:田中)            線を消す",
    "詰め直す                                          取り消した記録をファイルから落とす",
    "同期                                              ファイルへ確実に書き出す",
    "終わる                                            終了",
    "点の書き方: (名前)  (ラベル:名前)  (ラベル:)  (?なんでも)  ()",
    NULL};

/* 1 文を実行。空の文なら何もしない。戻り値は YUI_* */
static int exec_stmt(P *p) {
    yui *db = p->db;
    char *kw;
    int rc = 0, s;
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
    if (strcmp(kw, "作る") == 0) rc = cmd_make(p);
    else if (strcmp(kw, "結ぶ") == 0) rc = cmd_link(p);
    else if (strcmp(kw, "探す") == 0) rc = cmd_search(p);
    else if (strcmp(kw, "辿る") == 0) rc = cmd_walk(p);
    else if (strcmp(kw, "見る") == 0) rc = cmd_show(p);
    else if (strcmp(kw, "一覧") == 0) rc = cmd_list(p);
    else if (strcmp(kw, "消す") == 0) rc = cmd_delete(p);
    else if (strcmp(kw, "詰め直す") == 0) rc = cmd_repack(p);
    else if (strcmp(kw, "同期") == 0) {
        if (fsync(db->fd) != 0) {
            set_err(db, "同期に失敗しました: %s", strerror(errno));
            return YUI_ERROR;
        }
        emit(db, YUI_SUMMARY, "同期しました");
    } else if (strcmp(kw, "手引き") == 0) {
        int i;
        for (i = 0; HELP[i]; i++) emit(db, YUI_ROW, HELP[i]);
    } else if (strcmp(kw, "終わる") == 0) {
        return YUI_END;
    } else {
        set_err(db, "知らない言葉です: %s（「手引き」で一覧を出せます）", kw);
        return YUI_ERROR;
    }
    if (rc != 0) return YUI_ERROR;
    s = peek(p);
    if (s != 0 && s != '\n' && s != '\x01') {
        perr(p, "文のあとに余分な文字があります");
        return YUI_ERROR;
    }
    return YUI_OK;
}

int yui_exec(yui *db, const char *query, yui_cb cb, void *ctx) {
    P p;
    int rc = YUI_OK;
    if (!db || db->fd < 0) return YUI_ERROR;
    free(db->err);
    db->err = NULL;
    if (!query) query = "";
    if (!utf8_valid(query)) {
        set_err(db, "問い合わせが UTF-8 として正しくありません");
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
    return rc;
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
    for (i = 0; i < db->gramcap; i++) free(db->grams[i].list.v);
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
        if (write_all(db->fd, MAGIC, MAGIC_LEN) != 0) {
            set_err(db, "書けません: %s", strerror(errno));
            return -1;
        }
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

int yui_open(const char *path, yui **out) {
    yui *db = xmalloc(sizeof *db);
    memset(db, 0, sizeof *db);
    db->fd = -1;
    *out = db;
    crc_init(db);
    if (!path || !*path) {
        set_err(db, "ファイル名がありません");
        return YUI_ERROR;
    }
    db->path = xstrdup(path);
    db->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (db->fd < 0) {
        set_err(db, "開けません: %s: %s", path, strerror(errno));
        return YUI_ERROR;
    }
    if (flock(db->fd, LOCK_EX | LOCK_NB) != 0) {
        set_err(db, "別のところで開かれています: %s", path);
        goto fail;
    }
    if (load_file(db) != 0) goto fail;
    return YUI_OK;
fail:
    close(db->fd);
    db->fd = -1;
    return YUI_ERROR;
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
    free_state(db);
    free(db->err);
    free(db->notice);
    free(db->path);
    free(db);
    return rc;
}
