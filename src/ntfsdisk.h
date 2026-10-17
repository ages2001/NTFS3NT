/*
 * NTFS3NT - on-disk NTFS structures (NTFS 3.1 layout, as written by every
 * Windows from NT 3.51's NTFS 1.2 through modern 3.1; this driver targets the
 * 3.1 layout and tolerates the older ones it still reads).
 *
 * All multi-byte fields are little-endian on disk. Structures are byte-packed.
 * Shared by the kernel driver and the user-mode parser/test harness, so it
 * pulls its integer types from ntifs.h (kernel) or ntfsusr.h (user mode) - see
 * the top of ntfs.h / the tools - and uses no kernel-only types here.
 */

#ifndef _NTFSDISK_H_
#define _NTFSDISK_H_

#pragma pack(push, 1)

/* ------------------------------------------------------------------ */
/* Boot sector / BPB (sector 0 of the volume)                          */
/* ------------------------------------------------------------------ */

typedef struct _NTFS_BOOT_SECTOR {
    UCHAR       Jump[3];                 /* 0x00 EB 52 90                      */
    UCHAR       Oem[8];                  /* 0x03 "NTFS    "                    */
    /* BIOS parameter block */
    USHORT      BytesPerSector;          /* 0x0B                              */
    UCHAR       SectorsPerCluster;       /* 0x0D                              */
    USHORT      ReservedSectors;         /* 0x0E must be 0                    */
    UCHAR       Zero1[3];                /* 0x10 must be 0                    */
    USHORT      Zero2;                   /* 0x13 must be 0                    */
    UCHAR       Media;                   /* 0x15                              */
    USHORT      Zero3;                   /* 0x16 must be 0                    */
    USHORT      SectorsPerTrack;         /* 0x18                              */
    USHORT      Heads;                   /* 0x1A                              */
    ULONG       HiddenSectors;           /* 0x1C                              */
    ULONG       Zero4;                   /* 0x20 must be 0                    */
    ULONG       Signature4;              /* 0x24 usually 0x00800080           */
    ULONGLONG   TotalSectors;            /* 0x28                              */
    ULONGLONG   MftLcn;                  /* 0x30 LCN of $MFT                  */
    ULONGLONG   MftMirrLcn;              /* 0x38 LCN of $MFTMirr              */
    CHAR        ClustersPerMftRecord;    /* 0x40 signed: <0 => 1<<(-v) bytes  */
    UCHAR       Zero5[3];                /* 0x41                              */
    CHAR        ClustersPerIndexBlock;   /* 0x44 signed, same convention      */
    UCHAR       Zero6[3];                /* 0x45                              */
    ULONGLONG   SerialNumber;            /* 0x48                              */
    ULONG       Checksum;                /* 0x50                              */
    UCHAR        Bootstrap[426];         /* 0x54                              */
    USHORT      EndMarker;               /* 0x1FE 0xAA55                      */
} NTFS_BOOT_SECTOR, *PNTFS_BOOT_SECTOR;

/* ------------------------------------------------------------------ */
/* MFT file record ("FILE") header                                     */
/* ------------------------------------------------------------------ */

#define NTFS_FILE_SIGNATURE   0x454C4946   /* 'FILE' */
#define NTFS_INDX_SIGNATURE   0x58444E49   /* 'INDX' */
#define NTFS_BAAD_SIGNATURE   0x44414142   /* 'BAAD' */

/* MFT record flags */
#define MFT_RECORD_IN_USE        0x0001
#define MFT_RECORD_IS_DIRECTORY  0x0002

typedef struct _NTFS_RECORD_HEADER {
    ULONG       Type;                    /* 0x00 'FILE' / 'INDX'              */
    USHORT      UsaOffset;               /* 0x04 update sequence array offset */
    USHORT      UsaCount;                /* 0x06 USN + fixup count            */
    ULONGLONG   Lsn;                     /* 0x08 $LogFile sequence number     */
} NTFS_RECORD_HEADER, *PNTFS_RECORD_HEADER;

typedef struct _MFT_RECORD {
    NTFS_RECORD_HEADER Rhdr;             /* 0x00                              */
    USHORT      SequenceNumber;          /* 0x10                              */
    USHORT      HardLinkCount;           /* 0x12                              */
    USHORT      FirstAttributeOffset;    /* 0x14                              */
    USHORT      Flags;                   /* 0x16 MFT_RECORD_*                 */
    ULONG       BytesInUse;              /* 0x18 used length of this record    */
    ULONG       BytesAllocated;          /* 0x1C allocated length (rec size)   */
    ULONGLONG   BaseFileRecord;          /* 0x20 0 if base record             */
    USHORT      NextAttributeId;         /* 0x28                              */
    USHORT      Align;                   /* 0x2A (NTFS 3.1)                   */
    ULONG       MftRecordNumber;         /* 0x2C (NTFS 3.1)                   */
    /* update sequence array follows at UsaOffset */
} MFT_RECORD, *PMFT_RECORD;

/* ------------------------------------------------------------------ */
/* Attribute record                                                    */
/* ------------------------------------------------------------------ */

