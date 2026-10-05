/* SPDX-License-Identifier: GPL-2.0-only */
#include <IOKit/audio/IOAudioBlitterLibDispatch.h>
#include <IOKit/audio/IOAudioStream.h>

#include "CMI8788AudioEngine.h"

#define NUM_SAMPLE_FRAMES   16384
#define NUM_CHANNELS        2
#define BYTES_PER_SAMPLE    4
#define BYTES_PER_FRAME     (NUM_CHANNELS * BYTES_PER_SAMPLE)
#define BUFFER_SIZE         (NUM_SAMPLE_FRAMES * BYTES_PER_FRAME)
#define INITIAL_SAMPLE_RATE 48000

/* The multichannel DMA FIFO is 1024 bytes (oxygen_pcm.c FIFO_BYTES_MULTICH),
 * the recording one 256 bytes: that much is in flight past the DMA pointer. */
#define OUTPUT_LATENCY_FRAMES (1024 / BYTES_PER_FRAME)
#define INPUT_LATENCY_FRAMES  (256 / BYTES_PER_FRAME)
/* Keep CoreAudio this far ahead of the DMA read pointer (burst is 8 dwords). */
#define SAMPLE_OFFSET_FRAMES  32

#define DMA_CHANNELS (OXYGEN_CHANNEL_MULTICH | OXYGEN_CHANNEL_B)

#define LOG(fmt, ...) IOLog("CMI8788AudioEngine: " fmt "\n", ##__VA_ARGS__)

static const UInt32 kSampleRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

OSDefineMetaClassAndStructors(CMI8788AudioEngine, IOAudioEngine);

bool CMI8788AudioEngine::initWithChip(CMI8788Chip *chip, IOPCIDevice *pci)
{
    if (!chip || !pci || !super::init(NULL))
        return false;
    chip_ = chip;
    pci_ = pci;
    pci_->retain();
    return true;
}

bool CMI8788AudioEngine::allocateDMABuffer(DMABuffer &buffer, UInt32 bytes)
{
    /* The CMI8788 takes 32-bit bus addresses for one contiguous buffer. */
    buffer.memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes,
        0x00000000FFFFF000ULL);
    if (!buffer.memory)
        return false;
    buffer.address = buffer.memory->getBytesNoCopy();
    bzero(buffer.address, bytes);

    buffer.command = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, 0,
                                                     IODMACommand::kMapped, 0, 1);
    if (!buffer.command || buffer.command->setMemoryDescriptor(buffer.memory) != kIOReturnSuccess)
        return false;

    IODMACommand::Segment64 segment;
    UInt32 numSegments = 1;
    UInt64 offset = 0;
    if (buffer.command->gen64IOVMSegments(&offset, &segment, &numSegments) != kIOReturnSuccess ||
        numSegments != 1 || segment.fLength < bytes || segment.fIOVMAddr > 0xffffffffULL) {
        LOG("DMA buffer is not one 32-bit segment");
        return false;
    }
    buffer.busAddress = (UInt32)segment.fIOVMAddr;
    return true;
}

void CMI8788AudioEngine::freeDMABuffer(DMABuffer &buffer)
{
    if (buffer.command) {
        buffer.command->clearMemoryDescriptor();
        OSSafeReleaseNULL(buffer.command);
    }
    OSSafeReleaseNULL(buffer.memory);
    buffer.address = NULL;
    buffer.busAddress = 0;
}

IOAudioStream *CMI8788AudioEngine::createStream(IOAudioStreamDirection direction, DMABuffer &buffer)
{
    IOAudioStream *stream = new IOAudioStream;
    if (!stream)
        return NULL;
    if (!stream->initWithAudioEngine(this, direction, 1)) {
        stream->release();
        return NULL;
    }
    stream->setSampleBuffer(buffer.address, BUFFER_SIZE);

    IOAudioStreamFormat format = {
        NUM_CHANNELS,
        kIOAudioStreamSampleFormatLinearPCM,
        kIOAudioStreamNumericRepresentationSignedInt,
        24,                                 /* bit depth: the chip uses the top 24 */
        32,                                 /* bit width */
        kIOAudioStreamAlignmentHighByte,
        kIOAudioStreamByteOrderLittleEndian,
        true,                               /* mixable */
        0
    };
    for (UInt32 rate : kSampleRates) {
        IOAudioSampleRate sampleRate = { rate, 0 };
        stream->addAvailableFormat(&format, &sampleRate, &sampleRate);
    }
    /* the hardware rate is programmed by initHardware, not via the driver */
    stream->setFormat(&format, false);
    return stream;
}

