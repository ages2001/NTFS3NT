/*
 * NTFS3NT - prototypes for the shared, kernel-free NTFS logic (ntfssup.c):
 * boot parsing, record fixups, attribute walking, data-run decoding and index
 * iteration. No I/O and no kernel calls, so it builds into the driver, the
 * tools and the Linux test alike.
 */

#ifndef _NTFSSUP_H_
#define _NTFSSUP_H_

#include "ntfsdisk.h"

/* Resolved volume geometry from the boot sector. */
typedef struct _NTFS_GEOMETRY {
    ULONG       BytesPerSector;
    ULONG       SectorsPerCluster;
    ULONG       ClusterSize;             /* bytes                             */
    ULONG       MftRecordSize;           /* bytes (usually 1024)              */
    ULONG       IndexBlockSize;          /* bytes (usually 4096)              */
    ULONGLONG   TotalSectors;
    ULONGLONG   TotalClusters;
    ULONGLONG   MftLcn;
    ULONGLONG   MftMirrLcn;
    ULONGLONG   SerialNumber;
} NTFS_GEOMETRY, *PNTFS_GEOMETRY;

/* One decoded run of a non-resident attribute (VCN range -> LCN range). */
typedef struct _NTFS_RUN {
    ULONGLONG   Vcn;                     /* starting virtual cluster          */
    ULONGLONG   Lcn;                     /* starting logical cluster (if !Sparse)*/
    ULONGLONG   Length;                  /* clusters                          */
    BOOLEAN     Sparse;                  /* TRUE = hole, no LCN               */
} NTFS_RUN, *PNTFS_RUN;

/* Boot parsing. Returns FALSE if the sector is not a usable NTFS BPB. */
BOOLEAN NtfsParseBoot(const void *Sector, ULONG SectorBytes, PNTFS_GEOMETRY Geo);

/*
 * Validate an NTFS record ('FILE'/'INDX'), check its update sequence number and
 * apply the fixups in place. RecordSize is the whole record; BytesPerSector the
 * fixup stride. Returns FALSE on a bad signature or USN mismatch (torn write).
 */
BOOLEAN NtfsApplyFixup(void *Record, ULONG RecordSize, ULONG BytesPerSector);

/*
 * Inverse of NtfsApplyFixup, for writing a record back: bump the update
 * sequence number, stash each sector's last two bytes into the USA and stamp
 * the USN into the sector tails. Call right before writing the record to disk.
 */
void NtfsSetFixup(void *Record, ULONG RecordSize, ULONG BytesPerSector);

/* Attribute walking within a base MFT record (bounds-checked). */
PATTRIBUTE_RECORD NtfsFirstAttribute(PMFT_RECORD Record);
PATTRIBUTE_RECORD NtfsNextAttribute(PMFT_RECORD Record, PATTRIBUTE_RECORD Attr);

/*
 * Find the first attribute of Type (optionally matching a name). Pass Name=NULL
 * to match the unnamed attribute. Returns NULL if not present in this record.
 */
PATTRIBUTE_RECORD NtfsFindAttribute(PMFT_RECORD Record, ULONG Type,
                                    const WCHAR *Name, ULONG NameLength);

/* Resident attribute value access. */
PVOID NtfsResidentValue(PATTRIBUTE_RECORD Attr);
ULONG NtfsResidentValueLength(PATTRIBUTE_RECORD Attr);

/* Pointer to an attribute's name (UTF-16, NameLength chars), or NULL. */
PWCHAR NtfsAttributeName(PATTRIBUTE_RECORD Attr);

/*
 * Decode a non-resident attribute's mapping pairs into Out[0..MaxRuns). Returns
 * FALSE on a malformed run list; *Count gets the number of runs produced.
 */
BOOLEAN NtfsDecodeRuns(PATTRIBUTE_RECORD Attr, PNTFS_RUN Out, ULONG MaxRuns,
                       PULONG Count);

