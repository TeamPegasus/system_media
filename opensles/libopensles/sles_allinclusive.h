/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "OpenSLES.h"
#include <stddef.h> // offsetof
#include <stdlib.h> // malloc
#include <string.h> // memcmp
#include <stdio.h>  // debugging
#include <assert.h> // debugging
#include <pthread.h>
#include <unistd.h> // usleep
#include <errno.h>

#include "MPH.h"
#include "MPH_to.h"
#include "devices.h"
#include "ThreadPool.h"

typedef struct CAudioPlayer_struct CAudioPlayer;
typedef struct C3DGroup_struct C3DGroup;

#ifdef USE_SNDFILE
#include <sndfile.h>
#include "SLSndFile.h"
#endif // USE_SNDFILE

#ifdef USE_SDL
#include <SDL/SDL_audio.h>
#endif // USE_SDL

#ifdef USE_ANDROID
#include "media/AudioSystem.h"
#include "media/AudioTrack.h"
#include "media/mediaplayer.h"
#include <utils/String8.h>
#define ANDROID_SL_MILLIBEL_MAX 0
#include <binder/ProcessState.h>
#endif

#ifdef USE_OUTPUTMIXEXT
#include "OutputMixExt.h"
#endif

// Hook functions

typedef void (*VoidHook)(void *self);
typedef SLresult (*StatusHook)(void *self);
typedef SLresult (*AsyncHook)(void *self, SLboolean async);

// Describes how an interface is related to a given class

#define INTERFACE_IMPLICIT           0
#define INTERFACE_EXPLICIT           1
#define INTERFACE_OPTIONAL           2
#define INTERFACE_DYNAMIC            3
#define INTERFACE_UNAVAILABLE        4
#define INTERFACE_DYNAMIC_GAME       INTERFACE_DYNAMIC
#define INTERFACE_DYNAMIC_MUSIC      INTERFACE_DYNAMIC
#define INTERFACE_DYNAMIC_MUSIC_GAME INTERFACE_DYNAMIC
#define INTERFACE_EXPLICIT_GAME      INTERFACE_EXPLICIT
#define INTERFACE_GAME               INTERFACE_OPTIONAL
#define INTERFACE_GAME_MUSIC         INTERFACE_OPTIONAL
#define INTERFACE_MUSIC_GAME         INTERFACE_OPTIONAL
#define INTERFACE_OPTIONAL_DYNAMIC   INTERFACE_DYNAMIC
#define INTERFACE_PHONE_GAME         INTERFACE_OPTIONAL
#define INTERFACE_TBD                INTERFACE_IMPLICIT

// Describes how an interface is related to a given object

#define INTERFACE_UNINITIALIZED 1  // not requested at object creation time
#define INTERFACE_EXPOSED       2  // requested at object creation time
#define INTERFACE_ADDING_1      3  // part 1 of asynchronous AddInterface, pending
#define INTERFACE_ADDING_2      4  // synchronous AddInterface, or part 2 of asynchronous
#define INTERFACE_ADDED         5  // AddInterface has completed
#define INTERFACE_REMOVING      6  // unlocked phase of (synchronous) RemoveInterface
#define INTERFACE_SUSPENDING    7  // suspend in progress
#define INTERFACE_SUSPENDED     8  // suspend has completed
#define INTERFACE_RESUMING_1    9  // part 1 of asynchronous ResumeInterface, pending
#define INTERFACE_RESUMING_2   10  // synchronous ResumeInterface, or part 2 of asynchronous
#define INTERFACE_ADDING_1A    11  // part 1 of asynchronous AddInterface, aborted
#define INTERFACE_RESUMING_1A  12  // part 1 of asynchronous ResumeInterface, aborted

// Maps an interface ID to its offset within the class that exposes it

struct iid_vtable {
    unsigned char mMPH;
    unsigned char mInterface;   // relationship
    /*size_t*/ unsigned short mOffset;
};

// Per-class const data shared by all instances of the same class

typedef struct {
    const struct iid_vtable *mInterfaces;
    SLuint32 mInterfaceCount;  // number of possible interfaces
    const signed char *mMPH_to_index;
    // FIXME not yet used
    const char * const mName;
    size_t mSize;
    SLuint32 mObjectID;
    // hooks
    AsyncHook mRealize;
    AsyncHook mResume;
    VoidHook mDestroy;
} ClassTable;

