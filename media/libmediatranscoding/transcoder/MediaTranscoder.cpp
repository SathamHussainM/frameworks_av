/*
 * Copyright (C) 2020 The Android Open Source Project
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

// #define LOG_NDEBUG 0
#define LOG_TAG "MediaTranscoder"

#include <android-base/logging.h>
#include <fcntl.h>
#include <media/MediaSampleReaderNDK.h>
#include <media/MediaSampleWriter.h>
#include <media/MediaTranscoder.h>
#include <media/NdkCommon.h>
#include <media/PassthroughTrackTranscoder.h>
#include <media/VideoTrackTranscoder.h>
#include <unistd.h>

namespace android {

static AMediaFormat* mergeMediaFormats(AMediaFormat* base, AMediaFormat* overlay) {
    if (base == nullptr || overlay == nullptr) {
        LOG(ERROR) << "Cannot merge null formats";
        return nullptr;
    }

    AMediaFormat* format = AMediaFormat_new();
    if (AMediaFormat_copy(format, base) != AMEDIA_OK) {
        AMediaFormat_delete(format);
        return nullptr;
    }

    // Note: AMediaFormat does not expose a function for appending values from another format or for
    // iterating over all values and keys in a format. Instead we define a static list of known keys
    // along with their value types and copy the ones that are present. A better solution would be
    // to either implement required functions in NDK or to parse the overlay format's string
    // representation and copy all existing keys.
    static const AMediaFormatUtils::EntryCopier kSupportedFormatEntries[] = {
            ENTRY_COPIER(AMEDIAFORMAT_KEY_MIME, String),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_DURATION, Int64),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_WIDTH, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_HEIGHT, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_BIT_RATE, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_PROFILE, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_LEVEL, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_COLOR_FORMAT, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_COLOR_RANGE, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_COLOR_STANDARD, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_COLOR_TRANSFER, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_FRAME_RATE, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_PRIORITY, Int32),
            ENTRY_COPIER2(AMEDIAFORMAT_KEY_OPERATING_RATE, Float, Int32),
    };
    const size_t entryCount = sizeof(kSupportedFormatEntries) / sizeof(kSupportedFormatEntries[0]);

    AMediaFormatUtils::CopyFormatEntries(overlay, format, kSupportedFormatEntries, entryCount);
    return format;
}

void MediaTranscoder::sendCallback(media_status_t status) {
    // If the transcoder is already cancelled explicitly, don't send any error callbacks.
    // Tracks and sample writer will report errors for abort. However, currently we can't
    // tell it apart from real errors. Ideally we still want to report real errors back
    // to client, as there is a small chance that explicit abort and the real error come
    // at around the same time, we should report that if abort has a specific error code.
    // On the other hand, if the transcoder actually finished (status is AMEDIA_OK) at around
    // the same time of the abort, we should still report the finish back to the client.
    if (mCancelled && status != AMEDIA_OK) {
        return;
    }

    bool expected = false;
    if (mCallbackSent.compare_exchange_strong(expected, true)) {
        if (status == AMEDIA_OK) {
            mCallbacks->onFinished(this);
        } else {
            mCallbacks->onError(this, status);
        }

        // Transcoding is done and the callback to the client has been sent, so tear down the
        // pipeline but do it asynchronously to avoid deadlocks. If an error occurred, client
        // should clean up the file.
        std::thread asyncCancelThread{[self = shared_from_this()] { self->cancel(); }};
        asyncCancelThread.detach();
    }
}

void MediaTranscoder::onTrackFormatAvailable(const MediaTrackTranscoder* transcoder) {
    LOG(INFO) << "TrackTranscoder " << transcoder << " format available.";

    std::scoped_lock lock{mTracksAddedMutex};

    // Ignore duplicate format change.
    if (mTracksAdded.count(transcoder) > 0) {
        return;
    }

    // Add track to the writer.
    auto consumer = mSampleWriter->addTrack(transcoder->getOutputFormat());
    if (consumer == nullptr) {
        LOG(ERROR) << "Unable to add track to sample writer.";
        sendCallback(AMEDIA_ERROR_UNKNOWN);
        return;
    }

    MediaTrackTranscoder* mutableTranscoder = const_cast<MediaTrackTranscoder*>(transcoder);
    mutableTranscoder->setSampleConsumer(consumer);

    mTracksAdded.insert(transcoder);
    if (mTracksAdded.size() == mTrackTranscoders.size()) {
        // Enable sequential access mode on the sample reader to achieve optimal read performance.
        // This has to wait until all tracks have delivered their output formats and the sample
        // writer is started. Otherwise the tracks will not get their output sample queues drained
        // and the transcoder could hang due to one track running out of buffers and blocking the
        // other tracks from reading source samples before they could output their formats.
        mSampleReader->setEnforceSequentialAccess(true);
        LOG(INFO) << "Starting sample writer.";
        bool started = mSampleWriter->start();
        if (!started) {
            LOG(ERROR) << "Unable to start sample writer.";
            sendCallback(AMEDIA_ERROR_UNKNOWN);
        }
    }
}

void MediaTranscoder::onTrackFinished(const MediaTrackTranscoder* transcoder) {
    LOG(DEBUG) << "TrackTranscoder " << transcoder << " finished";
}

void MediaTranscoder::onTrackError(const MediaTrackTranscoder* transcoder, media_status_t status) {
    LOG(ERROR) << "TrackTranscoder " << transcoder << " returned error " << status;
    sendCallback(status);
}

void MediaTranscoder::onFinished(const MediaSampleWriter* writer __unused, media_status_t status) {
    LOG((status != AMEDIA_OK) ? ERROR : DEBUG) << "Sample writer finished with status " << status;
    sendCallback(status);
}

void MediaTranscoder::onProgressUpdate(const MediaSampleWriter* writer __unused, int32_t progress) {
    // Dispatch progress updated to the client.
    mCallbacks->onProgressUpdate(this, progress);
}

MediaTranscoder::MediaTranscoder(const std::shared_ptr<CallbackInterface>& callbacks, pid_t pid,
                                 uid_t uid)
      : mCallbacks(callbacks), mPid(pid), mUid(uid) {}

std::shared_ptr<MediaTranscoder> MediaTranscoder::create(
        const std::shared_ptr<CallbackInterface>& callbacks, pid_t pid, uid_t uid,
        const std::shared_ptr<ndk::ScopedAParcel>& pausedState) {
    if (pausedState != nullptr) {
        LOG(INFO) << "Initializing from paused state.";
    }
    if (callbacks == nullptr) {
        LOG(ERROR) << "Callbacks cannot be null";
        return nullptr;
    }

    return std::shared_ptr<MediaTranscoder>(new MediaTranscoder(callbacks, pid, uid));
}

media_status_t MediaTranscoder::configureSource(int fd) {
    if (fd < 0) {
        LOG(ERROR) << "Invalid source fd: " << fd;
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const size_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    mSampleReader = MediaSampleReaderNDK::createFromFd(fd, 0 /* offset */, fileSize);

    if (mSampleReader == nullptr) {
        LOG(ERROR) << "Unable to parse source fd: " << fd;
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    const size_t trackCount = mSampleReader->getTrackCount();
    for (size_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        AMediaFormat* trackFormat = mSampleReader->getTrackFormat(static_cast<int>(trackIndex));
        if (trackFormat == nullptr) {
            LOG(ERROR) << "Track #" << trackIndex << " has no format";
            return AMEDIA_ERROR_MALFORMED;
        }

        mSourceTrackFormats.emplace_back(trackFormat, &AMediaFormat_delete);
    }

    return AMEDIA_OK;
}

