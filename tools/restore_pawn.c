/* restore_pawn — splice an archived pawn into a DDDA.sav as the main pawn.
 *
 * At writeback / archive time the mod stashes each pawn's full
 * <class type="cSAVE_DATA_CMC">…</class> XML region as a sidecar
 * (<HEX>.xml) next to the .pawn archive. The sidecar is well-formed XML —
 * a single root element — so any XML tool can parse it. This tool reads
 * that sidecar and overwrites the target save's own mCmc[0] (main-pawn)
 * region with the same bytes, then re-deflates and repacks. Result:
 * in-game, the pawn that lived inside that archive becomes the player's
 * main pawn — vocation, skills, augments, inclinations, gear, study flags
 * all preserved. (Visible appearance is read by the game from a separate
 * source and is not replaced.)
 *
 * Usage:
 *   restore_pawn <archive-or-xml> <DDDA.sav>
 *
 * <archive-or-xml>:
 *   Path to a .pawn archive (the matching .xml sibling is resolved
 *   automatically) OR directly to an .xml file.
 *
 * <DDDA.sav>:
 *   Target save file. Modified in place.
 *
 * Exit codes:
 *   0  success
 *   1  argument / IO error
 *   2  target save unparseable (bad header / inflate failure)
 *   3  no .xml sidecar found for the given .pawn argument
 *   4  main-pawn region not found in target save (schema mismatch)
 *
 * Build:
 *   i686-w64-mingw32-gcc — see Makefile target `restore_pawn`. The tool is
 *   ANSI C + vendored easyzlib; no Windows-specific APIs beyond stdio. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../pawnsave.h"

/* See pawnlib.c — same convention. The Makefile injects PAWNLIB_VERSION at
 * compile time from the top-level VERSION file. */
#ifndef PAWNLIB_VERSION
#define PAWNLIB_VERSION "dev"
#endif

#include <zlib.h>

/* DDDA PC save constants — see ddsavetool main.cpp / pawnsave.c header
 * comment. The file is always exactly 524288 bytes; the body trails as
 * 0x00 padding past the deflated payload. */
#define SAVE_FIXED_SIZE  524288u
#define SAVE_HEADER_SZ   32u
#define SAVE_VER         21u
#define SAVE_MAGIC1      860693325u
#define SAVE_MAGIC2      860700740u
#define SAVE_MAGIC3      1079398965u

/* CRC32-JAM: standard CRC32 polynomial, init = 0xFFFFFFFF, NO final XOR.
 * Computed over the deflated payload bytes (not the inflated XML). The
 * game appears not to validate this on load, but every sample save we have
 * carries a correct value — keep parity. */
static uint32_t crc32jam(const uint8_t *data, size_t len)
{
    static uint32_t tbl[256];
    static int tbl_init = 0;
    if (!tbl_init) {
        for (uint32_t c = 0; c < 256; c++) {
            uint32_t x = c;
            for (int b = 0; b < 8; b++)
                x = (x & 1u) ? ((x >> 1) ^ 0xEDB88320u) : (x >> 1);
            tbl[c] = x;
        }
        tbl_init = 1;
    }
    uint32_t x = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        x = (x >> 8) ^ tbl[(x ^ data[i]) & 0xFFu];
    return x;
}

