/* pawnsave - decompress DDDA.sav and extract hired-pawn creator+gear state.
 *
 * DDDA.sav format:
 *   u32 version (21)
 *   u32 realSize            <- decompressed payload size
 *   u32 compressedSize
 *   u32 magic1 (860693325)
 *   u32 zero
 *   u32 magic2 (860700740)
 *   u32 crc32jam hash
 *   u32 magic3 (1079398965)
 *   [32 bytes header above]
 *   zlib-deflated XML of the save tree (~20 MB when inflated)
 *
 * XML layout we care about (top-level):
 *   <class type="cSAVE_DATA_CMC">   (x3 at the front: MainPawn, Hired1, Hired2)
 *       ...
 *       <class name="mArisenName" type="cName">
 *           ...
 *           <array name="( u8* )mEditName" type="u8" count="25">
 *               <u8 value="N"/> ... (ASCII codes, NUL-padded)
 *           </array>
 *           ...
 *       </class>
 *       ...
 *       <array name="mEquipItem" ...>12x cITEM_PARAM_DATA</array>
 *   </class>
 *
 * Hired-pawn iteration is scoped to the FIRST `<array name="mCmc" type=
 * "class" count="3">` only — its three children are mCmc[0]=main pawn,
 * mCmc[1]=Hired1, mCmc[2]=Hired2, by position. The save also contains
 * unrelated cSAVE_DATA_CMC blocks elsewhere (mCloseFriendPawn rift cache,
 * mPlayerDataAuto checkpoint copy, etc.); those carry stale data and must
 * not be treated as hired-slot state.
 *
 * The save format was validated end-to-end by tools/save_to_archive.py. */

#include "pawnsave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/* ===========================================================================
 * Minimal XML primitives, scoped to DDDA.sav's patterns.
 *
 * No DOM, no entity handling, no attribute-order tolerance beyond what the
 * save actually uses. These are composable enough to extend later (study
 * data lives in <array name="mStudyData.KillCnt" type="u32" count="72">…
 * see gear-persistence.md). Everything is substring search on bounded
 * spans, so each call is O(span_length). Within a ≤5 KB array body the
 * cost is trivial; scanning the full ~20 MB XML for the top-level array
 * anchor is a single memmem-style pass per query.
 * ===========================================================================
 */
typedef struct {
    const char *p;
    const char *end;
} xspan;

static const char *xfind(xspan in, const char *needle)
{
    size_t nlen = strlen(needle);
    if ((size_t)(in.end - in.p) < nlen) return NULL;
    const char *last = in.end - nlen;
    for (const char *p = in.p; p <= last; p++) {
        if (memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/* Find a self-closing <TAG name="NAME" value="V"/> and parse V as a signed
 * long. Matches the exact `name="NAME" value="` substring, so order-of-
 * attributes or odd spacing would miss — that's fine for our fixed save
 * format. Unfindable fields return -1; caller checks the return. */
static int xml_get_long(xspan in, const char *name, long *out)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "name=\"%s\" value=\"", name);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;
    const char *hit = xfind(in, needle);
    if (!hit) return -1;
    const char *val = hit + n;
    const char *q = memchr(val, '"', (size_t)(in.end - val));
    if (!q) return -1;
    char buf[32];
    size_t len = (size_t)(q - val);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, val, len);
    buf[len] = 0;
    char *endp;
    long v = strtol(buf, &endp, 10);
    if (endp == buf) return -1;
    *out = v;
    return 0;
}

/* Iterate every <array name="NAME" ...>body</array>. Callback receives the
 * inner body span and the 0-based match index. Return non-zero from the
 * callback to stop iteration early. */
typedef int (*xml_array_cb)(xspan body, int index, void *ctx);

