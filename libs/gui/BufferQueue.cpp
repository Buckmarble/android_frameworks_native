/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "BufferQueue"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <gui/BufferQueue.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>

namespace android {

// Get an ID that's unique within this process.
static int32_t createProcessUniqueId() {
    static volatile int32_t globalCounter = 0;
    return android_atomic_inc(&globalCounter);
}

static const char* scalingModeName(int scalingMode) {
    switch (scalingMode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE: return "FREEZE";
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW: return "SCALE_TO_WINDOW";
        case NATIVE_WINDOW_SCALING_MODE_SCALE_CROP: return "SCALE_CROP";
        default: return "Unknown";
    }
}

BufferQueue::BufferQueue(const sp<IGraphicBufferAlloc>& allocator) :
    mDefaultWidth(1),
    mDefaultHeight(1),
    mMaxAcquiredBufferCount(1),
    mDefaultMaxBufferCount(2),
    mOverrideMaxBufferCount(0),
    mConsumerControlledByApp(false),
    mDequeueBufferCannotBlock(false),
    mUseAsyncBuffer(true),
    mConnectedApi(NO_CONNECTED_API),
    mAbandoned(false),
    mFrameCounter(0),
    mBufferHasBeenQueued(false),
    mDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888),
    mConsumerUsageBits(0),
    mTransformHint(0)
{
    // Choose a name using the PID and a process-unique ID.
    mConsumerName = String8::format("unnamed-%d-%d", getpid(), createProcessUniqueId());

    ST_LOGV("BufferQueue");
    if (allocator == NULL) {
        sp<ISurfaceComposer> composer(ComposerService::getComposerService());
        mGraphicBufferAlloc = composer->createGraphicBufferAlloc();
        if (mGraphicBufferAlloc == 0) {
            ST_LOGE("createGraphicBufferAlloc() failed in BufferQueue()");
        }
    } else {
        mGraphicBufferAlloc = allocator;
    }
    Rect x(0,0,0,0);
    mCurrentDirtyRegion.set(x);

    for(size_t cur=0; cur<NUM_BUFFER_SLOTS; cur++)
        mDirtyRegion[cur].set(x);

}

BufferQueue::~BufferQueue() {
    ST_LOGV("~BufferQueue");
}

status_t BufferQueue::setDefaultMaxBufferCountLocked(int count) {
    const int minBufferCount = mUseAsyncBuffer ? 2 : 1;
    if (count < minBufferCount || count > NUM_BUFFER_SLOTS)
        return BAD_VALUE;

    mDefaultMaxBufferCount = count;
    mDequeueCondition.broadcast();

    return NO_ERROR;
}

void BufferQueue::setConsumerName(const String8& name) {
    Mutex::Autolock lock(mMutex);
    mConsumerName = name;
}

status_t BufferQueue::setDefaultBufferFormat(uint32_t defaultFormat) {
    Mutex::Autolock lock(mMutex);
    mDefaultBufferFormat = defaultFormat;
    return NO_ERROR;
}

status_t BufferQueue::setConsumerUsageBits(uint32_t usage) {
    Mutex::Autolock lock(mMutex);
    mConsumerUsageBits = usage;
    return NO_ERROR;
}

status_t BufferQueue::setTransformHint(uint32_t hint) {
    ST_LOGV("setTransformHint: %02x", hint);
    Mutex::Autolock lock(mMutex);
    mTransformHint = hint;
    return NO_ERROR;
}

status_t BufferQueue::updateDirtyRegion(int bufferidx, int l, int t,
                                       int r, int b) {
    Mutex::Autolock lock(mMutex);
    ST_LOGV("updateDirtyRegion: buffer idx:%d, dirty rect:[%d,%d][%d,%d]",
            bufferidx, l, t, r, b);
    Rect x(l,t,r,b);
    mDirtyRegion[bufferidx].set(x);
    return OK;
}