static int read_file_all(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0)                     { fclose(f); return -1; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf)                      { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static int write_file_all(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int rc = (w == len) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

/* Case-insensitive suffix match. mingw provides _stricmp; for portability
 * with the cross-compile defaults, just open-code the compare. */
static int has_suffix_ci(const char *s, const char *suf)
{
    size_t L = strlen(s), M = strlen(suf);
    if (L < M) return 0;
    const char *t = s + L - M;
    for (size_t i = 0; i < M; i++) {
        char a = t[i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Resolve <arg> to an .xml sidecar path. If <arg> already ends in .xml,
 * use as-is. If it ends in .pawn, swap the extension to .xml. Otherwise
 * reject. */
static int resolve_sidecar_path(const char *arg, char *out, size_t cap)
{
    size_t L = strlen(arg);
    if (has_suffix_ci(arg, ".xml")) {
        if (L + 1 > cap) return -1;
        memcpy(out, arg, L + 1);
        return 0;
    }
    if (has_suffix_ci(arg, ".pawn")) {
        /* "<stem>.pawn" -> "<stem>.xml": strip ".pawn", append ".xml". */
        size_t stem_len = L - 5;
        if (stem_len + 5 > cap) return -1;  /* ".xml" + NUL = 5 bytes */
        memcpy(out, arg, stem_len);
        memcpy(out + stem_len, ".xml", 5);  /* includes NUL */
        return 0;
    }
    return -1;
}

static void print_usage(FILE *f)
{
    fprintf(f,
        "restore_pawn %s — splice an archived pawn into DDDA.sav as the main pawn\n"
        "\n"
        "Usage:\n"
        "  restore_pawn <archive-or-xml> <DDDA.sav>\n"
        "  restore_pawn --version\n"
        "\n"
        "  <archive-or-xml>      .pawn archive (resolves matching .xml sibling)\n"
        "                        or .xml sidecar file directly.\n"
        "  <DDDA.sav>            Target save. Overwritten in place.\n"
        "\n"
        "  --version             Print version and exit.\n"
        "\n"
        "Note: only the live mCmc array is patched; the checkpoint snapshot\n"
        "is not. Save at an inn after restoring to refresh the checkpoint —\n"
        "otherwise a death-and-restart will revert to the previous main pawn.\n",
        PAWNLIB_VERSION);
}

int main(int argc, char *argv[])
{
    const char *src_arg = NULL;
    const char *sav_arg = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 ||
            strcmp(a, "-h") == 0)                 { print_usage(stdout); return 0; }
        else if (strcmp(a, "--version") == 0)     { printf("restore_pawn %s\n", PAWNLIB_VERSION); return 0; }
        else if (!src_arg)                        src_arg = a;
        else if (!sav_arg)                        sav_arg = a;
        else                                      { print_usage(stderr); return 1; }
    }
    if (!src_arg || !sav_arg) { print_usage(stderr); return 1; }

    /* First line of every run — bug reports paste this without thinking. */
    printf("restore_pawn %s\n", PAWNLIB_VERSION);

    /* 1. Resolve sidecar path. */
    char sidecar_path[1024];
    if (resolve_sidecar_path(src_arg, sidecar_path, sizeof(sidecar_path)) != 0) {
        fprintf(stderr, "restore_pawn: '%s' must end in .pawn or .xml\n", src_arg);
        return 1;
    }

    /* 2. Read sidecar bytes. */
    uint8_t *sidecar = NULL;
    size_t sidecar_len = 0;
    if (read_file_all(sidecar_path, &sidecar, &sidecar_len) != 0) {
        fprintf(stderr, "restore_pawn: cannot read sidecar '%s'\n", sidecar_path);
        if (has_suffix_ci(src_arg, ".pawn")) {
            fprintf(stderr, "  (no .xml sibling — this archive predates the "
                            "sidecar feature; re-summon and rest with the pawn "
                            "to generate one, then retry)\n");
            free(sidecar);
            return 3;
        }
        free(sidecar);
        return 1;
    }
    printf("sidecar:    %s (%lu bytes)\n",
           sidecar_path, (unsigned long)sidecar_len);

    /* Sanity: verify the sidecar starts with <class type="cSAVE_DATA_CMC">
     * and ends with </class>. Anything else is corrupt or wrong file. */
    static const char OPEN_TAG[]  = "<class type=\"cSAVE_DATA_CMC\">";
    static const char CLOSE_TAG[] = "</class>";
    const size_t open_len  = sizeof(OPEN_TAG)  - 1;
    const size_t close_len = sizeof(CLOSE_TAG) - 1;
    if (sidecar_len < open_len + close_len ||
        memcmp(sidecar, OPEN_TAG, open_len) != 0 ||
        memcmp(sidecar + sidecar_len - close_len, CLOSE_TAG, close_len) != 0) {
        fprintf(stderr, "restore_pawn: sidecar '%s' is not a cSAVE_DATA_CMC region\n",
                sidecar_path);
        free(sidecar);
        return 1;
    }

    /* 3. Read target save. */
    uint8_t *save_bytes = NULL;
    size_t save_len = 0;
    if (read_file_all(sav_arg, &save_bytes, &save_len) != 0) {
        fprintf(stderr, "restore_pawn: cannot read save '%s'\n", sav_arg);
        free(sidecar);
        return 1;
    }
    if (save_len < SAVE_HEADER_SZ) {
        fprintf(stderr, "restore_pawn: save '%s' too short (%lu bytes)\n",
                sav_arg, (unsigned long)save_len);
        free(save_bytes); free(sidecar);
        return 2;
    }
    printf("save:       %s (%lu bytes)\n", sav_arg, (unsigned long)save_len);

    uint32_t version, real_size, comp_size, m1, zero, m2, hash, m3;
    memcpy(&version,   save_bytes +  0, 4);
    memcpy(&real_size, save_bytes +  4, 4);
    memcpy(&comp_size, save_bytes +  8, 4);
    memcpy(&m1,        save_bytes + 12, 4);
    memcpy(&zero,      save_bytes + 16, 4);
    memcpy(&m2,        save_bytes + 20, 4);
    memcpy(&hash,      save_bytes + 24, 4);
    memcpy(&m3,        save_bytes + 28, 4);
    if (version != SAVE_VER || m1 != SAVE_MAGIC1 ||
        m2 != SAVE_MAGIC2 || m3 != SAVE_MAGIC3) {
        fprintf(stderr,
                "restore_pawn: '%s' is not a DDDA PC save (magic / version mismatch)\n",
                sav_arg);
        free(save_bytes); free(sidecar);
        return 2;
    }
    if (comp_size == 0 || comp_size > save_len - SAVE_HEADER_SZ) {
        fprintf(stderr,
                "restore_pawn: '%s' header comp_size=%u inconsistent with file size\n",
                sav_arg, (unsigned)comp_size);
        free(save_bytes); free(sidecar);
        return 2;
    }
    (void)zero; (void)hash;  /* zero is informational; hash is recomputed below */

    /* 4. Inflate XML payload. */
    uint8_t *xml = (uint8_t *)malloc(real_size);
    if (!xml) {
        fprintf(stderr, "restore_pawn: out of memory (%u bytes)\n", real_size);
        free(save_bytes); free(sidecar);
        return 1;
    }
    unsigned long inflated_len = real_size;
    int rc = uncompress(xml, &inflated_len,
                        save_bytes + SAVE_HEADER_SZ, comp_size);
    if (rc != 0 || (uint32_t)inflated_len != real_size) {
        fprintf(stderr,
                "restore_pawn: inflate failed (rc=%d, %lu of %u bytes)\n",
                rc, inflated_len, real_size);
        free(xml); free(save_bytes); free(sidecar);
        return 2;
    }
    printf("inflated:   %u bytes\n", real_size);

    /* 5. Locate target's main-pawn region. */
    size_t mp_off = 0, mp_len = 0;
    if (pawnsave_find_main_pawn_region(xml, real_size, &mp_off, &mp_len) != 0) {
        fprintf(stderr,
                "restore_pawn: main-pawn region not found in '%s' "
                "(schema mismatch — game patched?)\n", sav_arg);
        free(xml); free(save_bytes); free(sidecar);
        return 4;
    }
    printf("target mp:  bytes [%lu, %lu)  size=%lu\n",
           (unsigned long)mp_off,
           (unsigned long)(mp_off + mp_len),
           (unsigned long)mp_len);

    /* 6. Splice the new bytes in. New XML = pre + sidecar + post. */
    size_t new_xml_len = real_size - mp_len + sidecar_len;
    uint8_t *new_xml = (uint8_t *)malloc(new_xml_len);
    if (!new_xml) {
        fprintf(stderr, "restore_pawn: out of memory (%lu bytes)\n",
                (unsigned long)new_xml_len);
        free(xml); free(save_bytes); free(sidecar);
        return 1;
    }
    memcpy(new_xml,                          xml,                            mp_off);
    memcpy(new_xml + mp_off,                 sidecar,                        sidecar_len);
    memcpy(new_xml + mp_off + sidecar_len,
           xml + mp_off + mp_len,
           real_size - mp_off - mp_len);
    long delta = (long)new_xml_len - (long)real_size;
    printf("spliced:    %lu bytes  (delta %+ld)\n",
           (unsigned long)new_xml_len, delta);
    free(xml);

    /* 7. Re-deflate. zlib's compressBound(n) returns the worst-case size. */
    unsigned long comp_cap = compressBound((unsigned long)new_xml_len);
    uint8_t *new_comp = (uint8_t *)malloc((size_t)comp_cap);
    if (!new_comp) {
        fprintf(stderr, "restore_pawn: out of memory (%lu bytes)\n", comp_cap);
        free(new_xml); free(save_bytes); free(sidecar);
        return 1;
    }
    unsigned long new_comp_len = comp_cap;
    rc = compress(new_comp, &new_comp_len, new_xml, (unsigned long)new_xml_len);
    if (rc != 0) {
        fprintf(stderr, "restore_pawn: deflate failed (rc=%d)\n", rc);
        free(new_comp); free(new_xml); free(save_bytes); free(sidecar);
        return 1;
    }
    printf("deflated:   %lu bytes\n", new_comp_len);
    free(new_xml);

    if ((uint32_t)new_comp_len > SAVE_FIXED_SIZE - SAVE_HEADER_SZ) {
        fprintf(stderr,
                "restore_pawn: re-deflated payload (%lu) exceeds save body capacity (%u). "
                "Aborting; target save left untouched.\n",
                new_comp_len, SAVE_FIXED_SIZE - SAVE_HEADER_SZ);
        free(new_comp); free(save_bytes); free(sidecar);
        return 1;
    }

    /* 8. Build the new 524288-byte save buffer. Padding bytes after the
     * deflated payload are zero. */
    uint8_t *new_save = (uint8_t *)calloc(1, SAVE_FIXED_SIZE);
    if (!new_save) {
        fprintf(stderr, "restore_pawn: out of memory (%u bytes)\n", SAVE_FIXED_SIZE);
        free(new_comp); free(save_bytes); free(sidecar);
        return 1;
    }
    uint32_t new_real      = (uint32_t)new_xml_len;
    uint32_t new_comp_size = (uint32_t)new_comp_len;
    uint32_t new_zero      = 0;
    uint32_t new_hash      = crc32jam(new_comp, (size_t)new_comp_len);
    uint32_t v_ver = SAVE_VER, v_m1 = SAVE_MAGIC1, v_m2 = SAVE_MAGIC2, v_m3 = SAVE_MAGIC3;
    memcpy(new_save +  0, &v_ver,         4);
    memcpy(new_save +  4, &new_real,      4);
    memcpy(new_save +  8, &new_comp_size, 4);
    memcpy(new_save + 12, &v_m1,          4);
    memcpy(new_save + 16, &new_zero,      4);
    memcpy(new_save + 20, &v_m2,          4);
    memcpy(new_save + 24, &new_hash,      4);
    memcpy(new_save + 28, &v_m3,          4);
    memcpy(new_save + SAVE_HEADER_SZ, new_comp, (size_t)new_comp_len);
    free(new_comp);

    /* 9. Overwrite the target save in place. */
    free(save_bytes);

    if (write_file_all(sav_arg, new_save, SAVE_FIXED_SIZE) != 0) {
        fprintf(stderr, "restore_pawn: cannot write '%s' (target save left in indeterminate state)\n",
                sav_arg);
        free(new_save); free(sidecar);
        return 1;
    }
    printf("wrote:      %s (%u bytes)\n", sav_arg, SAVE_FIXED_SIZE);

    free(new_save);
    free(sidecar);

    printf("\nDone. Load DDDA in-game; the archived pawn is now in the main slot.\n"
           "Tip: save at an inn after loading — that refreshes the checkpoint snapshot\n"
           "so a death/reload won't revert to the previous main pawn.\n");
    return 0;
}
