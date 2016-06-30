#ifndef CAFDEFINES_H
#define CAFDEFINES_H

/* dense pack all structs */
/* in a CAF file there are no pad fields to ensure correct byte alignment */
#pragma pack(push, 1)

/* ------------------------------------------------ */
/* MARKER CHUNK */

/* this is for the mSMPTETime of CAFMarker */
typedef struct {
    unsigned char   mHours;
    unsigned char   mMinutes;
    unsigned char   mSeconds;
    unsigned char   mFrames;
    uint32_t        mSubFrameSampleOffset;
} CAF_SMPTE_Time;

/* this is for the mSMPTE_TimeType of CAFMarkerChunk */
typedef enum {
    kCAF_SMPTE_TimeTypeNone     = 0,
    kCAF_SMPTE_TimeType24       = 1,
    kCAF_SMPTE_TimeType25       = 2,
    kCAF_SMPTE_TimeType30Drop   = 3,
    kCAF_SMPTE_TimeType30       = 4,
    kCAF_SMPTE_TimeType2997     = 5,
    kCAF_SMPTE_TimeType2997Drop = 6,
    kCAF_SMPTE_TimeType60       = 7,
    kCAF_SMPTE_TimeType5994     = 8
} kCAF_SMPTE_TimeType; // uint32_t

/* this is for the mType field of CAFMarker */
typedef enum {
    kCAFMarkerType_Generic              = 0,
    kCAFMarkerType_ProgramStart         = 'pbeg',
    kCAFMarkerType_ProgramEnd           = 'pend',
    kCAFMarkerType_TrackStart           = 'tbeg',
    kCAFMarkerType_TrackEnd             = 'tend',
    kCAFMarkerType_Index                = 'indx',
    kCAFMarkerType_RegionStart          = 'rbeg',
    kCAFMarkerType_RegionEnd            = 'rend',
    kCAFMarkerType_RegionSyncPoint      = 'rsyc',
    kCAFMarkerType_SelectionStart       = 'sbeg',
    kCAFMarkerType_SelectionEnd         = 'send',
    kCAFMarkerType_EditSourceBegin      = 'cbeg',
    kCAFMarkerType_EditSourceEnd        = 'cend',
    kCAFMarkerType_EditDestinationBegin = 'dbeg',
    kCAFMarkerType_EditDestinationEnd   = 'dend',
    kCAFMarkerType_SustainLoopStart     = 'slbg',
    kCAFMarkerType_SustainLoopEnd       = 'slen',
    kCAFMarkerType_ReleaseLoopStart     = 'rlbg',
    kCAFMarkerType_ReleaseLoopEnd       = 'rlen'
} kCAFMarkerType; // uint32_t

/* individual marker struct */
typedef struct {
    uint32_t        mType;
    float64_t       mFramePosition;
    uint32_t        mMarkerID;        // reference to a mStringsIDs for naming 
    CAF_SMPTE_Time  mSMPTETime;
    uint32_t        mChannel;
} CAFMarker;


/* chunk holding all markers */
typedef struct {
    uint32_t     mSMPTE_TimeType;
    uint32_t     mNumberMarkers;
    CAFMarker*  mMarkers;
} CAFMarkerChunk;

/* ------------------------------------------------ */
/* REGIONS CHUNK */

/* used for mFlags in CAFRegion */
typedef enum {
    kCAFRegionFlag_LoopEnable    = 1,
    kCAFRegionFlag_PlayForward   = 2,
    kCAFRegionFlag_PlayBackward  = 4
} kCAFRegionFlag; // uint32_t

typedef struct {
    uint32_t    mRegionID;
    uint32_t    mFlags;
    uint32_t    mNumberMarkers;
    CAFMarker* mMarkers;
} CAFRegion;

#define SIZEOF_CAFRegion 12

typedef struct {
    uint32_t     mSMPTE_TimeType;
    uint32_t     mNumberRegions;
    CAFRegion*   mRegions;
} CAFRegionChunk;

#define SIZEOF_CAFRegionChunk 8

/* ------------------------------------------------ */
/* STRINGS CHUNK - used as labels for Markers and Regions*/

typedef struct {
    uint32_t  mStringID;
    int64_t   mStringStartByteOffset;
} CAFStringID;

typedef struct {
    uint32_t       mNumEntries; // The number of strings in the mStrings field.
    CAFStringID*   mStringsIDs; // the marker refers to this id with mMarkerID
    unsigned char* mStrings;    // An array of null-terminated UTF8-encoded text strings.
} CAFStrings;
#define SIZEOF_CAFStrings 4

#pragma pack(pop)

#endif // CAFDEFINES_H