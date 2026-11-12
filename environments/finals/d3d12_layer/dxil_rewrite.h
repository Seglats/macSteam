#ifndef MACFINALS_DXIL_REWRITE_H
#define MACFINALS_DXIL_REWRITE_H

#include <stddef.h>

/* Rewrites a DXBC container holding DXIL. Returns the number of rewrite sites (dynamic
 * reads of a local float array made defined, plus reads of a constant-buffer copy turned
 * into direct buffer reads); 0 means the shader is unchanged and *out is NULL. *cb_reads
 * (may be NULL) gets how many were constant-buffer reads. On success *out is a new
 * container (free() it) with a recomputed checksum. */
/* no_discard (debug): also makes every dx.op.discard in the shader never fire; *discards (may
 * be NULL) gets how many were changed. no_fast (debug): drops every fast-math flag from binary
 * operators and comparisons; *fast_ops (may be NULL) gets how many changed. precise_discard:
 * drops those flags only from what a discard's condition depends on. */
unsigned dxil_rewrite_container(const void *blob, size_t size, void **out, size_t *out_size, unsigned *cb_reads,
                                int no_discard, unsigned *discards, int no_fast, unsigned *fast_ops, int precise_discard);

#endif