/*
 * Encode runs[0..Count) as an NTFS mapping-pairs stream into Out (OutMax bytes,
 * including the terminating 0). *OutLen gets the bytes written. Returns FALSE if
 * the buffer is too small. Mirrors NtfsDecodeRuns.
 */
BOOLEAN NtfsEncodeRuns(PNTFS_RUN Runs, ULONG Count, PUCHAR Out, ULONG OutMax,
                       PULONG OutLen);

/* Rewrite the unnamed non-resident $DATA run list + sizes in record m, shifting
 * following attributes as needed. FALSE if the new mapping pairs don't fit. */
BOOLEAN NtfsRewriteDataRuns(PMFT_RECORD m, ULONG RecSize, PNTFS_RUN Runs,
                            ULONG RunCount, ULONGLONG AllocSize, ULONGLONG DataSize);
/* Same, for a specific non-resident attribute (unnamed or named $DATA/ADS). */
BOOLEAN NtfsRewriteAttrRuns(PMFT_RECORD m, ULONG RecSize, PATTRIBUTE_RECORD a,
                            PNTFS_RUN Runs, ULONG RunCount, ULONGLONG AllocSize,
                            ULONGLONG DataSize);
/* Update only DataSize/InitializedSize of the unnamed non-resident $DATA. */
BOOLEAN NtfsSetDataSize(PMFT_RECORD m, ULONGLONG DataSize);

/* Build a fresh base FILE record (SI + indexed FILE_NAME + resident $DATA). */
void NtfsRecordToV1(PVOID Buf, ULONG RecSize);
void NtfsBuildFileRecord(PVOID Buf, ULONG RecSize, ULONG SecSize,
                         ULONGLONG RecNo, USHORT Seq, ULONGLONG ParentRef,
                         const WCHAR *Name, ULONG NameLen, ULONGLONG FileTime,
                         const void *Data, ULONG DataLen);

/* Convert the unnamed resident $DATA to a single-run non-resident attribute. */
BOOLEAN NtfsConvertDataToNonResident(PMFT_RECORD m, ULONG RecSize, ULONGLONG Lcn,
                                     ULONGLONG Clusters, ULONGLONG AllocSize,
                                     ULONGLONG DataSize);
BOOLEAN NtfsSetBaseDataRuns(PMFT_RECORD m, ULONG RecSize, PNTFS_RUN Runs, ULONG Rc,
                            ULONGLONG AllocSize, ULONGLONG DataSize, ULONG CompUnit,
                            ULONGLONG CompressedBytes);
/* Insert/remove an index entry in a SMALL (resident) $INDEX_ROOT $I30. */
BOOLEAN NtfsIndexRootInsert(PMFT_RECORD m, ULONG RecSize, const void *Entry, ULONG EntryLen);
BOOLEAN NtfsIndexRootRemove(PMFT_RECORD m, ULONG RecSize, const WCHAR *Name, ULONG NameLen);
/* Resize a resident attribute's value in place (shift following attributes). */
BOOLEAN NtfsResizeResidentValue(PMFT_RECORD m, ULONG RecSize, PATTRIBUTE_RECORD a, ULONG NewLen);
/* Initialise an empty INDX leaf block; split a full leaf and promote the median. */
void NtfsInitIndexLeaf(PUCHAR Buf, ULONG ibs, ULONG SecSize, ULONGLONG Vcn);
BOOLEAN NtfsPromoteSplit(PMFT_RECORD root, ULONG RecSize, PUCHAR OldLeaf, PUCHAR LowBuf,
                         PUCHAR NewBuf, ULONG ibs, ULONG SecSize, ULONGLONG leafVcn,
                         ULONGLONG newVcn, const void *NewEntry, ULONG EntLen);

/*
 * Map a VCN to its LCN using decoded runs. Returns FALSE if the VCN is not
 * mapped (past the end or a sparse hole, with *Sparse set to say which).
 * *RunClustersLeft gets how many contiguous clusters remain from Vcn.
 */