bool CMI8788AudioEngine::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;

    setDescription(chip_->modelName());
    IOAudioSampleRate initialRate = { INITIAL_SAMPLE_RATE, 0 };
    setSampleRate(&initialRate);
    setNumSampleFramesPerBuffer(NUM_SAMPLE_FRAMES);
    setSampleOffset(SAMPLE_OFFSET_FRAMES);
    setOutputSampleLatency(OUTPUT_LATENCY_FRAMES);
    setInputSampleLatency(INPUT_LATENCY_FRAMES);

    if (!allocateDMABuffer(output_, BUFFER_SIZE) || !allocateDMABuffer(input_, BUFFER_SIZE)) {
        LOG("cannot allocate DMA buffers");
        return false;
    }

    /* oxygen_hw_params: one interrupt per buffer wrap, used for timestamps */
    chip_->setDMABuffer(OXYGEN_CHANNEL_MULTICH, output_.busAddress, BUFFER_SIZE, BUFFER_SIZE);
    chip_->setDMABuffer(OXYGEN_CHANNEL_B, input_.busAddress, BUFFER_SIZE, BUFFER_SIZE);
    chip_->setPlaybackRate(INITIAL_SAMPLE_RATE);
    chip_->setCaptureRate(INITIAL_SAMPLE_RATE);

    IOAudioStream *stream = createStream(kIOAudioStreamDirectionOutput, output_);
    if (!stream)
        return false;
    addAudioStream(stream);
    stream->release();

    stream = createStream(kIOAudioStreamDirectionInput, input_);
    if (!stream)
        return false;
    addAudioStream(stream);
    stream->release();

    /* last, so no failure path has to unregister it */
    IOWorkLoop *workLoop = getWorkLoop();
    if (!workLoop)
        return false;
    interruptSource_ = IOFilterInterruptEventSource::filterInterruptEventSource(
        this, interruptHandler, interruptFilter, pci_, 0);
    if (!interruptSource_ || workLoop->addEventSource(interruptSource_) != kIOReturnSuccess) {
        LOG("cannot register interrupt handler");
        OSSafeReleaseNULL(interruptSource_);
        return false;
    }
    interruptSource_->enable();

    /* Diagnostics, only with "Debug" = true in the kext personality: publish
     * DMA positions, input peak and routing registers to the I/O registry once
     * a second (ioreg -l -r -c CMI8788AudioEngine | grep Debug). */
    if (provider->getProperty("Debug") == kOSBooleanTrue) {
        debugTimer_ = IOTimerEventSource::timerEventSource(this, debugTimerFired);
        if (debugTimer_ && workLoop->addEventSource(debugTimer_) == kIOReturnSuccess)
            debugTimer_->setTimeoutMS(1000);
        else
            OSSafeReleaseNULL(debugTimer_);
        LOG("debug diagnostics enabled");
    }

    LOG("output DMA at 0x%08x, input DMA at 0x%08x", output_.busAddress, input_.busAddress);
    return true;
}

void CMI8788AudioEngine::stop(IOService *provider)
{
    if (debugTimer_) {
        debugTimer_->cancelTimeout();
        IOWorkLoop *workLoop = getWorkLoop();
        if (workLoop)
            workLoop->removeEventSource(debugTimer_);
        OSSafeReleaseNULL(debugTimer_);
    }
    chip_->stopDMA(DMA_CHANNELS);
    chip_->disableInterrupts(OXYGEN_CHANNEL_MULTICH);
    if (interruptSource_) {
        interruptSource_->disable();
        IOWorkLoop *workLoop = getWorkLoop();
        if (workLoop)
            workLoop->removeEventSource(interruptSource_);
        OSSafeReleaseNULL(interruptSource_);
    }
    super::stop(provider);
}

void CMI8788AudioEngine::free()
{
    OSSafeReleaseNULL(debugTimer_);
    OSSafeReleaseNULL(interruptSource_);
    freeDMABuffer(output_);
    freeDMABuffer(input_);
    OSSafeReleaseNULL(pci_);
    super::free();
}

/* oxygen_pointer */
UInt32 CMI8788AudioEngine::getCurrentSampleFrame()
{
    UInt32 offset = chip_->dmaPosition(OXYGEN_CHANNEL_MULTICH) - output_.busAddress;
    return (offset / BYTES_PER_FRAME) % NUM_SAMPLE_FRAMES;
}

/* oxygen_prepare + oxygen_trigger(START), both directions together */
IOReturn CMI8788AudioEngine::performAudioEngineStart()
{
    chip_->flushDMA(DMA_CHANNELS);
    chip_->enableInterrupts(OXYGEN_CHANNEL_MULTICH);
    takeTimeStamp(false);
    chip_->startDMA(DMA_CHANNELS);
    return kIOReturnSuccess;
}