status_t BufferQueue::setCurrentDirtyRegion(int cur) {
    Mutex::Autolock lock(mMutex);
    ST_LOGV("setCurrentDirtyRegion");

    mCurrentDirtyRegion.set(mDirtyRegion[cur]);

    if(mCurrentDirtyRegion.isEmpty()) {
            mCurrentDirtyRegion.clear();
    }

    mDirtyRegion[cur].clear();
    return OK;
}

status_t BufferQueue::getCurrentDirtyRegion(Rect& dirtyRect) {
    Mutex::Autolock lock(mMutex);
    dirtyRect = mCurrentDirtyRegion;
    return OK;
}

status_t BufferQueue::setBufferCount(int bufferCount) {
    ST_LOGV("setBufferCount: count=%d", bufferCount);

    sp<IConsumerListener> listener;
    {
        Mutex::Autolock lock(mMutex);

        if (mAbandoned) {
            ST_LOGE("setBufferCount: BufferQueue has been abandoned!");
            return NO_INIT;
        }
        if (bufferCount > NUM_BUFFER_SLOTS) {
            ST_LOGE("setBufferCount: bufferCount too large (max %d)",
                    NUM_BUFFER_SLOTS);
            return BAD_VALUE;
        }

        // Error out if the user has dequeued buffers
        for (int i=0 ; i<NUM_BUFFER_SLOTS; i++) {
            if (mSlots[i].mBufferState == BufferSlot::DEQUEUED) {
                ST_LOGE("setBufferCount: client owns some buffers");
                return -EINVAL;
            }
        }

        if (bufferCount == 0) {
            mOverrideMaxBufferCount = 0;
            mDequeueCondition.broadcast();
            return NO_ERROR;
        }

        // fine to assume async to false before we're setting the buffer count
        const int minBufferSlots = getMinMaxBufferCountLocked(false);
        if (bufferCount < minBufferSlots) {
            ST_LOGE("setBufferCount: requested buffer count (%d) is less than "
                    "minimum (%d)", bufferCount, minBufferSlots);
            return BAD_VALUE;
        }
BufferQueue::ProxyConsumerListener::ProxyConsumerListener(
        const wp<ConsumerListener>& consumerListener):
        mConsumerListener(consumerListener) {}

BufferQueue::ProxyConsumerListener::~ProxyConsumerListener() {}

void BufferQueue::ProxyConsumerListener::onFrameAvailable() {
    sp<ConsumerListener> listener(mConsumerListener.promote());
    if (listener != NULL) {
        listener->onBuffersReleased();
    }

    return NO_ERROR;
}

status_t BufferQueue::setBuffersSize(int size) {
    ST_LOGV("setBuffersSize: size=%d", size);
    Mutex::Autolock lock(mMutex);
    mGraphicBufferAlloc->setGraphicBufferSize(size);
    return NO_ERROR;
}

int BufferQueue::query(int what, int* outValue)
{
    ATRACE_CALL();
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        ST_LOGE("query: BufferQueue has been abandoned!");
        return NO_INIT;
    }

    int value;
    switch (what) {
    case NATIVE_WINDOW_WIDTH:
        value = mDefaultWidth;
        break;
    case NATIVE_WINDOW_HEIGHT:
        value = mDefaultHeight;
        break;
    case NATIVE_WINDOW_FORMAT:
        value = mDefaultBufferFormat;
        break;
    case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
        value = getMinUndequeuedBufferCount(false);
        break;
    case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND:
        value = (mQueue.size() >= 2);
        break;
    case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
        value = mConsumerUsageBits;
        break;
    default:
        return BAD_VALUE;
    }
    outValue[0] = value;
    return NO_ERROR;
}

status_t BufferQueue::requestBuffer(int slot, sp<GraphicBuffer>* buf) {
    ATRACE_CALL();
    ST_LOGV("requestBuffer: slot=%d", slot);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("requestBuffer: BufferQueue has been abandoned!");
        return NO_INIT;
    }
    if (slot < 0 || slot >= NUM_BUFFER_SLOTS) {
        ST_LOGE("requestBuffer: slot index out of range [0, %d]: %d",
                NUM_BUFFER_SLOTS, slot);
        return BAD_VALUE;
    } else if (mSlots[slot].mBufferState != BufferSlot::DEQUEUED) {
        ST_LOGE("requestBuffer: slot %d is not owned by the client (state=%d)",
                slot, mSlots[slot].mBufferState);
        return BAD_VALUE;
    }
    mSlots[slot].mRequestBufferCalled = true;
    *buf = mSlots[slot].mGraphicBuffer;
    return NO_ERROR;
}