static int xml_for_each_array(xspan in, const char *name,
                              xml_array_cb cb, void *ctx)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "<array name=\"%s\"", name);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    xspan remaining = in;
    int idx = 0;
    while (1) {
        const char *start = xfind(remaining, needle);
        if (!start) return 0;
        /* Skip past the '>' that closes the <array ...> opening tag. */
        const char *gt = memchr(start + n, '>', (size_t)(in.end - (start + n)));
        if (!gt) return 0;
        const char *body_start = gt + 1;
        /* mEquipItem, mStudyData.* etc. don't nest <array> inside <array>
         * in this save, so the first </array> after body_start is ours. */
        xspan after = { body_start, in.end };
        const char *close = xfind(after, "</array>");
        if (!close) return 0;
        xspan body = { body_start, close };
        int r = cb(body, idx++, ctx);
        if (r) return r;
        remaining.p = close + 8;  /* len of "</array>" */
    }
}

/* Iterate every <class type="TYPE">body</class> inside a span. Matches only
 * the bare `<class type="...">` form — the name-prefixed variant
 * `<class name="..." type="...">` is intentionally skipped, which is what
 * lets us scope top-level cSAVE_DATA_CMC blocks without hitting the nested
 * mKaiouData / mKaiouPornData sub-classes. */
typedef int (*xml_class_cb)(xspan body, int index, void *ctx);

static int xml_for_each_class(xspan in, const char *type,
                              xml_class_cb cb, void *ctx)
{
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "<class type=\"%s\">", type);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    xspan remaining = in;
    int idx = 0;
    while (1) {
        const char *start = xfind(remaining, needle);
        if (!start) return 0;
        const char *body_start = start + n;
        /* Nested <class> inside a cSAVE_DATA_CMC body (mArisenName, item
         * records, …) means we can't just find the first </class>. Walk
         * with a depth counter, recognising both the bare form and the
         * name-prefixed form as openings. */
        int depth = 1;
        const char *q = body_start;
        const char *close = NULL;
        while (q < in.end) {
            const char *lt = memchr(q, '<', (size_t)(in.end - q));
            if (!lt) break;
            if ((size_t)(in.end - lt) >= 8 && memcmp(lt, "</class>", 8) == 0) {
                if (--depth == 0) { close = lt; break; }
                q = lt + 8;
            } else if ((size_t)(in.end - lt) >= 12 &&
                       memcmp(lt, "<class name=", 12) == 0) {
                depth++;
                q = lt + 12;
            } else if ((size_t)(in.end - lt) >= 12 &&
                       memcmp(lt, "<class type=", 12) == 0) {
                depth++;
                q = lt + 12;
            } else {
                q = lt + 1;
            }
        }
        if (!close) return 0;
        xspan body = { body_start, close };
        int r = cb(body, idx++, ctx);
        if (r) return r;
        remaining.p = close + 8;  /* len of "</class>" */
    }
}

/* Locate the body span of `<array name="NAME" type="TYPE" ...>…</array>`.
 * Returns 0 on success with *out_body set to the bytes between the opening
 * tag's `>` and the matching `</array>`. Returns 1 if absent. Returns -1 on
 * over-long name (cannot construct the search needle). */
static int xml_find_typed_array_body(xspan in, const char *name,
                                     const char *type, xspan *out_body)
{
    char opener[96];
    int n = snprintf(opener, sizeof(opener),
                     "<array name=\"%s\" type=\"%s\"", name, type);
    if (n <= 0 || n >= (int)sizeof(opener)) return -1;
    const char *start = xfind(in, opener);
    if (!start) return 1;
    const char *gt = memchr(start + n, '>', (size_t)(in.end - (start + n)));
    if (!gt) return 1;
    xspan body = { gt + 1, in.end };
    const char *close = xfind(body, "</array>");
    if (!close) return 1;
    body.end = close;
    *out_body = body;
    return 0;
}

/* Walk `<ELEM value="X"/>` children of a body span, copying each value's text
 * (between the opening `"` and closing `"`) into a caller-supplied small
 * buffer and invoking `parse` to convert it. Stops after `count` elements
 * or when the body runs out of matching tags. Returns 0 on success. */
typedef void (*xml_value_parse_cb)(const char *text, void *out_array, int idx);