std::vector<std::shared_ptr<AMediaFormat>> MediaTranscoder::getTrackFormats() const {
    // Return a deep copy of the formats to avoid the caller modifying our internal formats.
    std::vector<std::shared_ptr<AMediaFormat>> trackFormats;
    for (const std::shared_ptr<AMediaFormat>& sourceFormat : mSourceTrackFormats) {
        AMediaFormat* copy = AMediaFormat_new();
        AMediaFormat_copy(copy, sourceFormat.get());
        trackFormats.emplace_back(copy, &AMediaFormat_delete);
    }
    return trackFormats;
}

media_status_t MediaTranscoder::configureTrackFormat(size_t trackIndex, AMediaFormat* trackFormat) {
    if (mSampleReader == nullptr) {
        LOG(ERROR) << "Source must be configured before tracks";
        return AMEDIA_ERROR_INVALID_OPERATION;
    } else if (trackIndex >= mSourceTrackFormats.size()) {
        LOG(ERROR) << "Track index " << trackIndex
                   << " is out of bounds. Track count: " << mSourceTrackFormats.size();
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    media_status_t status = mSampleReader->selectTrack(trackIndex);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to select track " << trackIndex;
        return status;
    }

    std::shared_ptr<MediaTrackTranscoder> transcoder;
    std::shared_ptr<AMediaFormat> format;

    if (trackFormat == nullptr) {
        transcoder = std::make_shared<PassthroughTrackTranscoder>(shared_from_this());
    } else {
        const char* srcMime = nullptr;
        if (!AMediaFormat_getString(mSourceTrackFormats[trackIndex].get(), AMEDIAFORMAT_KEY_MIME,
                                    &srcMime)) {
            LOG(ERROR) << "Source track #" << trackIndex << " has no mime type";
            return AMEDIA_ERROR_MALFORMED;
        }

        if (strncmp(srcMime, "video/", 6) != 0) {
            LOG(ERROR) << "Only video tracks are supported for transcoding. Unable to configure "
                          "track #"
                       << trackIndex << " with mime " << srcMime;
            return AMEDIA_ERROR_UNSUPPORTED;
        }

        const char* dstMime = nullptr;
        if (AMediaFormat_getString(trackFormat, AMEDIAFORMAT_KEY_MIME, &dstMime)) {
            if (strncmp(dstMime, "video/", 6) != 0) {
                LOG(ERROR) << "Unable to convert media types for track #" << trackIndex << ", from "
                           << srcMime << " to " << dstMime;
                return AMEDIA_ERROR_UNSUPPORTED;
            }
        }

        transcoder = VideoTrackTranscoder::create(shared_from_this(), mPid, mUid);

        AMediaFormat* mergedFormat =
                mergeMediaFormats(mSourceTrackFormats[trackIndex].get(), trackFormat);
        if (mergedFormat == nullptr) {
            LOG(ERROR) << "Unable to merge source and destination formats";
            return AMEDIA_ERROR_UNKNOWN;
        }

        format = std::shared_ptr<AMediaFormat>(mergedFormat, &AMediaFormat_delete);
    }

    status = transcoder->configure(mSampleReader, trackIndex, format);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Configure track transcoder for track #" << trackIndex << " returned error "
                   << status;
        return status;
    }

    mTrackTranscoders.emplace_back(std::move(transcoder));
    return AMEDIA_OK;
}