// BufferHeader describes each element of a BufferQueue, other than the data

typedef struct {
    const void *mBuffer;
    SLuint32 mSize;
} BufferHeader;

#ifdef USE_OUTPUTMIXEXT

// stereo is a frame consisting of a pair of 16-bit PCM samples

typedef struct {
    short left;
    short right;
} stereo;

#endif

#ifdef __cplusplus
#define this this_
#endif

#ifdef USE_SNDFILE

#define SndFile_BUFSIZE 512     // in 16-bit samples
#define SndFile_NUMBUFS 2

struct SndFile {
    // save URI also?
    SLchar *mPathname;
    SNDFILE *mSNDFILE;
    // These are used when Enqueue returns SL_RESULT_BUFFER_INSUFFICIENT
    const void *mRetryBuffer;
    SLuint32 mRetrySize;
    SLuint32 mWhich;    // which buffer to use next
    short mBuffer[SndFile_BUFSIZE * SndFile_NUMBUFS];
};

#endif // USE_SNDFILE

/* Our own merged version of SLDataSource and SLDataSink */

typedef union {
    SLuint32 mLocatorType;
    SLDataLocator_Address mAddress;
    SLDataLocator_BufferQueue mBufferQueue;
    SLDataLocator_IODevice mIODevice;
    SLDataLocator_MIDIBufferQueue mMIDIBufferQueue;
    SLDataLocator_OutputMix mOutputMix;
    SLDataLocator_URI mURI;
} DataLocator;

typedef union {
    SLuint32 mFormatType;
    SLDataFormat_PCM mPCM;
    SLDataFormat_MIME mMIME;
} DataFormat;

typedef struct {
    union {
        SLDataSource mSource;
        SLDataSink mSink;
        struct {
            DataLocator *pLocator;
            DataFormat *pFormat;
        } mNeutral;
    } u;
    DataLocator mLocator;
    DataFormat mFormat;
} DataLocatorFormat;

/* Interface structures */

typedef struct Object_interface {
    const struct SLObjectItf_ *mItf;
    // field mThis would be redundant within an IObject, so we substitute mEngine
    struct Engine_interface *mEngine;
    const ClassTable *mClass;
    SLuint32 mInstanceID; // for debugger and for RPC
    slObjectCallback mCallback;
    void *mContext;
    unsigned mGottenMask;           // interfaces which are exposed or added, and then gotten
    unsigned mLossOfControlMask;    // interfaces with loss of control enabled
    SLint32 mPriority;
    pthread_mutex_t mMutex;
    pthread_cond_t mCond;
    SLuint8 mState;                 // really SLuint32, but SLuint8 to save space
    SLuint8 mPreemptable;           // really SLboolean, but SLuint8 to save space
    // for best alignment, do not add any fields here
#define INTERFACES_Default 2
    SLuint8 mInterfaceStates[INTERFACES_Default];    // state of each of interface
} IObject;

#include "locks.h"

typedef struct {
    const struct SL3DCommitItf_ *mItf;
    IObject *mThis;
    SLboolean mDeferred;
    SLuint32 mGeneration;   // incremented each master clock cycle
} I3DCommit;

enum CartesianSphericalActive {
    CARTESIAN_COMPUTED_SPHERICAL_SET,
    CARTESIAN_REQUESTED_SPHERICAL_SET,
    CARTESIAN_UNKNOWN_SPHERICAL_SET,
    CARTESIAN_SET_SPHERICAL_COMPUTED,   // not in 1.0.1
    CARTESIAN_SET_SPHERICAL_REQUESTED,  // not in 1.0.1
    CARTESIAN_SET_SPHERICAL_UNKNOWN
};