static int xml_walk_value_tags(xspan body, const char *elem,
                               int count, void *out_array,
                               xml_value_parse_cb parse)
{
    char tag[16];
    int tn = snprintf(tag, sizeof(tag), "<%s value=\"", elem);
    if (tn <= 0 || tn >= (int)sizeof(tag)) return -1;
    size_t tlen = (size_t)tn;

    const char *p = body.p;
    int idx = 0;
    while (idx < count) {
        xspan rem = { p, body.end };
        const char *hit = xfind(rem, tag);
        if (!hit) break;
        const char *v = hit + tlen;
        const char *q = memchr(v, '"', (size_t)(body.end - v));
        if (!q) break;
        char buf[32];
        size_t len = (size_t)(q - v);
        if (len == 0 || len >= sizeof(buf)) break;
        memcpy(buf, v, len);
        buf[len] = 0;
        parse(buf, out_array, idx++);
        p = q + 1;
    }
    return 0;
}

static void parse_u32_elem(const char *t, void *arr, int idx)
{
    ((uint32_t *)arr)[idx] = (uint32_t)strtoul(t, NULL, 10);
}
static void parse_f32_elem(const char *t, void *arr, int idx)
{
    ((float *)arr)[idx] = strtof(t, NULL);
}
static void parse_u8_elem(const char *t, void *arr, int idx)
{
    ((uint8_t *)arr)[idx] = (uint8_t)strtoul(t, NULL, 10);
}

/* Fill `out[0..count-1]` with values from `<array name="NAME" type="u32" count="N">`
 * by walking its inner `<u32 value="N"/>` children in order. Missing entries
 * stay zero (callers should pre-zero the buffer if they care). Returns 0 on
 * success — including when the array is absent — so a pawn that simply has
 * no knowledge yet doesn't fail the parse. */
static int xml_get_u32_array(xspan in, const char *name, uint32_t *out, int count)
{
    xspan body;
    int r = xml_find_typed_array_body(in, name, "u32", &body);
    if (r != 0) return r < 0 ? -1 : 0;
    return xml_walk_value_tags(body, "u32", count, out, parse_u32_elem);
}

/* f32 sibling of xml_get_u32_array. Output element type is `float`; absent
 * array → no writes (callers pre-zero). */
static int xml_get_f32_array(xspan in, const char *name, float *out, int count)
{
    xspan body;
    int r = xml_find_typed_array_body(in, name, "f32", &body);
    if (r != 0) return r < 0 ? -1 : 0;
    return xml_walk_value_tags(body, "f32", count, out, parse_f32_elem);
}

/* u8 sibling of xml_get_u32_array. Output element type is `uint8_t`; absent
 * array → no writes (callers pre-zero). Distinct from xml_get_cname, which
 * stops at the first 0 byte to recover a NUL-terminated string. */
static int xml_get_u8_array(xspan in, const char *name, uint8_t *out, int count)
{
    xspan body;
    int r = xml_find_typed_array_body(in, name, "u8", &body);
    if (r != 0) return r < 0 ? -1 : 0;
    return xml_walk_value_tags(body, "u8", count, out, parse_u8_elem);
}

/* Read a <class name="NAME" type="cName">…</class> sub-block: the
 * `mEditName` u8 array inside it holds the ASCII-coded name, NUL-padded to
 * 25 bytes. Writes up to out_cap-1 chars + NUL into out; returns 0 on
 * success, -1 if the class or inner array wasn't found. */