status_t BufferQueue::dequeueBuffer(int *outBuf, sp<Fence>* outFence, bool async,
        uint32_t w, uint32_t h, uint32_t format, uint32_t usage) {
    ATRACE_CALL();
    ST_LOGV("dequeueBuffer: w=%d h=%d fmt=%#x usage=%#x", w, h, format, usage);

    if ((w && !h) || (!w && h)) {
        ST_LOGE("dequeueBuffer: invalid size: w=%u, h=%u", w, h);
        return BAD_VALUE;
    }

    status_t returnFlags(OK);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSyncKHR eglFence = EGL_NO_SYNC_KHR;

    { // Scope for the lock
        Mutex::Autolock lock(mMutex);

        if (format == 0) {
            format = mDefaultBufferFormat;
        }
        // turn on usage bits the consumer requested
        usage |= mConsumerUsageBits;

        int found = -1;
        bool tryAgain = true;
        while (tryAgain) {
            if (mAbandoned) {
                ST_LOGE("dequeueBuffer: BufferQueue has been abandoned!");
                return NO_INIT;
            }

            const int maxBufferCount = getMaxBufferCountLocked(async);
            if (async && mOverrideMaxBufferCount) {
                // FIXME: some drivers are manually setting the buffer-count (which they
                // shouldn't), so we do this extra test here to handle that case.
                // This is TEMPORARY, until we get this fixed.
                if (mOverrideMaxBufferCount < maxBufferCount) {
                    ST_LOGE("dequeueBuffer: async mode is invalid with buffercount override");
                    return BAD_VALUE;
                }
            }

            // Free up any buffers that are in slots beyond the max buffer
            // count.
            for (int i = maxBufferCount; i < NUM_BUFFER_SLOTS; i++) {
                assert(mSlots[i].mBufferState == BufferSlot::FREE);
                if (mSlots[i].mGraphicBuffer != NULL) {
                    freeBufferLocked(i);
                    returnFlags |= IGraphicBufferProducer::RELEASE_ALL_BUFFERS;
                }
            }

            // look for a free buffer to give to the client
            found = INVALID_BUFFER_SLOT;
            int dequeuedCount = 0;
            int acquiredCount = 0;
            for (int i = 0; i < maxBufferCount; i++) {
                const int state = mSlots[i].mBufferState;
                switch (state) {
                    case BufferSlot::DEQUEUED:
                        dequeuedCount++;
                        break;
                    case BufferSlot::ACQUIRED:
                        acquiredCount++;
                        break;
                    case BufferSlot::FREE:
                        /* We return the oldest of the free buffers to avoid
                         * stalling the producer if possible.  This is because
                         * the consumer may still have pending reads of the
                         * buffers in flight.
                         */
                        if ((found < 0) ||
                                mSlots[i].mFrameNumber < mSlots[found].mFrameNumber) {
                            found = i;
                        }
                        break;
                }
            }

            // clients are not allowed to dequeue more than one buffer
            // if they didn't set a buffer count.
            if (!mOverrideMaxBufferCount && dequeuedCount) {
                ST_LOGE("dequeueBuffer: can't dequeue multiple buffers without "
                        "setting the buffer count");
                return -EINVAL;
            }

            // See whether a buffer has been queued since the last
            // setBufferCount so we know whether to perform the min undequeued
            // buffers check below.
            if (mBufferHasBeenQueued) {
                // make sure the client is not trying to dequeue more buffers
                // than allowed.
                const int newUndequeuedCount = maxBufferCount - (dequeuedCount+1);
                const int minUndequeuedCount = getMinUndequeuedBufferCount(async);
                if (newUndequeuedCount < minUndequeuedCount) {
                    ST_LOGE("dequeueBuffer: min undequeued buffer count (%d) "
                            "exceeded (dequeued=%d undequeudCount=%d)",
                            minUndequeuedCount, dequeuedCount,
                            newUndequeuedCount);
                    return -EBUSY;
                }
            }

            // If no buffer is found, wait for a buffer to be released or for
            // the max buffer count to change.
            tryAgain = found == INVALID_BUFFER_SLOT;
            if (tryAgain) {
                // return an error if we're in "cannot block" mode (producer and consumer
                // are controlled by the application) -- however, the consumer is allowed
                // to acquire briefly an extra buffer (which could cause us to have to wait here)
                // and that's okay because we know the wait will be brief (it happens
                // if we dequeue a buffer while the consumer has acquired one but not released
                // the old one yet -- for e.g.: see GLConsumer::updateTexImage()).
                if (mDequeueBufferCannotBlock && (acquiredCount <= mMaxAcquiredBufferCount)) {
                    ST_LOGE("dequeueBuffer: would block! returning an error instead.");
                    return WOULD_BLOCK;
                }
                mDequeueCondition.wait(mMutex);
            }
        }


        if (found == INVALID_BUFFER_SLOT) {
            // This should not happen.
            ST_LOGE("dequeueBuffer: no available buffer slots");
            return -EBUSY;
        }

        const int buf = found;
        *outBuf = found;

        ATRACE_BUFFER_INDEX(buf);

        const bool useDefaultSize = !w && !h;
        if (useDefaultSize) {
            // use the default size
            w = mDefaultWidth;
            h = mDefaultHeight;
        }

        mSlots[buf].mBufferState = BufferSlot::DEQUEUED;

        const sp<GraphicBuffer>& buffer(mSlots[buf].mGraphicBuffer);
        if ((buffer == NULL) ||
            (uint32_t(buffer->width)  != w) ||
            (uint32_t(buffer->height) != h) ||
            (uint32_t(buffer->format) != format) ||
            ((uint32_t(buffer->usage) & usage) != usage))
        {
            mSlots[buf].mAcquireCalled = false;
            mSlots[buf].mGraphicBuffer = NULL;
            mSlots[buf].mRequestBufferCalled = false;
            mSlots[buf].mEglFence = EGL_NO_SYNC_KHR;
            mSlots[buf].mFence = Fence::NO_FENCE;
            mSlots[buf].mEglDisplay = EGL_NO_DISPLAY;

            returnFlags |= IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION;
        }


        if (CC_UNLIKELY(mSlots[buf].mFence == NULL)) {
            ST_LOGE("dequeueBuffer: about to return a NULL fence from mSlot. "
                    "buf=%d, w=%d, h=%d, format=%d",
                    buf, buffer->width, buffer->height, buffer->format);
        }

        dpy = mSlots[buf].mEglDisplay;
        eglFence = mSlots[buf].mEglFence;
        *outFence = mSlots[buf].mFence;
        mSlots[buf].mEglFence = EGL_NO_SYNC_KHR;
        mSlots[buf].mFence = Fence::NO_FENCE;
    }  // end lock scope

    if (returnFlags & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) {
        status_t error;
        sp<GraphicBuffer> graphicBuffer(
                mGraphicBufferAlloc->createGraphicBuffer(w, h, format, usage, &error));
        if (graphicBuffer == 0) {
            ST_LOGE("dequeueBuffer: SurfaceComposer::createGraphicBuffer failed");
            return error;
        }

        { // Scope for the lock
            Mutex::Autolock lock(mMutex);

            if (mAbandoned) {
                ST_LOGE("dequeueBuffer: BufferQueue has been abandoned!");
                return NO_INIT;
            }

            mSlots[*outBuf].mFrameNumber = ~0;
            mSlots[*outBuf].mGraphicBuffer = graphicBuffer;
        }
    }

    if (eglFence != EGL_NO_SYNC_KHR) {
        EGLint result = eglClientWaitSyncKHR(dpy, eglFence, 0, 1000000000);
        // If something goes wrong, log the error, but return the buffer without
        // synchronizing access to it.  It's too late at this point to abort the
        // dequeue operation.
        if (result == EGL_FALSE) {
            ST_LOGE("dequeueBuffer: error waiting for fence: %#x", eglGetError());
        } else if (result == EGL_TIMEOUT_EXPIRED_KHR) {
            ST_LOGE("dequeueBuffer: timeout waiting for fence");
        }
        eglDestroySyncKHR(dpy, eglFence);
    }

    ST_LOGV("dequeueBuffer: returning slot=%d/%llu buf=%p flags=%#x", *outBuf,
            mSlots[*outBuf].mFrameNumber,
            mSlots[*outBuf].mGraphicBuffer->handle, returnFlags);

    return returnFlags;
}