BOOLEAN NtfsMapVcn(PNTFS_RUN Runs, ULONG Count, ULONGLONG Vcn,
                   PULONGLONG Lcn, PULONGLONG RunClustersLeft, PBOOLEAN Sparse);

/*
 * NTFS filename collation: compare two names case-insensitively the way a
 * directory index is ordered. Uses a plain ASCII upcase (A-Z); correct for the
 * ASCII names the tools create. Returns <0, 0 or >0. The kernel driver will use
 * the volume's $UpCase table for full Unicode correctness.
 */
int NtfsCollateName(const WCHAR *A, ULONG ALen, const WCHAR *B, ULONG BLen);

/*
 * Insert a prebuilt index entry (EntryLen bytes) into an index node buffer in
 * collation order. Header is the INDEX_HEADER; Capacity is the bytes available
 * for the node body from Header onward. Returns FALSE if it would not fit (the
 * caller must then grow the node or split). Updates IndexLength.
 */
BOOLEAN NtfsIndexInsert(PINDEX_HEADER Header, ULONG Capacity,
                        const void *Entry, ULONG EntryLen);

/* Directory record / attribute insertion / SMALL->LARGE index promotion. */
#define NTFS_FILE_NAME_INDEX_PRESENT 0x10000000
extern const WCHAR *NtfsUpcaseTable;
void NtfsBuildDirRecord(PVOID Buf, ULONG RecSize, ULONG SecSize, ULONGLONG RecNo,
                        USHORT Seq, ULONGLONG ParentRef, const WCHAR *Name, ULONG NameLen,
                        ULONGLONG FileTime, ULONG IndexBlockSize, ULONG ClusterSize);
BOOLEAN NtfsIsDos83Name(const WCHAR *Name, ULONG Len);
BOOLEAN NtfsInsertAttributeEx(PMFT_RECORD m, ULONG RecSize, const void *Attr, ULONG Len, BOOLEAN First);
BOOLEAN NtfsInsertAttribute(PMFT_RECORD m, ULONG RecSize, const void *Attr, ULONG Len);
void NtfsRemoveAttribute(PMFT_RECORD m, PATTRIBUTE_RECORD a);
PINDEX_ENTRY NtfsFindIndexEntry(PINDEX_HEADER Header, const WCHAR *Name, ULONG NameLen);
BOOLEAN NtfsPromoteRootToLarge(PMFT_RECORD m, ULONG RecSize, PUCHAR Leaf, ULONG ibs,
                               ULONG SecSize, ULONGLONG Lcn, ULONGLONG Clusters,
                               ULONGLONG AllocBytes);


/*
 * Remove the index entry whose FILE_NAME key matches (Name,NameLen) from the
 * node. Returns TRUE if found and removed; updates IndexLength.
 */
BOOLEAN NtfsIndexRemove(PINDEX_HEADER Header, const WCHAR *Name, ULONG NameLen);

/* Index entry walking within an INDEX_HEADER (root node or an INDX block). */
PINDEX_ENTRY NtfsFirstIndexEntry(PINDEX_HEADER Header);
PINDEX_ENTRY NtfsNextIndexEntry(PINDEX_ENTRY Entry);
/* Child-node VCN of a subnode entry (valid only if INDEX_ENTRY_HAS_SUBNODE). */
ULONGLONG NtfsIndexSubnodeVcn(PINDEX_ENTRY Entry);

/* LZNT1 decompression (NTFS transparent compression). In is the mapped
 * (compressed) bytes of one compression unit; Out is filled to OutMax (unit
 * size) and zero-padded. Returns OutMax. */
ULONG NtfsLznt1Unit(const unsigned char *In, ULONG InLen, unsigned char *Out, ULONG OutMax);
ULONG NtfsLznt1CompressUnit(const unsigned char *In, ULONG UnitLen, unsigned char *Out, ULONG OutMax, unsigned char *Body);

#endif /* _NTFSSUP_H_ */
