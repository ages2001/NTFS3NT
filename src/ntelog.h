/*
 * NTFS3NT engine - $LogFile (LFS) on-disk structures and the journal API.
 *
 * The log is a client of the Log File Service (LFS). It has two restart
 * pages at the front, then log record pages. We read the restart area and,
 * if the volume was not shut down cleanly, replay the log the way Windows'
 * NTFS does (analysis -> redo -> undo) so the metadata is consistent before
 * we touch it. We then keep the log ourselves while the volume is mounted
 * for write, writing records Windows can in turn replay after a crash.
 *
 * Layout and field offsets follow Windows NT 4.0 - 11 (LFS 1.1) and, where it
 * differs, Windows 8+ (LFS 2.0). All fields are little-endian.
 */

#ifndef _NTELOG_H_
#define _NTELOG_H_

#pragma pack(push, 1)

/* ------------------------------------------------------------------ */
/* LFS restart page (the first two pages of $LogFile)                  */
/* ------------------------------------------------------------------ */

#define LFS_RSTR_SIGNATURE   0x52545352   /* 'RSTR' */
#define LFS_CHKD_SIGNATURE   0x444B4843   /* 'CHKD' (chkdsk left it) */
#define LFS_RCRD_SIGNATURE   0x44524352   /* 'RCRD' */

/* Multi-sector (update-sequence) header, shared by restart and record pages */
typedef struct _LFS_MST_HEADER {
    ULONG       Signature;               /* 0x00 'RSTR' / 'RCRD'              */
    USHORT      UsaOffset;               /* 0x04                              */
    USHORT      UsaCount;                /* 0x06                              */
    ULONGLONG   Lsn_OrFileOffset;        /* 0x08 restart: chkdsk LSN; record: file offset */
} LFS_MST_HEADER, *PLFS_MST_HEADER;

/* The restart page header (the MST header plus LFS fields) */
typedef struct _LFS_RESTART_PAGE_HEADER {
    ULONG       Signature;               /* 0x00 'RSTR'                       */
    USHORT      UsaOffset;               /* 0x04                              */
    USHORT      UsaCount;                /* 0x06                              */
    ULONGLONG   ChkDskLsn;               /* 0x08                              */
    ULONG       SystemPageSize;          /* 0x10 usually 4096                 */
    ULONG       LogPageSize;             /* 0x14 usually 4096                 */
    USHORT      RestartAreaOffset;       /* 0x18 from page start              */
    SHORT       MinorVersion;            /* 0x1A 1.1 => 1; 2.0 => 0           */
    SHORT       MajorVersion;            /* 0x1C 1.1 => 1; 2.0 => 2           */
    /* USA at UsaOffset */
} LFS_RESTART_PAGE_HEADER, *PLFS_RESTART_PAGE_HEADER;

/* The LFS restart area (at RestartAreaOffset within the restart page) */
typedef struct _LFS_RESTART_AREA {
    ULONGLONG   CurrentLsn;              /* 0x00 highest LSN written          */
    USHORT      LogClients;              /* 0x08 number of client records     */
    USHORT      ClientFreeList;          /* 0x0A head of the free client list */
    USHORT      ClientInUseList;         /* 0x0C head of the in-use list      */
    USHORT      Flags;                   /* 0x0E RESTART_AREA_* (clean bit)   */
    ULONG       SeqNumberBits;           /* 0x10 bits of the LSN sequence num */
    USHORT      RestartAreaLength;       /* 0x14                              */
    USHORT      ClientArrayOffset;       /* 0x16 from restart-area start      */
    ULONGLONG   FileSize;                /* 0x18 usable log size (bytes)      */
    ULONG       LastLsnDataLength;       /* 0x20                              */
    USHORT      LogRecordHeaderLength;   /* 0x24 (0x30 for 1.1)               */
    USHORT      LogPageDataOffset;       /* 0x26 first byte of data in a page  */
    ULONG       RestartLogOpenCount;     /* 0x28 bumped each mount (2.0)       */
    ULONG       Reserved;                /* 0x2C                              */
    /* client array at ClientArrayOffset */
} LFS_RESTART_AREA, *PLFS_RESTART_AREA;

#define LFS_RESTART_VOLUME_IS_CLEAN   0x0002   /* no replay needed */
#define LFS_RESTART_SPACE_FILLER      0x0001

/* An LFS client record (one per client; NTFS uses a single "NTFS" client) */
typedef struct _LFS_CLIENT_RECORD {
    ULONGLONG   OldestLsn;               /* 0x00                              */
    ULONGLONG   ClientRestartLsn;        /* 0x08 the client's checkpoint LSN  */
    USHORT      PrevClient;              /* 0x10                              */
    USHORT      NextClient;              /* 0x12                              */
    USHORT      SeqNumber;               /* 0x14                              */
    UCHAR       Reserved[6];             /* 0x16                              */
    ULONG       ClientNameLength;        /* 0x1C bytes                        */
    WCHAR       ClientName[64];          /* 0x20 "NTFS"                       */
} LFS_CLIENT_RECORD, *PLFS_CLIENT_RECORD;