typedef struct {
    const struct SL3DDopplerItf_ *mItf;
    IObject *mThis;
    // The API allows client to specify either Cartesian and spherical velocities.
    // But an implementation will likely prefer one or the other. So for
    // maximum portablity, we maintain both units and an indication of which
    // unit was set most recently. In addition, we keep a flag saying whether
    // the other unit has been derived yet. It can take significant time
    // to compute the other unit, so this may be deferred to another thread.
    // For this reason we also keep an indication of whether the secondary
    // has been computed yet, and its accuracy.
    // Though only one unit is primary at a time, a union is inappropriate:
    // the application might read in both units (not in 1.0.1),
    // and due to multi-threading concerns.
    SLVec3D mVelocityCartesian;
    struct {
        SLmillidegree mAzimuth;
        SLmillidegree mElevation;
        SLmillidegree mSpeed;
    } mVelocitySpherical;
    enum CartesianSphericalActive mVelocityActive;
    SLpermille mDopplerFactor;
} I3DDoppler;

typedef struct {
    const struct SL3DGroupingItf_ *mItf;
    IObject *mThis;
    C3DGroup *mGroup;   // link to associated group or NULL
} I3DGrouping;

enum AnglesVectorsActive {
    ANGLES_COMPUTED_VECTORS_SET,    // not in 1.0.1
    ANGLES_REQUESTED_VECTORS_SET,   // not in 1.0.1
    ANGLES_UNKNOWN_VECTORS_SET,
    ANGLES_SET_VECTORS_COMPUTED,
    ANGLES_SET_VECTORS_REQUESTED,
    ANGLES_SET_VECTORS_UNKNOWN
};

typedef struct {
    const struct SL3DLocationItf_ *mItf;
    IObject *mThis;
    SLVec3D mLocationCartesian;
    struct {
        SLmillidegree mAzimuth;
        SLmillidegree mElevation;
        SLmillimeter mDistance;
    } mLocationSpherical;
    enum CartesianSphericalActive mLocationActive;
    struct {
        SLmillidegree mHeading;
        SLmillidegree mPitch;
        SLmillidegree mRoll;
    } mOrientationAngles;
    struct {
        SLVec3D mFront;
        SLVec3D mAbove;
        SLVec3D mUp;
    } mOrientationVectors;
    enum AnglesVectorsActive mOrientationActive;
    // Rotations can be slow, so are deferred.
    SLmillidegree mTheta;
    SLVec3D mAxis;
    SLboolean mRotatePending;
} I3DLocation;

typedef struct {
    const struct SL3DMacroscopicItf_ *mItf;
    IObject *mThis;
    struct {
        SLmillimeter mWidth;
        SLmillimeter mHeight;
        SLmillimeter mDepth;
    } mSize;
    struct {
        SLmillimeter mHeading;
        SLmillimeter mPitch;
        SLmillimeter mRoll;
    } mOrientationAngles;
    struct {
        SLVec3D mFront;
        SLVec3D mUp;
    } mOrientationVectors;
    enum AnglesVectorsActive mOrientationActive;
    // Rotations can be slow, so are deferred.
    SLmillidegree mTheta;
    SLVec3D mAxis;
    SLboolean mRotatePending;
} I3DMacroscopic;

typedef struct {
    const struct SL3DSourceItf_ *mItf;
    IObject *mThis;
    SLboolean mHeadRelative;
    SLboolean mRolloffMaxDistanceMute;
    SLmillimeter mMaxDistance;
    SLmillimeter mMinDistance;
    SLmillidegree mConeInnerAngle;
    SLmillidegree mConeOuterAngle;
    SLmillibel mConeOuterLevel;
    SLpermille mRolloffFactor;
    SLpermille mRoomRolloffFactor;
    SLuint8 mDistanceModel;
} I3DSource;

typedef struct {
    const struct SLAudioDecoderCapabilitiesItf_ *mItf;
    IObject *mThis;
} IAudioDecoderCapabilities;

typedef struct {
    const struct SLAudioEncoderItf_ *mItf;
    IObject *mThis;
    SLAudioEncoderSettings mSettings;
} IAudioEncoder;

typedef struct {
    const struct SLAudioEncoderCapabilitiesItf_ *mItf;
    IObject *mThis;
} IAudioEncoderCapabilities;

typedef struct {
    const struct SLAudioIODeviceCapabilitiesItf_ *mItf;
    IObject *mThis;
    slAvailableAudioInputsChangedCallback mAvailableAudioInputsChangedCallback;
    void *mAvailableAudioInputsChangedContext;
    slAvailableAudioOutputsChangedCallback mAvailableAudioOutputsChangedCallback;
    void *mAvailableAudioOutputsChangedContext;
    slDefaultDeviceIDMapChangedCallback mDefaultDeviceIDMapChangedCallback;
    void *mDefaultDeviceIDMapChangedContext;
} IAudioIODeviceCapabilities;