static int xml_get_cname(xspan in, const char *name, char *out, size_t out_cap)
{
    if (out_cap == 0) return -1;
    out[0] = 0;

    char opener[96];
    int n = snprintf(opener, sizeof(opener),
                     "<class name=\"%s\" type=\"cName\">", name);
    if (n <= 0 || n >= (int)sizeof(opener)) return -1;
    const char *start = xfind(in, opener);
    if (!start) return -1;
    const char *body_start = start + n;
    xspan after = { body_start, in.end };
    const char *close = xfind(after, "</class>");
    if (!close) return -1;
    xspan body = { body_start, close };

    /* Locate the u8 array. The name attribute contains literal parens, so
     * we match the whole opening-tag substring. */
    const char *arr = xfind(body, "<array name=\"( u8* )mEditName\"");
    if (!arr) return -1;
    const char *gt = memchr(arr, '>', (size_t)(body.end - arr));
    if (!gt) return -1;
    xspan arr_body = { gt + 1, body.end };
    const char *arr_end = xfind(arr_body, "</array>");
    if (!arr_end) return -1;
    arr_body.end = arr_end;

    /* Walk the <u8 value="N"/> entries in order, decoding each into a byte. */
    size_t w = 0;
    const char *p = arr_body.p;
    const char *tag = "<u8 value=\"";
    size_t tlen = strlen(tag);
    while (w + 1 < out_cap) {
        xspan rem = { p, arr_body.end };
        const char *hit = xfind(rem, tag);
        if (!hit) break;
        const char *v = hit + tlen;
        const char *q = memchr(v, '"', (size_t)(arr_body.end - v));
        if (!q) break;
        char buf[8];
        size_t len = (size_t)(q - v);
        if (len == 0 || len >= sizeof(buf)) break;
        memcpy(buf, v, len);
        buf[len] = 0;
        int c = atoi(buf);
        if (c == 0) break;                            /* C-string NUL terminator */
        if (c < 0 || c > 127) break;                  /* ASCII-only by construction */
        out[w++] = (char)c;
        p = q + 1;
    }
    out[w] = 0;
    return 0;
}

/* ===========================================================================
 * Gear-specific extraction.
 * ===========================================================================
 */
static int parse_gear_record(xspan body, struct sav_gear_record *rec)
{
    long v;
    if (xml_get_long(body, "data.mNum",          &v) < 0) return -1;
    rec->mNum = (int16_t)v;
    if (xml_get_long(body, "data.mItemNo",       &v) < 0) return -1;
    rec->mItemNo = (int16_t)v;
    if (xml_get_long(body, "data.mFlag",         &v) < 0) return -1;
    rec->mFlag = (uint32_t)v;
    if (xml_get_long(body, "data.mChgNum",       &v) < 0) return -1;
    rec->mChgNum = (uint16_t)v;
    if (xml_get_long(body, "data.mDay1",         &v) < 0) return -1;
    rec->mDay1 = (uint16_t)v;
    if (xml_get_long(body, "data.mDay2",         &v) < 0) return -1;
    rec->mDay2 = (uint16_t)v;
    if (xml_get_long(body, "data.mDay3",         &v) < 0) return -1;
    rec->mDay3 = (uint16_t)v;
    if (xml_get_long(body, "data.mMutationPool", &v) < 0) return -1;
    rec->mMutationPool = (int8_t)v;
    if (xml_get_long(body, "data.mOwnerId",      &v) < 0) return -1;
    rec->mOwnerId = (int8_t)v;
    if (xml_get_long(body, "data.mKey",          &v) < 0) return -1;
    rec->mKey = (uint32_t)v;
    return 0;
}

struct class_iter_ctx {
    struct sav_gear_record recs[12];
    int count;
};

static int class_cb(xspan body, int idx, void *ctxp)
{
    struct class_iter_ctx *c = ctxp;
    if (idx >= 12) return 1;                      /* stop — more than a full array */
    if (parse_gear_record(body, &c->recs[idx]) < 0) {
        /* Malformed record: treat as empty. */
        memset(&c->recs[idx], 0, sizeof(c->recs[idx]));
        c->recs[idx].mItemNo = -1;
    }
    c->count = idx + 1;
    return 0;
}

struct array_gear_ctx {
    struct sav_gear_record *recs;                 /* points into c->recs */
    int have_gear;
};

