/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CMI8788Driver_CMI8788AudioEngine_h
#define CMI8788Driver_CMI8788AudioEngine_h

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/audio/IOAudioEngine.h>

#include "CMI8788Chip.h"

//! Stereo playback (multichannel DMA -> PCM1792A) and capture (DMA B <- CS5381),
//! 24-bit samples in 32-bit little-endian containers, one shared sample clock.
class CMI8788AudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(CMI8788AudioEngine);
    typedef IOAudioEngine super;

public:
    bool initWithChip(CMI8788Chip *chip, IOPCIDevice *pci);
    bool initHardware(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    UInt32 getCurrentSampleFrame() override;
    IOReturn performAudioEngineStart() override;
    IOReturn performAudioEngineStop() override;
    IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat,
                                 const IOAudioSampleRate *newSampleRate) override;

    IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame,
                               UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat,
                               IOAudioStream *audioStream) override;
    IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame,
                                 UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat,
                                 IOAudioStream *audioStream) override;

private:
    struct DMABuffer {
        IOBufferMemoryDescriptor *memory;
        IODMACommand *command;
        void *address;
        UInt32 busAddress;
    };

    bool allocateDMABuffer(DMABuffer &buffer, UInt32 bytes);
    void freeDMABuffer(DMABuffer &buffer);
    IOAudioStream *createStream(IOAudioStreamDirection direction, DMABuffer &buffer);

    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source);
    static void interruptHandler(OSObject *owner, IOInterruptEventSource *source, int count);
    static void debugTimerFired(OSObject *owner, IOTimerEventSource *timer);

    CMI8788Chip *chip_;
    IOPCIDevice *pci_;
    IOFilterInterruptEventSource *interruptSource_;
    IOTimerEventSource *debugTimer_;
    DMABuffer output_;
    DMABuffer input_;
    volatile bool gpioChanged_;
    volatile UInt32 convertCalls_;
};

#endif