media_status_t MediaTranscoder::configureDestination(int fd) {
    if (fd < 0) {
        LOG(ERROR) << "Invalid destination fd: " << fd;
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    if (mSampleWriter != nullptr) {
        LOG(ERROR) << "Destination is already configured.";
        return AMEDIA_ERROR_INVALID_OPERATION;
    }

    mSampleWriter = MediaSampleWriter::Create();
    const bool initOk = mSampleWriter->init(fd, shared_from_this());

    if (!initOk) {
        LOG(ERROR) << "Unable to initialize sample writer with destination fd: " << fd;
        mSampleWriter.reset();
        return AMEDIA_ERROR_UNKNOWN;
    }

    return AMEDIA_OK;
}

media_status_t MediaTranscoder::start() {
    if (mTrackTranscoders.size() < 1) {
        LOG(ERROR) << "Unable to start, no tracks are configured.";
        return AMEDIA_ERROR_INVALID_OPERATION;
    } else if (mSampleWriter == nullptr) {
        LOG(ERROR) << "Unable to start, destination is not configured";
        return AMEDIA_ERROR_INVALID_OPERATION;
    }

    // Start transcoders
    for (auto& transcoder : mTrackTranscoders) {
        bool started = transcoder->start();
        if (!started) {
            LOG(ERROR) << "Unable to start track transcoder.";
            cancel();
            return AMEDIA_ERROR_UNKNOWN;
        }
    }
    return AMEDIA_OK;
}

media_status_t MediaTranscoder::pause(std::shared_ptr<ndk::ScopedAParcel>* pausedState) {
    // TODO: write internal states to parcel.
    *pausedState = std::shared_ptr<::ndk::ScopedAParcel>(new ::ndk::ScopedAParcel());
    return cancel();
}

media_status_t MediaTranscoder::resume() {
    // TODO: restore internal states from parcel.
    return start();
}

media_status_t MediaTranscoder::cancel() {
    bool expected = false;
    if (!mCancelled.compare_exchange_strong(expected, true)) {
        // Already cancelled.
        return AMEDIA_OK;
    }

    mSampleWriter->stop();
    mSampleReader->setEnforceSequentialAccess(false);
    for (auto& transcoder : mTrackTranscoders) {
        transcoder->stop();
    }

    return AMEDIA_OK;
}

}  // namespace android