typedef struct {
    const struct SLBassBoostItf_ *mItf;
    IObject *mThis;
    SLboolean mEnabled;
    SLpermille mStrength;
} IBassBoost;

typedef struct BufferQueue_interface {
    const struct SLBufferQueueItf_ *mItf;
    IObject *mThis;
    SLBufferQueueState mState;
    slBufferQueueCallback mCallback;
    void *mContext;
    SLuint32 mNumBuffers;
    BufferHeader *mArray;
    BufferHeader *mFront, *mRear;
    SLuint32 mSizeConsumed;
    // saves a malloc in the typical case
#define BUFFER_HEADER_TYPICAL 4
    BufferHeader mTypical[BUFFER_HEADER_TYPICAL+1];
} IBufferQueue;

typedef struct {
    const struct SLDeviceVolumeItf_ *mItf;
    IObject *mThis;
    SLint32 mVolume[2]; // FIXME Hard-coded for default in/out
} IDeviceVolume;

typedef struct {
    const struct SLDynamicInterfaceManagementItf_ *mItf;
    IObject *mThis;
    slDynamicInterfaceManagementCallback mCallback;
    void *mContext;
} IDynamicInterfaceManagement;

typedef struct {
    const struct SLDynamicSourceItf_ *mItf;
    IObject *mThis;
    SLDataSource *mDataSource;
} IDynamicSource;

// private

struct EnableLevel {
    SLboolean mEnable;
    SLmillibel mSendLevel;
};

// indexes into IEffectSend.mEnableLevels

#define AUX_ENVIRONMENTALREVERB 0
#define AUX_PRESETREVERB        1
#define AUX_MAX                 2

typedef struct {
    const struct SLEffectSendItf_ *mItf;
    IObject *mThis;
    struct OutputMix_class *mOutputMix;
    SLmillibel mDirectLevel;
    struct EnableLevel mEnableLevels[AUX_MAX];
} IEffectSend;

// private

typedef struct Engine_interface {
    const struct SLEngineItf_ *mItf;
    IObject *mThis;
    SLboolean mLossOfControlGlobal;
#ifdef USE_SDL
    struct OutputMix_class *mOutputMix; // SDL pulls PCM from an arbitrary OutputMixExt
#endif
    // Each engine is its own universe.
    SLuint32 mInstanceCount;
    unsigned mInstanceMask; // 1 bit per active object
#define MAX_INSTANCE 32     // see mInstanceMask
    IObject *mInstances[MAX_INSTANCE];
    SLboolean mShutdown;
    ThreadPool mThreadPool; // for asynchronous operations
} IEngine;

typedef struct {
    const struct SLEngineCapabilitiesItf_ *mItf;
    IObject *mThis;
    SLboolean mThreadSafe;
} IEngineCapabilities;

typedef struct {
    const struct SLEnvironmentalReverbItf_ *mItf;
    IObject *mThis;
    SLEnvironmentalReverbSettings mProperties;
} IEnvironmentalReverb;

struct EqualizerBand {
    SLmilliHertz mMin;
    SLmilliHertz mCenter;
    SLmilliHertz mMax;
};

#define MAX_EQ_BANDS 4  // compile-time limit, runtime limit may be smaller

typedef struct {
    const struct SLEqualizerItf_ *mItf;
    IObject *mThis;
    SLboolean mEnabled;
    SLuint16 mPreset;
    SLmillibel mLevels[MAX_EQ_BANDS];
    // const to end of struct
    SLuint16 mNumPresets;
    SLuint16 mNumBands;
    const struct EqualizerBand *mBands;
    const struct EqualizerPreset *mPresets;
    SLmillibel mBandLevelRangeMin;
    SLmillibel mBandLevelRangeMax;
} IEqualizer;

#define MAX_LED_COUNT 32

typedef struct {
    const struct SLLEDArrayItf_ *mItf;
    IObject *mThis;
    SLuint32 mLightMask;
    SLHSL mColors[MAX_LED_COUNT];
    // const
    SLuint8 mCount;
} ILEDArray;