status_t BufferQueue::queueBuffer(int buf,
        const QueueBufferInput& input, QueueBufferOutput* output) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(buf);

    Rect crop;
    uint32_t transform;
    int scalingMode;
    int64_t timestamp;
    bool isAutoTimestamp;
    bool async;
    sp<Fence> fence;

    input.deflate(&timestamp, &isAutoTimestamp, &crop, &scalingMode, &transform,
            &async, &fence);

    if (fence == NULL) {
        ST_LOGE("queueBuffer: fence is NULL");
        return BAD_VALUE;
    }

    switch (scalingMode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_CROP:
        case NATIVE_WINDOW_SCALING_MODE_NO_SCALE_CROP:
            break;
        default:
            ST_LOGE("unknown scaling mode: %d", scalingMode);
            return -EINVAL;
    }

    sp<IConsumerListener> listener;

    { // scope for the lock
        Mutex::Autolock lock(mMutex);

        if (mAbandoned) {
            ST_LOGE("queueBuffer: BufferQueue has been abandoned!");
            return NO_INIT;
        }

        const int maxBufferCount = getMaxBufferCountLocked(async);
        if (async && mOverrideMaxBufferCount) {
            // FIXME: some drivers are manually setting the buffer-count (which they
            // shouldn't), so we do this extra test here to handle that case.
            // This is TEMPORARY, until we get this fixed.
            if (mOverrideMaxBufferCount < maxBufferCount) {
                ST_LOGE("queueBuffer: async mode is invalid with buffercount override");
                return BAD_VALUE;
            }
        }
        if (buf < 0 || buf >= maxBufferCount) {
            ST_LOGE("queueBuffer: slot index out of range [0, %d]: %d",
                    maxBufferCount, buf);
            return -EINVAL;
        } else if (mSlots[buf].mBufferState != BufferSlot::DEQUEUED) {
            ST_LOGE("queueBuffer: slot %d is not owned by the client "
                    "(state=%d)", buf, mSlots[buf].mBufferState);
            return -EINVAL;
        } else if (!mSlots[buf].mRequestBufferCalled) {
            ST_LOGE("queueBuffer: slot %d was enqueued without requesting a "
                    "buffer", buf);
            return -EINVAL;
        }

        ST_LOGV("queueBuffer: slot=%d/%llu time=%#llx crop=[%d,%d,%d,%d] "
                "tr=%#x scale=%s",
                buf, mFrameCounter + 1, timestamp,
                crop.left, crop.top, crop.right, crop.bottom,
                transform, scalingModeName(scalingMode));

        const sp<GraphicBuffer>& graphicBuffer(mSlots[buf].mGraphicBuffer);
        Rect bufferRect(graphicBuffer->getWidth(), graphicBuffer->getHeight());
        Rect croppedCrop;
        crop.intersect(bufferRect, &croppedCrop);
        if (croppedCrop != crop) {
            ST_LOGE("queueBuffer: crop rect is not contained within the "
                    "buffer in slot %d", buf);
            return -EINVAL;
        }

        mSlots[buf].mFence = fence;
        mSlots[buf].mBufferState = BufferSlot::QUEUED;
        mFrameCounter++;
        mSlots[buf].mFrameNumber = mFrameCounter;

        BufferItem item;
        item.mAcquireCalled = mSlots[buf].mAcquireCalled;
        item.mGraphicBuffer = mSlots[buf].mGraphicBuffer;
        item.mCrop = crop;
        item.mTransform = transform & ~NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY;
        item.mTransformToDisplayInverse = bool(transform & NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY);
        item.mScalingMode = scalingMode;
        item.mTimestamp = timestamp;
        item.mIsAutoTimestamp = isAutoTimestamp;
        item.mFrameNumber = mFrameCounter;
        item.mBuf = buf;
        item.mFence = fence;
        item.mIsDroppable = mDequeueBufferCannotBlock || async;

        if (mQueue.empty()) {
            // when the queue is empty, we can ignore "mDequeueBufferCannotBlock", and
            // simply queue this buffer.
            mQueue.push_back(item);
            listener = mConsumerListener;
        } else {
            // when the queue is not empty, we need to look at the front buffer
            // state and see if we need to replace it.
            Fifo::iterator front(mQueue.begin());
            if (front->mIsDroppable) {
                // buffer slot currently queued is marked free if still tracked
                if (stillTracking(front)) {
                    mSlots[front->mBuf].mBufferState = BufferSlot::FREE;
                    // reset the frame number of the freed buffer so that it is the first in
                    // line to be dequeued again.
                    mSlots[front->mBuf].mFrameNumber = 0;
                }
                // and we record the new buffer in the queued list
                *front = item;
            } else {
                mQueue.push_back(item);
                listener = mConsumerListener;
            }
        }

        mBufferHasBeenQueued = true;
        mDequeueCondition.broadcast();

        output->inflate(mDefaultWidth, mDefaultHeight, mTransformHint,
                mQueue.size());

        ATRACE_INT(mConsumerName.string(), mQueue.size());
    } // scope for the lock

    // call back without lock held
    if (listener != 0) {
        listener->onFrameAvailable();
    }
}