/* oxygen_trigger(STOP) + oxygen_hw_free */
IOReturn CMI8788AudioEngine::performAudioEngineStop()
{
    chip_->stopDMA(DMA_CHANNELS);
    chip_->disableInterrupts(OXYGEN_CHANNEL_MULTICH);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioEngine::performFormatChange(IOAudioStream *audioStream,
                                                 const IOAudioStreamFormat *newFormat,
                                                 const IOAudioSampleRate *newSampleRate)
{
    if (newSampleRate) {
        LOG("sample rate -> %u", newSampleRate->whole);
        chip_->setPlaybackRate(newSampleRate->whole);
        chip_->setCaptureRate(newSampleRate->whole);
    }
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf,
                                               UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                               const IOAudioStreamFormat *streamFormat,
                                               IOAudioStream *audioStream)
{
    UInt32 first = firstSampleFrame * streamFormat->fNumChannels;
    IOAF_Float32ToNativeInt32((const Float32 *)mixBuf + first, (SInt32 *)sampleBuf + first,
                              numSampleFrames * streamFormat->fNumChannels);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioEngine::convertInputSamples(const void *sampleBuf, void *destBuf,
                                                 UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                                 const IOAudioStreamFormat *streamFormat,
                                                 IOAudioStream *audioStream)
{
    UInt32 first = firstSampleFrame * streamFormat->fNumChannels;
    convertCalls_++;
    IOAF_NativeInt32ToFloat32((const SInt32 *)sampleBuf + first, (Float32 *)destBuf,
                              numSampleFrames * streamFormat->fNumChannels);
    return kIOReturnSuccess;
}

/* oxygen_interrupt, primary-interrupt half. The line may be shared. */
bool CMI8788AudioEngine::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    CMI8788AudioEngine *engine = (CMI8788AudioEngine *)owner;
    UInt16 status = engine->chip_->interruptStatus();
    if (status == 0 || status == 0xffff)
        return false;
    engine->chip_->ackInterrupts(status);

    if (status & OXYGEN_CHANNEL_MULTICH)
        engine->takeTimeStamp();
    if (status & OXYGEN_INT_GPIO) {
        engine->gpioChanged_ = true;
        return true;    /* handle on the work loop */
    }
    return false;
}

/* xonar_ext_power_gpio_changed */
void CMI8788AudioEngine::interruptHandler(OSObject *owner, IOInterruptEventSource *source, int count)
{
    CMI8788AudioEngine *engine = (CMI8788AudioEngine *)owner;
    if (!engine->gpioChanged_)
        return;
    engine->gpioChanged_ = false;
    if (engine->chip_->hasExternalPower())
        LOG("external power restored");
    else
        LOG("external power cable unplugged!");
}

void CMI8788AudioEngine::debugTimerFired(OSObject *owner, IOTimerEventSource *timer)
{
    CMI8788AudioEngine *engine = (CMI8788AudioEngine *)owner;
    CMI8788Chip *chip = engine->chip_;

    const SInt32 *in = (const SInt32 *)engine->input_.address;
    UInt32 peak = 0, nonzero = 0;
    for (UInt32 i = 0; in && i < BUFFER_SIZE / BYTES_PER_SAMPLE; ++i) {
        SInt32 v = in[i];
        UInt32 mag = v < 0 ? (UInt32)(-(SInt64)v) : (UInt32)v;
        if (mag > peak)
            peak = mag;
        if (v)
            ++nonzero;
    }
    engine->setProperty("DebugInputPeak", peak, 32);
    engine->setProperty("DebugConvertInputCalls", engine->convertCalls_, 32);
    engine->setProperty("DebugInputNonzeroSamples", nonzero, 32);
    engine->setProperty("DebugInputDMAOffset",
                        chip->dmaPosition(OXYGEN_CHANNEL_B) - engine->input_.busAddress, 32);
    engine->setProperty("DebugOutputDMAOffset",
                        chip->dmaPosition(OXYGEN_CHANNEL_MULTICH) - engine->output_.busAddress, 32);
    engine->setProperty("DebugDMAStatus", chip->read8(OXYGEN_DMA_STATUS), 8);
    engine->setProperty("DebugRecRouting", chip->read8(OXYGEN_REC_ROUTING), 8);
    engine->setProperty("DebugRecFormat", chip->read8(OXYGEN_REC_FORMAT), 8);
    engine->setProperty("DebugI2SBFormat", chip->read16(OXYGEN_I2S_B_FORMAT), 16);
    engine->setProperty("DebugGPIOData", chip->read16(OXYGEN_GPIO_DATA), 16);
    engine->setProperty("DebugGPIOControl", chip->read16(OXYGEN_GPIO_CONTROL), 16);
    engine->setProperty("DebugMisc", chip->read8(OXYGEN_MISC), 8);
    engine->setProperty("DebugFunction", chip->read8(OXYGEN_FUNCTION), 8);
    timer->setTimeoutMS(1000);
}