typedef struct {
    const struct SLMetadataExtractionItf_ *mItf;
    IObject *mThis;
    SLuint32 mKeySize;
    const void *mKey;
    SLuint32 mKeyEncoding;
    const SLchar *mValueLangCountry;
    SLuint32 mValueEncoding;
    SLuint8 mFilterMask;
    int mKeyFilter;
} IMetadataExtraction;

typedef struct {
    const struct SLMetadataTraversalItf_ *mItf;
    IObject *mThis;
    SLuint32 mIndex;
    SLuint32 mMode;
    SLuint32 mCount;
    SLuint32 mSize;
} IMetadataTraversal;

typedef struct {
    const struct SLMIDIMessageItf_ *mItf;
    IObject *mThis;
    slMetaEventCallback mMetaEventCallback;
    void *mMetaEventContext;
    slMIDIMessageCallback mMessageCallback;
    void *mMessageContext;
    SLuint8 mMessageTypes;
} IMIDIMessage;

typedef struct {
    const struct SLMIDIMuteSoloItf_ *mItf;
    IObject *mThis;
    SLuint16 mChannelMuteMask;
    SLuint16 mChannelSoloMask;
    SLuint32 mTrackMuteMask;
    SLuint32 mTrackSoloMask;
    // const
    SLuint16 mTrackCount;
} IMIDIMuteSolo;

typedef struct {
    const struct SLMIDITempoItf_ *mItf;
    IObject *mThis;
    SLuint32 mTicksPerQuarterNote;
    SLuint32 mMicrosecondsPerQuarterNote;
} IMIDITempo;

typedef struct {
    const struct SLMIDITimeItf_ *mItf;
    IObject *mThis;
    SLuint32 mDuration;
    SLuint32 mPosition;
    SLuint32 mStartTick;
    SLuint32 mNumTicks;
} IMIDITime;

typedef struct {
    const struct SLMuteSoloItf_ *mItf;
    IObject *mThis;
    SLuint32 mMuteMask;
    SLuint32 mSoloMask;
    // const
    SLuint8 mNumChannels;
} IMuteSolo;

#define MAX_TRACK 32        // see mActiveMask

typedef struct {
    const struct SLOutputMixItf_ *mItf;
    IObject *mThis;
    slMixDeviceChangeCallback mCallback;
    void *mContext;
#ifdef USE_OUTPUTMIXEXT
    unsigned mActiveMask;   // 1 bit per active track
    struct Track mTracks[MAX_TRACK];
#endif
} IOutputMix;

#ifdef USE_OUTPUTMIXEXT
typedef struct {
    const struct SLOutputMixExtItf_ *mItf;
    IObject *mThis;
} IOutputMixExt;
#endif

typedef struct {
    const struct SLPitchItf_ *mItf;
    IObject *mThis;
    SLpermille mPitch;
    // const
    SLpermille mMinPitch;
    SLpermille mMaxPitch;
} IPitch;

typedef struct Play_interface {
    const struct SLPlayItf_ *mItf;
    IObject *mThis;
    SLuint32 mState;
    SLmillisecond mDuration;
    SLmillisecond mPosition;
    // unsigned mPositionSamples;  // position in sample units
    slPlayCallback mCallback;
    void *mContext;
    SLuint32 mEventFlags;
    SLmillisecond mMarkerPosition;
    SLmillisecond mPositionUpdatePeriod;
} IPlay;

typedef struct {
    const struct SLPlaybackRateItf_ *mItf;
    IObject *mThis;
    SLpermille mRate;
    SLuint32 mProperties;
    // const
    SLpermille mMinRate;
    SLpermille mMaxRate;
    SLpermille mStepSize;
    SLuint32 mCapabilities;
} IPlaybackRate;

typedef struct {
    const struct SLPrefetchStatusItf_ *mItf;
    IObject *mThis;
    SLuint32 mStatus;
    SLpermille mLevel;
    slPrefetchCallback mCallback;
    void *mContext;
    SLuint32 mCallbackEventsMask;
    SLpermille mFillUpdatePeriod;
} IPrefetchStatus;

typedef struct {
    const struct SLPresetReverbItf_ *mItf;
    IObject *mThis;
    SLuint16 mPreset;
} IPresetReverb;

