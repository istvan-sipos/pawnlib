#ifndef PAWNXFS_H
#define PAWNXFS_H

#include <stdint.h>
#include <stddef.h>

struct pawnxfs_patch {
    uint32_t      offset;   /* byte offset into the inflated XFS */
    const uint8_t *bytes;
    size_t        len;
};

/* Read <in_path> (a DDDA .pawn blob, 8192 bytes), decrypt-inflate-patch-
 * deflate-SHA1-encrypt, write to <out_path>. Patches are applied in order
 * after inflate, before deflate.
 *
 * In-place is fine: pass the same path for in/out.
 *
 * Returns 0 on success, negative on failure. If err_buf is non-NULL it
 * receives a short human diagnostic on failure. */
int pawnxfs_poke(const char *in_path, const char *out_path,
                 const struct pawnxfs_patch *patches, int count,
                 char *err_buf, size_t err_size);

#endif