/* Attribute type codes */
#define ATTR_STANDARD_INFORMATION   0x10
#define ATTR_ATTRIBUTE_LIST         0x20
#define ATTR_FILE_NAME              0x30
#define ATTR_OBJECT_ID              0x40
#define ATTR_SECURITY_DESCRIPTOR    0x50
#define ATTR_VOLUME_NAME            0x60
#define ATTR_VOLUME_INFORMATION     0x70
#define ATTR_DATA                   0x80
#define ATTR_INDEX_ROOT             0x90
#define ATTR_INDEX_ALLOCATION       0xA0
#define ATTR_BITMAP                 0xB0
#define ATTR_REPARSE_POINT          0xC0
#define ATTR_EA_INFORMATION         0xD0
#define ATTR_EA                     0xE0
#define ATTR_LOGGED_UTILITY_STREAM  0x100
#define ATTR_END                    0xFFFFFFFF

/* Attribute flags */
#define ATTR_FLAG_COMPRESSED        0x0001
#define ATTR_FLAG_ENCRYPTED         0x4000
#define ATTR_FLAG_SPARSE            0x8000

typedef struct _ATTRIBUTE_RECORD {
    ULONG       Type;                    /* 0x00 ATTR_*                       */
    ULONG       Length;                  /* 0x04 total length of this record   */
    UCHAR       NonResident;             /* 0x08 0 = resident, 1 = non-res    */
    UCHAR       NameLength;              /* 0x09 in characters                */
    USHORT      NameOffset;              /* 0x0A from start of this record     */
    USHORT      Flags;                   /* 0x0C ATTR_FLAG_*                  */
    USHORT      AttributeId;             /* 0x0E                              */
    union {
        struct {                         /* resident */
            ULONG   ValueLength;         /* 0x10                              */
            USHORT  ValueOffset;         /* 0x14 from start of this record     */
            UCHAR   IndexedFlag;         /* 0x16                              */
            UCHAR   ResPad;              /* 0x17                              */
        } Resident;
        struct {                         /* non-resident */
            ULONGLONG LowestVcn;         /* 0x10                              */
            ULONGLONG HighestVcn;        /* 0x18                              */
            USHORT  MappingPairsOffset;  /* 0x20 run list offset in record    */
            UCHAR   CompressionUnit;     /* 0x22 0 = none, else 1<<n clusters */
            UCHAR   NrPad[5];            /* 0x23                              */
            ULONGLONG AllocatedSize;     /* 0x28 allocated bytes of the runs  */
            ULONGLONG DataSize;          /* 0x30 real size (EOF)              */
            ULONGLONG InitializedSize;   /* 0x38 valid data length            */
            /* if compressed: ULONGLONG CompressedSize follows */
        } NonResident;
    } Form;
    /* name (UTF-16) at NameOffset; value or mapping pairs follow */
} ATTRIBUTE_RECORD, *PATTRIBUTE_RECORD;

/* ------------------------------------------------------------------ */
/* $STANDARD_INFORMATION (0x10)                                        */
/* ------------------------------------------------------------------ */

typedef struct _STANDARD_INFORMATION {
    ULONGLONG   CreationTime;            /* FILETIME (100ns since 1601)       */
    ULONGLONG   ChangeTime;              /* last data change                  */
    ULONGLONG   MftChangeTime;           /* last MFT record change            */
    ULONGLONG   LastAccessTime;
    ULONG       FileAttributes;          /* FILE_ATTRIBUTE_* (DOS attrs)      */
    ULONG       MaxVersions;
    ULONG       VersionNumber;
    ULONG       ClassId;
    /* NTFS 3.x adds: OwnerId, SecurityId, QuotaCharged, Usn */
    ULONG       OwnerId;
    ULONG       SecurityId;
    ULONGLONG   QuotaCharged;
    ULONGLONG   Usn;
} STANDARD_INFORMATION, *PSTANDARD_INFORMATION;

/* ------------------------------------------------------------------ */
/* $FILE_NAME (0x30) - also the key in directory indexes               */
/* ------------------------------------------------------------------ */

/* File-name namespaces */
#define NTFS_FNAME_POSIX    0
#define NTFS_FNAME_WIN32    1
#define NTFS_FNAME_DOS      2
#define NTFS_FNAME_WIN32DOS 3

typedef struct _FILE_NAME_ATTR {
    ULONGLONG   ParentDirectory;         /* MFT reference (48-bit num + 16 seq)*/
    ULONGLONG   CreationTime;
    ULONGLONG   ChangeTime;
    ULONGLONG   MftChangeTime;
    ULONGLONG   LastAccessTime;
    ULONGLONG   AllocatedSize;
    ULONGLONG   DataSize;
    ULONG       FileAttributes;
    ULONG       EaReparse;
    UCHAR       NameLength;               /* in characters                     */
    UCHAR       NameType;                 /* NTFS_FNAME_*                      */
    /* WCHAR Name[NameLength] follows (UTF-16, not NUL-terminated) */
} FILE_NAME_ATTR, *PFILE_NAME_ATTR;