static int array_gear_cb(xspan body, int idx, void *ctxp)
{
    (void)idx;
    struct array_gear_ctx *c = ctxp;
    struct class_iter_ctx inner;
    memset(&inner, 0, sizeof(inner));
    for (int i = 0; i < 12; i++) inner.recs[i].mItemNo = -1;

    xml_for_each_class(body, "sItemManager::cITEM_PARAM_DATA", class_cb, &inner);
    if (inner.count < 12) return 0;               /* skip short arrays */

    memcpy(c->recs, inner.recs, sizeof(inner.recs));
    c->have_gear = 1;
    return 1;                                     /* first mEquipItem only */
}

/* ===========================================================================
 * Top-level cSAVE_DATA_CMC iteration.
 * ===========================================================================
 */
struct cmc_iter_ctx {
    struct pawnsave_hired_info *out;              /* two-entry array */
};

static int cmc_cb(xspan body, int idx, void *ctxp)
{
    struct cmc_iter_ctx *c = ctxp;

    /* Position in the mCmc array IS the slot identifier:
     *   idx 0 = main pawn        — skip (handled by pawnsave_extract_main_pawn_xml)
     *   idx 1 = Hired1           -> out[0]
     *   idx 2 = Hired2           -> out[1]
     *   idx >= 3 — past mCmc count=3, stop iterating.
     * Caller pre-anchors the iterator on the first mCmc array, but
     * xml_for_each_class continues past mCmc's </array> into unrelated
     * blocks (mCloseFriendPawn, mPlayerDataAuto, …); the idx >= 3 guard
     * is what scopes us back. */
    if (idx == 0) return 0;
    if (idx >= 3) return 1;
    int slot = idx - 1;

    /* Always capture the slot — even when gear is fully empty (stripped
     * pawn) and even when the slot holds no live pawn (mArisenName='---').
     * Downstream filters (parse_arisen_stem + runtime alive flag) handle
     * the dead/unhired cases. */
    struct sav_gear_record recs[12];
    for (int i = 0; i < 12; i++) { memset(&recs[i], 0, sizeof(recs[i])); recs[i].mItemNo = -1; }
    struct array_gear_ctx gctx = { .recs = recs, .have_gear = 0 };
    xml_for_each_array(body, "mEquipItem", array_gear_cb, &gctx);

    c->out[slot].present = 1;
    memcpy(c->out[slot].gear, recs, sizeof(recs));
    xml_get_cname(body, "mArisenName",
                  c->out[slot].creator_name,
                  sizeof(c->out[slot].creator_name));

    /* mStudyFlag / mLocalStudyFlag: 322-entry u32 arrays that round-trip
     * verbatim between save XML and the .pawn archive's two `0x142`-tagged
     * structs. Together they cover every knowledge category — monsters,
     * vocations, areas, quests — so copying them whole preserves the lot. */
    xml_get_u32_array(body, "mStudyFlag",      c->out[slot].study_flag,       322);
    xml_get_u32_array(body, "mLocalStudyFlag", c->out[slot].local_study_flag, 322);

    /* mStudyData.{EncountFrame,KillCnt,UniqueCnt}: progression counters
     * sitting immediately after mLocalStudyFlag in the archive. Without
     * these, the bit-flips persisted via mStudyFlag survive a release/re-hire
     * but the partial progress that drives the *next* flip does not. */
    xml_get_f32_array(body, "mStudyData.EncountFrame", c->out[slot].study_encount_frame, 72);
    xml_get_u32_array(body, "mStudyData.KillCnt",      c->out[slot].study_kill_cnt,      72);
    xml_get_u8_array (body, "mStudyData.UniqueCnt",    c->out[slot].study_unique_cnt,   116);

    /* Heap-copy the full <class type="cSAVE_DATA_CMC">…</class> bytes for
     * the .xml sidecar. xml_for_each_class hands us the body span (between
     * the opening `>` of the type tag and the `<` of </class>); the outer
     * element extends by len("<class type=\"cSAVE_DATA_CMC\">") before
     * body.p and len("</class>") after body.end. Caller frees region_xml
     * with free(). */
    {
        static const char CMC_OPEN_TAG[] = "<class type=\"cSAVE_DATA_CMC\">";
        static const char CMC_CLOSE_TAG[] = "</class>";
        const char *outer_start = body.p - (sizeof(CMC_OPEN_TAG) - 1);
        const char *outer_end   = body.end + (sizeof(CMC_CLOSE_TAG) - 1);
        size_t outer_len = (size_t)(outer_end - outer_start);
        uint8_t *region = (uint8_t *)malloc(outer_len);
        if (region) {
            memcpy(region, outer_start, outer_len);
            c->out[slot].region_xml = region;
            c->out[slot].region_xml_len = outer_len;
        }
    }

    /* Stop once both slots have been filled. */
    return (c->out[0].present && c->out[1].present) ? 1 : 0;
}