typedef struct {
    const struct SLRatePitchItf_ *mItf;
    IObject *mThis;
    SLpermille mRate;
    // const
    SLpermille mMinRate;
    SLpermille mMaxRate;
} IRatePitch;

typedef struct {
    const struct SLRecordItf_ *mItf;
    IObject *mThis;
    SLuint32 mState;
    SLmillisecond mDurationLimit;
    SLmillisecond mPosition;
    slRecordCallback mCallback;
    void *mContext;
    SLuint32 mCallbackEventsMask;
    SLmillisecond mMarkerPosition;
    SLmillisecond mPositionUpdatePeriod;
} IRecord;

typedef struct {
    const struct SLSeekItf_ *mItf;
    IObject *mThis;
    SLmillisecond mPos;
    SLboolean mLoopEnabled;
    SLmillisecond mStartPos;
    SLmillisecond mEndPos;
} ISeek;

typedef struct {
    const struct SLThreadSyncItf_ *mItf;
    IObject *mThis;
    SLboolean mInCriticalSection;
    SLboolean mWaiting;
    pthread_t mOwner;
} IThreadSync;

typedef struct {
    const struct SLVibraItf_ *mItf;
    IObject *mThis;
    SLboolean mVibrate;
    SLmilliHertz mFrequency;
    SLpermille mIntensity;
} IVibra;

typedef struct {
    const struct SLVirtualizerItf_ *mItf;
    IObject *mThis;
    SLboolean mEnabled;
    SLpermille mStrength;
} IVirtualizer;

typedef struct {
    const struct SLVisualizationItf_ *mItf;
    IObject *mThis;
    slVisualizationCallback mCallback;
    void *mContext;
    SLmilliHertz mRate;
} IVisualization;

typedef struct {
    const struct SLVolumeItf_ *mItf;
    IObject *mThis;
    SLmillibel mLevel;
    SLboolean mMute;
    SLboolean mEnableStereoPosition;
    SLpermille mStereoPosition;
#ifdef USE_ANDROID
    /**
     * Amplification (can be attenuation) factor derived for the VolumeLevel
     */
    float mAmplFromVolLevel;
    /**
     * Left/right amplification (can be attenuations) factors derived for the StereoPosition
     */
    float mAmplFromStereoPos[2];
    /**
     * Channel mask for which channels are muted
     */
    int mChannelMutes;
    /**
     * Channel mask for which channels are solo'ed
     */
    int mChannelSolos;
#endif
} IVolume;

/* Class structures */

/*typedef*/ struct C3DGroup_struct {
    IObject mObject;
#define INTERFACES_3DGroup 6 // see MPH_to_3DGroup in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_3DGroup - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    I3DLocation m3DLocation;
    I3DDoppler m3DDoppler;
    I3DSource m3DSource;
    I3DMacroscopic m3DMacroscopic;
    unsigned mMemberMask;   // set of member objects
} /*C3DGroup*/;

#ifdef ANDROID
/*
 * Used to define the mapping from an OpenSL ES audio player to an Android
 * media framework object
 */
enum AndroidObject_type {
    INVALID_TYPE     =-1,
    MEDIAPLAYER      = 0,
    AUDIOTRACK_PUSH  = 1,
    AUDIOTRACK_PULL  = 2,
    NUM_AUDIOPLAYER_MAP_TYPES
};

enum AndroidObject_state {
    ANDROID_UNINITIALIZED = -1,
    ANDROID_PREPARING,
    ANDROID_PREPARED,
    ANDROID_PREFETCHING,
    ANDROID_READY,
    NUM_ANDROID_STATES
};

typedef struct {
    android::AudioTrack *mAudioTrack;
} AudioTrackData;

typedef struct {
    android::MediaPlayer *mMediaPlayer;
} MediaPlayerData;

#endif

