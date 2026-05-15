#ifndef PAWNSAVE_H
#define PAWNSAVE_H

#include <stdint.h>
#include <stddef.h>

/* One gear slot in a pawn's mEquipItem array, mirroring the save XML's
 * sItemManager::cITEM_PARAM_DATA record. See gear-persistence.md for how
 * these map to XFS offsets inside a .pawn archive. */
struct sav_gear_record {
    int16_t  mNum;
    int16_t  mItemNo;         /* -1 = empty slot */
    uint32_t mFlag;            /* bit 7 (0x80) is the runtime "equipped" marker */
    uint16_t mChgNum;
    uint16_t mDay1;
    uint16_t mDay2;
    uint16_t mDay3;
    int8_t   mMutationPool;
    int8_t   mOwnerId;         /* 0=Arisen, 1=MainPawn, 2=Hired1, 3=Hired2 */
    uint32_t mKey;
};

/* One hired-pawn snapshot extracted from DDDA.sav: creator name (which the
 * mod injects as "NNN:HEX" when the archive is written), the 12 gear records
 * tagged with this pawn's mOwnerId, the two 322-entry knowledge arrays that
 * the .pawn archive round-trips byte-for-byte from the save XML, and the
 * raw XML bytes of this pawn's full cSAVE_DATA_CMC region (used by the .xml
 * sidecar that restore_pawn consumes). */
struct pawnsave_hired_info {
    int      present;          /* 1 if a matching block was found, 0 otherwise */
    char     creator_name[32]; /* NUL-terminated; up to 25 chars + NUL from mArisenName */
    struct   sav_gear_record gear[12];
    uint32_t study_flag[322];        /* mStudyFlag       — pawn's per-entry knowledge state */
    uint32_t local_study_flag[322];  /* mLocalStudyFlag  — local/instance knowledge state   */

    /* mStudyData.* progression counters: per-enemy-type encounter time
     * (frames @ 30 fps), kill count, and unique-event counters. The game uses
     * these as accumulators that drive bit flips inside mStudyFlag once
     * per-entry thresholds are crossed. Persisting only mStudyFlag (the
     * already-unlocked bits) without these counters discards a hired pawn's
     * partial progress toward not-yet-unlocked knowledge between hires. */
    float    study_encount_frame[72];   /* mStudyData.EncountFrame — f32 × 72 */
    uint32_t study_kill_cnt[72];        /* mStudyData.KillCnt      — u32 × 72 */
    uint8_t  study_unique_cnt[116];     /* mStudyData.UniqueCnt    — u8  × 116 */

    /* Raw bytes of the full <class type="cSAVE_DATA_CMC">…</class> element,
     * including the open and close tags. Heap-allocated; caller must free
     * with `free(region_xml)` (NULL when present=0 or on allocation
     * failure). Used to write the .xml sidecar next to the source archive
     * at writeback time, so restore_pawn can later splice the pawn's full
     * state (vocation, skills, augments, inclinations) — none of which are
     * persisted into the .pawn archive itself — back into a target save's
     * main-pawn slot. */
    uint8_t *region_xml;
    size_t   region_xml_len;
};

/* Parse DDDA.sav once and fill both hired-pawn slots (Hired1 -> out[0],
 * Hired2 -> out[1]). Each out[i].present reflects whether a cSAVE_DATA_CMC
 * block was found whose first non-empty gear record has mOwnerId == 2 + i.
 *
 * save_bytes / save_len describe the raw save as handed to FileWrite: a
 * 32-byte header followed by zlib-compressed XML. The save is decompressed
 * into a heap buffer, scanned, and freed before this returns.
 *
 * Returns 0 on success (out[] populated, possibly with present=0 entries),
 * -1 on bad header, -2 on zlib failure. */
int pawnsave_read_hired(const void *save_bytes, int32_t save_len,
                        struct pawnsave_hired_info out[2]);

/* Extract the live main-pawn cSAVE_DATA_CMC region from an inflated save
 * XML buffer. Returns the byte range [out_off, out_off+out_len) covering
 * `<class type="cSAVE_DATA_CMC">…</class>` for the first slot of the first
 * `<array name="mCmc" type="class" count="3">` (mCmc[0] = main pawn).
 *
 * The xml buffer is already inflated — pass the result of decompressing the
 * save. Both `out_off` and `out_len` are byte counts.
 *
 * Returns 0 on success, -1 if the mCmc array or its first cSAVE_DATA_CMC
 * could not be located. */
int pawnsave_find_main_pawn_region(const uint8_t *xml, size_t xml_len,
                                   size_t *out_off, size_t *out_len);

/* All-in-one for the FileWrite hook: takes a raw DDDA.sav (header + zlib
 * payload), inflates it, locates the main-pawn region, allocates a copy of
 * those bytes, and returns the buffer (caller frees with free()).
 *
 * Returns 0 on success, -1 on bad header, -2 on zlib failure, -3 if the
 * main-pawn region could not be located. */
int pawnsave_extract_main_pawn_xml(const void *save_bytes, int32_t save_len,
                                   uint8_t **out_xml, size_t *out_xml_len);

#endif