#define LFS_CLIENT_NONE   0xFFFF

/* A log record page header (RCRD): the MST header plus LFS fields */
typedef struct _LFS_RECORD_PAGE_HEADER {
    ULONG       Signature;               /* 0x00 'RCRD'                       */
    USHORT      UsaOffset;               /* 0x04                              */
    USHORT      UsaCount;                /* 0x06                              */
    ULONGLONG   LastEndLsn;              /* 0x08 (copy of Packed below in 1.1)*/
    ULONGLONG   LastLsn;                 /* 0x10 highest LSN that ends here    */
    ULONG       Flags;                   /* 0x18 LOG_PAGE_LOG_RECORD_END      */
    USHORT      PageCount;               /* 0x1C                              */
    USHORT      PagePosition;            /* 0x1E                              */
    USHORT      NextRecordOffset;        /* 0x20 free byte in the page         */
    USHORT      Reserved1[3];            /* 0x22                              */
    ULONGLONG   LastEndLsn2;             /* 0x28                              */
    /* USA, then data at LogPageDataOffset */
} LFS_RECORD_PAGE_HEADER, *PLFS_RECORD_PAGE_HEADER;

#define LFS_PAGE_LOG_RECORD_END   0x0001

/* The LFS record header that precedes each client log record */
typedef struct _LFS_RECORD_HEADER {
    ULONGLONG   ThisLsn;                 /* 0x00                              */
    ULONGLONG   ClientPreviousLsn;       /* 0x08                              */
    ULONGLONG   ClientUndoNextLsn;       /* 0x10                              */
    ULONG       ClientDataLength;        /* 0x18 bytes of client data         */
    USHORT      SeqNumber;               /* 0x1C client id: seq               */
    USHORT      ClientIndex;             /* 0x1E client id: index             */
    ULONG       RecordType;              /* 0x20 LfsClientRecord / Restart     */
    ULONG       TransactionId;           /* 0x24                              */
    USHORT      Flags;                   /* 0x28 LOG_RECORD_MULTI_PAGE        */
    USHORT      Reserved2[3];            /* 0x2A                              */
    /* client data follows (ClientDataLength bytes), 8-byte aligned */
} LFS_RECORD_HEADER, *PLFS_RECORD_HEADER;

#define LFS_RECORD_MULTI_PAGE   0x0001

#define LFS_CLIENT_RECORD_OP    1    /* RecordType: a normal client record */
#define LFS_CLIENT_RESTART_OP   2    /* RecordType: the client's restart record */

/* ------------------------------------------------------------------ */
/* NTFS client log record (the ClientDataLength bytes)                 */
/* ------------------------------------------------------------------ */

typedef struct _NTFS_LOG_RECORD {
    USHORT      RedoOperation;           /* 0x00 NtfsOp*                      */
    USHORT      UndoOperation;           /* 0x02                              */
    USHORT      RedoOffset;              /* 0x04 from end of this header      */
    USHORT      RedoLength;              /* 0x06                              */
    USHORT      UndoOffset;              /* 0x08                              */
    USHORT      UndoLength;              /* 0x0A                              */
    USHORT      TargetAttribute;         /* 0x0C open-attribute-table offset  */
    USHORT      LcnsToFollow;            /* 0x0E                              */
    USHORT      RecordOffset;            /* 0x10 within the target            */
    USHORT      AttributeOffset;         /* 0x12                              */
    USHORT      ClusterBlockOffset;      /* 0x14 (in 512-byte blocks?)        */
    USHORT      Reserved;                /* 0x16                              */
    ULONGLONG   TargetVcn;               /* 0x18                              */
    ULONGLONG   TargetLcn[1];            /* 0x20 LcnsToFollow entries         */
    /* redo data at RedoOffset, undo data at UndoOffset (from header end) */
} NTFS_LOG_RECORD, *PNTFS_LOG_RECORD;

/* NTFS log operation codes */
enum {
    NtfsOpNoop                          = 0x00,
    NtfsOpCompensationLogRecord         = 0x01,
    NtfsOpInitializeFileRecordSegment   = 0x02,
    NtfsOpDeallocateFileRecordSegment   = 0x03,
    NtfsOpWriteEndOfFileRecordSegment   = 0x04,
    NtfsOpCreateAttribute               = 0x05,
    NtfsOpDeleteAttribute               = 0x06,
    NtfsOpUpdateResidentValue           = 0x07,
    NtfsOpUpdateNonresidentValue        = 0x08,
    NtfsOpUpdateMappingPairs            = 0x09,
    NtfsOpDeleteDirtyClusters           = 0x0A,
    NtfsOpSetNewAttributeSizes          = 0x0B,
    NtfsOpAddIndexEntryRoot             = 0x0C,
    NtfsOpDeleteIndexEntryRoot          = 0x0D,
    NtfsOpAddIndexEntryAllocation       = 0x0E,
    NtfsOpDeleteIndexEntryAllocation    = 0x0F,
    NtfsOpWriteEndOfIndexBuffer         = 0x10,
    NtfsOpSetIndexEntryVcnRoot          = 0x11,
    NtfsOpSetIndexEntryVcnAllocation    = 0x12,
    NtfsOpUpdateFileNameRoot            = 0x13,
    NtfsOpUpdateFileNameAllocation      = 0x14,
    NtfsOpSetBitsInNonresidentBitMap    = 0x15,
    NtfsOpClearBitsInNonresidentBitMap  = 0x16,
    NtfsOpHotFix                        = 0x17,
    NtfsOpEndTopLevelAction             = 0x18,
    NtfsOpPrepareTransaction            = 0x19,
    NtfsOpCommitTransaction             = 0x1A,
    NtfsOpForgetTransaction             = 0x1B,
    NtfsOpOpenNonresidentAttribute      = 0x1C,
    NtfsOpOpenAttributeTableDump        = 0x1D,
    NtfsOpAttributeNamesDump            = 0x1E,
    NtfsOpDirtyPageTableDump            = 0x1F,
    NtfsOpTransactionTableDump          = 0x20,
    NtfsOpUpdateRecordDataRoot          = 0x21,
    NtfsOpUpdateRecordDataAllocation    = 0x22
};