/*typedef*/ struct CAudioPlayer_struct {
    IObject mObject;
#define INTERFACES_AudioPlayer 26 // see MPH_to_AudioPlayer in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_AudioPlayer - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IPlay mPlay;
    I3DDoppler m3DDoppler;
    I3DGrouping m3DGrouping;
    I3DLocation m3DLocation;
    I3DSource m3DSource;
    IBufferQueue mBufferQueue;
    IEffectSend mEffectSend;
    IMuteSolo mMuteSolo;
    IMetadataExtraction mMetadataExtraction;
    IMetadataTraversal mMetadataTraversal;
    IPrefetchStatus mPrefetchStatus;
    IRatePitch mRatePitch;
    ISeek mSeek;
    IVolume mVolume;
    // optional interfaces
    I3DMacroscopic m3DMacroscopic;
    IBassBoost mBassBoost;
    IDynamicSource mDynamicSource;
    IEnvironmentalReverb mEnvironmentalReverb;
    IEqualizer mEqualizer;
    IPitch mPitch;
    IPresetReverb mPresetReverb;
    IPlaybackRate mPlaybackRate;
    IVirtualizer mVirtualizer;
    IVisualization mVisualization;
    // rest of fields are not related to the interfaces
    DataLocatorFormat mDataSource;
    DataLocatorFormat mDataSink;
#ifdef USE_SNDFILE
    struct SndFile mSndFile;
#endif // USE_SNDFILE
#ifdef ANDROID
    android::Mutex          *mpLock;
    enum AndroidObject_type mAndroidObjType;
    enum AndroidObject_state mAndroidObjState;
    union {
        AudioTrackData      mAudioTrackData;
        MediaPlayerData     mMediaPlayerData;
    };
#endif
} /*CAudioPlayer*/;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_AudioRecorder 9 // see MPH_to_AudioRecorder in MPH_to.c for list of interfaces
    SLuint8 mInterfaceContinued[INTERFACES_AudioRecorder - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IRecord mRecord;
    IAudioEncoder mAudioEncoder;
    // optional interfaces
    IBassBoost mBassBoost;
    IDynamicSource mDynamicSource;
    IEqualizer mEqualizer;
    IVisualization mVisualization;
    IVolume mVolume;
    // rest of fields are not related to the interfaces
    DataLocatorFormat mDataSource;
    DataLocatorFormat mDataSink;
} CAudioRecorder;

typedef struct {
    // mandated implicit interfaces
    IObject mObject;
#define INTERFACES_Engine 10 // see MPH_to_Engine in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_Engine - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IEngine mEngine;
    IEngineCapabilities mEngineCapabilities;
    IThreadSync mThreadSync;
    // mandated explicit interfaces
    IAudioIODeviceCapabilities mAudioIODeviceCapabilities;
    IAudioDecoderCapabilities mAudioDecoderCapabilities;
    IAudioEncoderCapabilities mAudioEncoderCapabilities;
    I3DCommit m3DCommit;
    // optional interfaces
    IDeviceVolume mDeviceVolume;
    pthread_t mSyncThread;
} CEngine;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_LEDDevice 3 // see MPH_to_LEDDevice in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_LEDDevice - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    ILEDArray mLEDArray;
    SLuint32 mDeviceID;
} CLEDDevice;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_Listener 4 // see MPH_to_Listener in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_Listener - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    I3DDoppler m3DDoppler;
    I3DLocation m3DLocation;
} CListener;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_MetadataExtractor 5 // see MPH_to_MetadataExtractor in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_MetadataExtractor - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IDynamicSource mDynamicSource;
    IMetadataExtraction mMetadataExtraction;
    IMetadataTraversal mMetadataTraversal;
} CMetadataExtractor;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_MidiPlayer 29 // see MPH_to_MidiPlayer in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_MidiPlayer - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IPlay mPlay;
    I3DDoppler m3DDoppler;
    I3DGrouping m3DGrouping;
    I3DLocation m3DLocation;
    I3DSource m3DSource;
    IBufferQueue mBufferQueue;
    IEffectSend mEffectSend;
    IMuteSolo mMuteSolo;
    IMetadataExtraction mMetadataExtraction;
    IMetadataTraversal mMetadataTraversal;
    IMIDIMessage mMIDIMessage;
    IMIDITime mMIDITime;
    IMIDITempo mMIDITempo;
    IMIDIMuteSolo mMIDIMuteSolo;
    IPrefetchStatus mPrefetchStatus;
    ISeek mSeek;
    IVolume mVolume;
    // optional interfaces
    I3DMacroscopic m3DMacroscopic;
    IBassBoost mBassBoost;
    IDynamicSource mDynamicSource;
    IEnvironmentalReverb mEnvironmentalReverb;
    IEqualizer mEqualizer;
    IPitch mPitch;
    IPresetReverb mPresetReverb;
    IPlaybackRate mPlaybackRate;
    IVirtualizer mVirtualizer;
    IVisualization mVisualization;
} CMidiPlayer;