/* ===========================================================================
 * Main-pawn region locator (Path B sidecar / restore_pawn).
 *
 * The first <array name="mCmc" type="class" count="3"> in the inflated save
 * holds three pawn slots; index 0 is the player's main pawn. The bare
 * <class type="cSAVE_DATA_CMC"> opener (no `name=` prefix) only appears
 * inside mCmc arrays, so the find sequence is:
 *   1. anchor on the first <array name="mCmc"...> opener,
 *   2. take the first <class type="cSAVE_DATA_CMC"> after that anchor,
 *   3. depth-walk to the matching </class>.
 * The returned span starts at the `<` of the opener and ends just past the
 * `>` of the close. ===========================================================
 */
/* Single-region finder with a resumable start offset. The public singular
 * and plural APIs both go through this. `search_start` is in bytes from
 * xml; the first `<array name="mCmc"...>` opener at or after `search_start`
 * is consumed. Returns 0 / fills *out_off and *out_len, or -1 if no further
 * region is found. */
static int find_main_pawn_region_from(const uint8_t *xml, size_t xml_len,
                                      size_t search_start,
                                      size_t *out_off, size_t *out_len)
{
    if (search_start >= xml_len) return -1;
    xspan whole = { (const char *)xml, (const char *)xml + xml_len };
    xspan from  = { (const char *)xml + search_start, whole.end };

    const char *arr_open = xfind(from, "<array name=\"mCmc\" type=\"class\" count=\"3\">");
    if (!arr_open) return -1;
    xspan after_arr = { arr_open, whole.end };

    const char *cmc_tag = "<class type=\"cSAVE_DATA_CMC\">";
    size_t cmc_tag_len = strlen(cmc_tag);
    const char *cmc_open = xfind(after_arr, cmc_tag);
    if (!cmc_open) return -1;

    int depth = 1;
    const char *q = cmc_open + cmc_tag_len;
    const char *close = NULL;
    while (q < whole.end) {
        const char *lt = memchr(q, '<', (size_t)(whole.end - q));
        if (!lt) break;
        if ((size_t)(whole.end - lt) >= 8 && memcmp(lt, "</class>", 8) == 0) {
            if (--depth == 0) { close = lt; break; }
            q = lt + 8;
        } else if ((size_t)(whole.end - lt) >= 12 &&
                   memcmp(lt, "<class name=", 12) == 0) {
            depth++;
            q = lt + 12;
        } else if ((size_t)(whole.end - lt) >= 12 &&
                   memcmp(lt, "<class type=", 12) == 0) {
            depth++;
            q = lt + 12;
        } else {
            q = lt + 1;
        }
    }
    if (!close) return -1;

    size_t off = (size_t)((const uint8_t *)cmc_open - xml);
    size_t end = (size_t)((const uint8_t *)close - xml) + 8;  /* len("</class>") */
    *out_off = off;
    *out_len = end - off;
    return 0;
}

int pawnsave_find_main_pawn_region(const uint8_t *xml, size_t xml_len,
                                   size_t *out_off, size_t *out_len)
{
    if (!xml || !out_off || !out_len) return -1;
    return find_main_pawn_region_from(xml, xml_len, 0, out_off, out_len);
}