void BufferQueue::ProxyConsumerListener::onBuffersReleased() {
    sp<ConsumerListener> listener(mConsumerListener.promote());
    if (listener != NULL) {
        listener->onBuffersReleased();
    }
}

void BufferQueue::ProxyConsumerListener::onSidebandStreamChanged() {
    sp<ConsumerListener> listener(mConsumerListener.promote());
    if (listener != NULL) {
        listener->onSidebandStreamChanged();
    }
}

void BufferQueue::createBufferQueue(sp<IGraphicBufferProducer>* outProducer,
        sp<IGraphicBufferConsumer>* outConsumer,
        const sp<IGraphicBufferAlloc>& allocator) {
    LOG_ALWAYS_FATAL_IF(outProducer == NULL,
            "BufferQueue: outProducer must not be NULL");
    LOG_ALWAYS_FATAL_IF(outConsumer == NULL,
            "BufferQueue: outConsumer must not be NULL");

    sp<BufferQueueCore> core(new BufferQueueCore(allocator));
    LOG_ALWAYS_FATAL_IF(core == NULL,
            "BufferQueue: failed to create BufferQueueCore");

    sp<IGraphicBufferProducer> producer(new BufferQueueProducer(core));
    LOG_ALWAYS_FATAL_IF(producer == NULL,
            "BufferQueue: failed to create BufferQueueProducer");

    sp<IGraphicBufferConsumer> consumer(new BufferQueueConsumer(core));
    LOG_ALWAYS_FATAL_IF(consumer == NULL,
            "BufferQueue: failed to create BufferQueueConsumer");

    *outProducer = producer;
    *outConsumer = consumer;
}

}; // namespace android
