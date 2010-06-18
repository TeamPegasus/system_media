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

/* AudioEncoderCapabilities implementation */

#include "sles_allinclusive.h"

static SLresult IAudioEncoderCapabilities_GetAudioEncoders(SLAudioEncoderCapabilitiesItf self,
    SLuint32 *pNumEncoders, SLuint32 *pEncoderIds)
{
    if (NULL == pNumEncoders)
        return SL_RESULT_PARAMETER_INVALID;
    if (NULL == pEncoderIds) {
        *pNumEncoders = MAX_ENCODERS;
    } else {
        SLuint32 numEncoders = *pNumEncoders;
        if (MAX_ENCODERS <= numEncoders)
            *pNumEncoders = numEncoders = MAX_ENCODERS;
        memcpy(pEncoderIds, Encoder_IDs, numEncoders * sizeof(SLuint32));
    }
    return SL_RESULT_SUCCESS;
}

static SLresult IAudioEncoderCapabilities_GetAudioEncoderCapabilities(
    SLAudioEncoderCapabilitiesItf self, SLuint32 encoderId, SLuint32 *pIndex,
    SLAudioCodecDescriptor *pDescriptor)
{
    return GetCodecCapabilities(encoderId, pIndex, pDescriptor,
        EncoderDescriptors);
}

static const struct SLAudioEncoderCapabilitiesItf_ IAudioEncoderCapabilities_Itf = {
    IAudioEncoderCapabilities_GetAudioEncoders,
    IAudioEncoderCapabilities_GetAudioEncoderCapabilities
};

void IAudioEncoderCapabilities_init(void *self)
{
    IAudioEncoderCapabilities *this = (IAudioEncoderCapabilities *) self;
    this->mItf = &IAudioEncoderCapabilities_Itf;
}