int pawnsave_find_main_pawn_regions(const uint8_t *xml, size_t xml_len,
                                    struct pawnsave_region *out, int cap,
                                    int *out_count)
{
    if (!xml || !out || cap <= 0 || !out_count) return -1;
    *out_count = 0;
    size_t start = 0;
    while (*out_count < cap) {
        size_t off, len;
        if (find_main_pawn_region_from(xml, xml_len, start, &off, &len) != 0) break;
        out[*out_count].off = off;
        out[*out_count].len = len;
        (*out_count)++;
        start = off + len;
    }
    return 0;
}

int pawnsave_extract_main_pawn_xml(const void *save_bytes, int32_t save_len,
                                   uint8_t **out_xml, size_t *out_xml_len)
{
    if (!save_bytes || save_len < 32 || !out_xml || !out_xml_len) return -1;
    *out_xml = NULL;
    *out_xml_len = 0;

    const uint8_t *b = save_bytes;
    uint32_t real_size, comp_size;
    memcpy(&real_size, b + 4, 4);
    memcpy(&comp_size, b + 8, 4);
    if (comp_size == 0 || comp_size > (uint32_t)(save_len - 32)) return -1;
    if (real_size == 0 || real_size > 64u * 1024u * 1024u) return -1;

    uint8_t *xml = (uint8_t *)malloc(real_size);
    if (!xml) return -2;
    unsigned long out_len = real_size;
    int rc = uncompress(xml, &out_len, b + 32, comp_size);
    if (rc != 0 || out_len != real_size) {
        free(xml);
        return -2;
    }

    size_t off = 0, len = 0;
    if (pawnsave_find_main_pawn_region(xml, out_len, &off, &len) != 0) {
        free(xml);
        return -3;
    }

    uint8_t *region = (uint8_t *)malloc(len);
    if (!region) {
        free(xml);
        return -2;
    }
    memcpy(region, xml + off, len);
    free(xml);

    *out_xml = region;
    *out_xml_len = len;
    return 0;
}

/* ===========================================================================
 * Public entry point.
 * ===========================================================================
 */
int pawnsave_read_hired(const void *save_bytes, int32_t save_len,
                        struct pawnsave_hired_info out[2])
{
    if (!save_bytes || save_len < 32 || !out) return -1;

    const uint8_t *b = save_bytes;
    uint32_t real_size, comp_size;
    memcpy(&real_size, b + 4, 4);
    memcpy(&comp_size, b + 8, 4);
    if (comp_size == 0 || comp_size > (uint32_t)(save_len - 32)) return -1;
    if (real_size == 0 || real_size > 64u * 1024u * 1024u) return -1;  /* sanity */

    /* ~20 MB decompressed, well within the DLL's address space. Freed before return. */
    uint8_t *xml = (uint8_t *)malloc(real_size);
    if (!xml) return -2;
    unsigned long out_len = real_size;
    int rc = uncompress(xml, &out_len, b + 32, comp_size);
    if (rc != 0 || out_len != real_size) {
        free(xml);
        return -2;
    }

    for (int i = 0; i < 2; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        for (int k = 0; k < 12; k++) out[i].gear[k].mItemNo = -1;
    }

    /* Anchor on the FIRST <array name="mCmc" type="class" count="3"> — its
     * three children are the only authoritative cSAVE_DATA_CMC blocks for
     * the active session. Anything past it (mCloseFriendPawn, mPlayerData-
     * Auto's mCmc snapshot, mKaiouData, …) carries stale or unrelated
     * state and must NOT seed hired-slot writeback. cmc_cb stops on
     * idx >= 3 so we don't bleed past the count=3 boundary. */
    xspan whole = { (const char *)xml, (const char *)xml + out_len };
    const char *arr_open = xfind(whole,
        "<array name=\"mCmc\" type=\"class\" count=\"3\">");
    if (arr_open) {
        xspan after_arr = { arr_open, whole.end };
        struct cmc_iter_ctx ctx = { .out = out };
        xml_for_each_class(after_arr, "cSAVE_DATA_CMC", cmc_cb, &ctx);
    }

    free(xml);
    return 0;
}