typedef struct OutputMix_class {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_OutputMix 11 // see MPH_to_OutputMix in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_OutputMix - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IOutputMix mOutputMix;
#ifdef USE_OUTPUTMIXEXT
    IOutputMixExt mOutputMixExt;
#endif
    IEnvironmentalReverb mEnvironmentalReverb;
    IEqualizer mEqualizer;
    IPresetReverb mPresetReverb;
    IVirtualizer mVirtualizer;
    IVolume mVolume;
    // optional interfaces
    IBassBoost mBassBoost;
    IVisualization mVisualization;
} COutputMix;

typedef struct {
    // mandated interfaces
    IObject mObject;
#define INTERFACES_VibraDevice 3 // see MPH_to_VibraDevice in MPH_to.c for list of interfaces
    SLuint8 mInterfaceStates2[INTERFACES_VibraDevice - INTERFACES_Default];
    IDynamicInterfaceManagement mDynamicInterfaceManagement;
    IVibra mVibra;
    //
    SLuint32 mDeviceID;
} CVibraDevice;

struct MPH_init {
    // unsigned char mMPH;
    VoidHook mInit;
    VoidHook mResume;
    VoidHook mDeinit;
};

extern /*static*/ int IID_to_MPH(const SLInterfaceID iid);
extern /*static*/ const struct MPH_init MPH_init_table[MPH_MAX];
extern SLresult checkInterfaces(const ClassTable *class__,
    SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
    const SLboolean *pInterfaceRequired, unsigned *pExposedMask);
extern IObject *construct(const ClassTable *class__,
    unsigned exposedMask, SLEngineItf engine);
extern const ClassTable *objectIDtoClass(SLuint32 objectID);
extern const struct SLInterfaceID_ SL_IID_array[MPH_MAX];
extern SLuint32 IObjectToObjectID(IObject *object);

// Map an interface to it's "object ID" (which is really a class ID).
// Note: this operation is undefined on IObject, as it lacks an mThis.
// If you have an IObject, then use IObjectToObjectID directly.

#define InterfaceToObjectID(this) IObjectToObjectID((this)->mThis)

// Map an interface to it's corresponding IObject.
// Note: this operation is undefined on IObject, as it lacks an mThis.
// If you have an IObject, then you're done -- you already have what you need.

#define InterfaceToIObject(this) ((this)->mThis)

#ifdef USE_ANDROID
#include "sles_to_android.h"
#endif

extern SLresult checkDataSource(const SLDataSource *pDataSrc, DataLocatorFormat *myDataSourceLocator);
extern SLresult checkDataSink(const SLDataSink *pDataSink, DataLocatorFormat *myDataSinkLocator);
extern void freeDataLocatorFormat(DataLocatorFormat *dlf);
extern SLresult CAudioPlayer_Realize(void *self, SLboolean async);
extern void CAudioPlayer_Destroy(void *self);
extern SLresult CEngine_Realize(void *self, SLboolean async);
extern void CEngine_Destroy(void *self);
#ifdef USE_SDL
extern void SDL_start(IEngine *thisEngine);
#endif
#define SL_OBJECT_STATE_REALIZING_1  ((SLuint32) 0x4) // async realize on work queue
#define SL_OBJECT_STATE_REALIZING_2  ((SLuint32) 0x5) // sync realize, or async realize hook
#define SL_OBJECT_STATE_RESUMING_1   ((SLuint32) 0x6) // async resume on work queue
#define SL_OBJECT_STATE_RESUMING_2   ((SLuint32) 0x7) // sync resume, or async resume hook
#define SL_OBJECT_STATE_SUSPENDING   ((SLuint32) 0x8) // suspend in progress
#define SL_OBJECT_STATE_REALIZING_1A ((SLuint32) 0x9) // abort while async realize on work queue
#define SL_OBJECT_STATE_RESUMING_1A  ((SLuint32) 0xA) // abort while async resume on work queue
extern void *sync_start(void *arg);
extern SLresult err_to_result(int err);
#define ctz __builtin_ctz