/* An MFT reference packs record number (low 48 bits) and sequence (high 16). */
#define NTFS_MREF_RECORD(r)   ((ULONGLONG)(r) & 0x0000FFFFFFFFFFFFULL)
#define NTFS_MREF_SEQUENCE(r) ((USHORT)((ULONGLONG)(r) >> 48))

/* Well-known MFT record numbers */
#define MFT_RN_MFT          0
#define MFT_RN_MFTMIRR      1
#define MFT_RN_LOGFILE      2
#define MFT_RN_VOLUME       3
#define MFT_RN_ATTRDEF      4
#define MFT_RN_ROOT         5
#define MFT_RN_BITMAP       6
#define MFT_RN_BOOT         7
#define MFT_RN_BADCLUS      8
#define MFT_RN_SECURE       9
#define MFT_RN_UPCASE       10
#define MFT_RN_EXTEND       11
#define MFT_RN_USER_FIRST   16

/* ------------------------------------------------------------------ */
/* $ATTRIBUTE_LIST (0x20) entry                                        */
/* ------------------------------------------------------------------ */

typedef struct _ATTRIBUTE_LIST_ENTRY {
    ULONG       Type;                    /* 0x00                              */
    USHORT      Length;                  /* 0x04 of this entry                */
    UCHAR       NameLength;              /* 0x06                              */
    UCHAR       NameOffset;             /* 0x07                              */
    ULONGLONG   LowestVcn;               /* 0x08                              */
    ULONGLONG   AttributeReference;      /* 0x10 MFT ref holding the attribute */
    USHORT      AttributeId;             /* 0x18                              */
    /* WCHAR Name[] at NameOffset */
} ATTRIBUTE_LIST_ENTRY, *PATTRIBUTE_LIST_ENTRY;

/* ------------------------------------------------------------------ */
/* Index structures ($INDEX_ROOT 0x90 / $INDEX_ALLOCATION 0xA0)        */
/* ------------------------------------------------------------------ */

/* INDEX_HEADER: the body shared by the root node and each INDX block. */
typedef struct _INDEX_HEADER {
    ULONG       EntriesOffset;           /* 0x00 from start of this header     */
    ULONG       IndexLength;             /* 0x04 used bytes incl. header       */
    ULONG       AllocatedLength;         /* 0x08                              */
    UCHAR       Flags;                   /* 0x0C 1 = has sub-nodes (large)    */
    UCHAR       Pad[3];                  /* 0x0D                              */
} INDEX_HEADER, *PINDEX_HEADER;

typedef struct _INDEX_ROOT {
    ULONG       AttributeType;           /* 0x00 indexed attr type (0x30)     */
    ULONG       CollationRule;           /* 0x04                              */
    ULONG       IndexBlockSize;          /* 0x08 bytes                        */
    UCHAR       ClustersPerIndexBlock;   /* 0x0C                              */
    UCHAR       Pad[3];                  /* 0x0D                              */
    INDEX_HEADER Header;                 /* 0x10                              */
} INDEX_ROOT, *PINDEX_ROOT;

/* An INDX record on disk: NTFS record header + VCN + index header. */
typedef struct _INDEX_BLOCK {
    NTFS_RECORD_HEADER Rhdr;             /* 0x00 'INDX' + USA                 */
    ULONGLONG   IndexBlockVcn;           /* 0x10 VCN of this block in the alloc*/
    INDEX_HEADER Header;                 /* 0x18                              */
} INDEX_BLOCK, *PINDEX_BLOCK;

/* Index entry flags */
#define INDEX_ENTRY_HAS_SUBNODE  0x01    /* entry points to a child INDX block */
#define INDEX_ENTRY_END          0x02    /* last entry (the "end" sentinel)    */

typedef struct _INDEX_ENTRY {
    ULONGLONG   FileReference;           /* 0x00 MFT ref of the indexed file   */
    USHORT      Length;                  /* 0x08 of this entry                 */
    USHORT      KeyLength;               /* 0x0A length of the key (FILE_NAME) */
    USHORT      Flags;                   /* 0x0C INDEX_ENTRY_*                 */
    USHORT      Pad;                     /* 0x0E                              */
    /* key (FILE_NAME_ATTR) follows for non-end entries;                      */
    /* last 8 bytes of the entry hold the child VCN when HAS_SUBNODE          */
} INDEX_ENTRY, *PINDEX_ENTRY;

/* ------------------------------------------------------------------ */
/* $VOLUME_INFORMATION (0x70)                                          */
/* ------------------------------------------------------------------ */

typedef struct _VOLUME_INFORMATION {
    ULONGLONG   Reserved;
    UCHAR       MajorVersion;            /* 3 for NTFS 3.x                    */
    UCHAR       MinorVersion;            /* 1 for 3.1                         */
    USHORT      Flags;                   /* dirty, etc.                       */
} VOLUME_INFORMATION, *PVOLUME_INFORMATION;

#define VOLUME_FLAG_DIRTY   0x0001

#pragma pack(pop)

#endif /* _NTFSDISK_H_ */
