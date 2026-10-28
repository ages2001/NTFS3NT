/*
 * NTFS3NT engine - run lists: the VCN -> LCN maps of nonresident attributes,
 * their mapping-pairs encoding, and the edits allocation needs (append,
 * punch a hole, cut the tail, replace a range).
 *
 * A run list here is an array of NTFS_RUN in VCN order with no gaps: holes
 * are runs with Sparse set. Adjacent runs that continue each other are
 * merged after every edit.
 */

#include "nteint.h"

/* ------------------------------------------------------------------ */
/* mapping pairs                                                       */
/* ------------------------------------------------------------------ */

static ULONG
SignedBytes(LONGLONG v)
{
    ULONG n = 1;
    while (n < 8) {
        LONGLONG lim = (LONGLONG)1 << (n * 8 - 1);
        if (v >= -lim && v < lim) break;
        n++;
    }
    return n;
}

static ULONG
UnsignedBytes(ULONGLONG v)
{
    ULONG n = 1;
    /* lengths are encoded signed too (a top bit set would read