/* The NTFS restart (checkpoint) record - the client data of a Restart op */
typedef struct _NTFS_RESTART {
    ULONG       MajorVersion;            /* 0x00                              */
    ULONG       MinorVersion;            /* 0x04                              */
    ULONGLONG   StartOfCheckpointLsn;    /* 0x08                              */
    ULONGLONG   OpenAttributeTableLsn;   /* 0x10                              */
    ULONGLONG   AttributeNamesLsn;       /* 0x18                              */
    ULONGLONG   DirtyPageTableLsn;       /* 0x20                              */
    ULONGLONG   TransactionTableLsn;     /* 0x28                              */
    ULONG       OpenAttributeTableLength;/* 0x30                              */
    ULONG       AttributeNamesLength;    /* 0x34                              */
    ULONG       DirtyPageTableLength;    /* 0x38                              */
    ULONG       TransactionTableLength;  /* 0x3C                              */
    ULONG       UsnJournalRef;           /* 0x40 low part of $UsnJrnl ref     */
    ULONG       BytesPerCluster;         /* 0x44                              */
} NTFS_RESTART, *PNTFS_RESTART;

/* A restart-table header (open attribute / dirty page / transaction tables) */
typedef struct _NTFS_RESTART_TABLE {
    USHORT      EntrySize;               /* 0x00                              */
    USHORT      NumberEntries;           /* 0x02                              */
    USHORT      NumberAllocated;         /* 0x04                              */
    USHORT      Reserved[3];             /* 0x06                              */
    ULONG       FreeGoal;                /* 0x0C                              */
    ULONG       FirstFree;               /* 0x10                              */
    ULONG       LastFree;                /* 0x14                              */
    /* entries follow at sizeof(this) */
} NTFS_RESTART_TABLE, *PNTFS_RESTART_TABLE;

/* Open-attribute table entry (v0: NT4..XP; v1 adds a name length/offset) */
typedef struct _NTFS_OPEN_ATTR_V0 {
    ULONG       AllocatedOrNextFree;     /* 0x00 (free-list link when free)   */
    ULONG       FileReferenceLow;        /* 0x04                              */
    ULONG       FileReferenceHigh;       /* 0x08 (seq in high 16 of combined) */
    ULONGLONG   LsnOfOpenRecord;         /* 0x0C? */
    UCHAR       Dirty;                   /*     */
    UCHAR       Pad0[3];
    ULONG       AttributeType;
    ULONGLONG   BytesPerIndexBuffer;
} NTFS_OPEN_ATTR_V0, *PNTFS_OPEN_ATTR_V0;

/* Dirty page table entry */
typedef struct _NTFS_DIRTY_PAGE {
    ULONG       AllocatedOrNextFree;     /* 0x00                              */
    ULONG       TargetAttribute;         /* 0x04 open-attr table offset       */
    ULONG       LengthOfTransfer;        /* 0x08                              */
    ULONG       Reserved;                /* 0x0C                              */
    ULONGLONG   Vcn;                     /* 0x10                              */
    ULONGLONG   OldestLsn;               /* 0x18                              */
    /* (lcns to follow in the older 32-bit form; 2.0 is 64-bit as above) */
} NTFS_DIRTY_PAGE, *PNTFS_DIRTY_PAGE;

/* Transaction table entry */
typedef struct _NTFS_TRANSACTION_ENTRY {
    ULONG       AllocatedOrNextFree;     /* 0x00                              */
    UCHAR       TransactionState;        /* 0x04 1 = active                   */
    UCHAR       Pad0[3];
    ULONGLONG   FirstLsn;                /* 0x08                              */
    ULONGLONG   PreviousLsn;             /* 0x10                              */
    ULONGLONG   UndoNextLsn;             /* 0x18                              */
    ULONG       UndoRecords;             /* 0x20                              */
    ULONG       UndoBytes;               /* 0x24                              */
} NTFS_TRANSACTION_ENTRY, *PNTFS_TRANSACTION_ENTRY;

#pragma pack(pop)

#endif /* _NTELOG_H_ */
