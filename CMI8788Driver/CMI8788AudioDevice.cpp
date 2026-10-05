/* SPDX-License-Identifier: GPL-2.0-only */
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioTypes.h>

#include "CMI8788AudioDevice.h"
#include "CMI8788AudioEngine.h"

#define LOG(fmt, ...) IOLog("CMI8788AudioDevice: " fmt "\n", ##__VA_ARGS__)

/* Output selector values (shown as data sources in Sound preferences). */
enum {
    kSelectSpeakers   = kIOAudioOutputPortSubTypeLine,        /* 'line' */
    kSelectHeadphones = kIOAudioOutputPortSubTypeHeadphones,  /* 'hdpn' */
    kSelectFrontPanel = 'fhdp',
};

static const UInt8  kInitialVolume = CMI8788Chip::kVolumeSteps - 2 * 30;  /* -30 dB */
static const SInt32 kInitialOutput = kSelectHeadphones;

OSDefineMetaClassAndStructors(CMI8788AudioDevice, IOAudioDevice);

/* Personality key "HeadphoneImpedance" (ohms) picks the st_hp_volume_offset
 * setting; without it we keep the Linux default of -18 dB (< 32 ohms). */
SInt8 CMI8788AudioDevice::headphoneGainOffset()
{
    OSNumber *ohms = OSDynamicCast(OSNumber, getProperty("HeadphoneImpedance"));
    if (!ohms)
        return 2 * -18;
    UInt32 value = ohms->unsigned32BitValue();
    if (value < 32)
        return 2 * -18;
    if (value < 64)
        return 2 * -12;
    if (value < 300)
        return 2 * -6;
    return 0;
}

bool CMI8788AudioDevice::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci || !chip_.attach(pci))
        return false;
    pci_ = pci;
    pci_->retain();
    chipAttached_ = true;

    UInt16 subdevice = chip_.identify();
    if (!subdevice) {
        LOG("not a Xonar Essence STX / STX II, not attaching");
        return false;
    }

    chip_.configurePCIeBridge();
    chip_.initChip();
    chip_.initModel(subdevice);
    chip_.setHeadphoneGainOffset(headphoneGainOffset());

    setDeviceName(chip_.modelName());
    setDeviceShortName("Xonar STX");
    setManufacturerName("ASUS");

    if (!createAudioEngine()) {
        chip_.shutdown();
        return false;
    }

    /* the engine's interrupt handler exists now: watch the power connector */
    chip_.enableInterrupts(OXYGEN_INT_GPIO);
    return true;
}

bool CMI8788AudioDevice::createAudioEngine()
{
    CMI8788AudioEngine *engine = new CMI8788AudioEngine;
    if (!engine || !engine->initWithChip(&chip_, pci_)) {
        OSSafeReleaseNULL(engine);
        return false;
    }

    /* PCM1792A attenuator: 0.5 dB steps from -60 dB to 0 dB */
    IOAudioControl *control;
    static const struct { UInt32 channel; const char *name; } channels[] = {
        { kIOAudioControlChannelIDDefaultLeft, "Left" },
        { kIOAudioControlChannelIDDefaultRight, "Right" },
    };
    for (const auto &ch : channels) {
        control = IOAudioLevelControl::createVolumeControl(
            kInitialVolume, 0, CMI8788Chip::kVolumeSteps, -(60 << 16), 0,
            ch.channel, ch.name, 0, kIOAudioControlUsageOutput);
        if (!control)
            goto fail;
        control->setValueChangeHandler(volumeChangeHandler, this);
        engine->addDefaultAudioControl(control);
        control->release();
    }

    control = IOAudioToggleControl::createMuteControl(
        false, kIOAudioControlChannelIDAll, "All", 0,
        kIOAudioControlUsageOutput);
    if (!control)
        goto fail;
    control->setValueChangeHandler(muteChangeHandler, this);
    engine->addDefaultAudioControl(control);
    control->release();

    {
        IOAudioSelectorControl *selector = IOAudioSelectorControl::createOutputSelector(
            kInitialOutput, kIOAudioControlChannelIDAll, "All");
        if (!selector)
            goto fail;
        selector->addAvailableSelection(kSelectHeadphones, "Headphones");
        selector->addAvailableSelection(kSelectSpeakers, "Line Out");
        selector->addAvailableSelection(kSelectFrontPanel, "Front Panel Headphones");
        selector->setValueChangeHandler(outputChangeHandler, this);
        engine->addDefaultAudioControl(selector);
        selector->release();
    }

    /* Controls don't call their handlers for the initial value. */
    volume_[0] = volume_[1] = kInitialVolume;
    chip_.setOutput(CMI8788Chip::kOutputHeadphones);
    chip_.setVolume(volume_[0], volume_[1]);
    chip_.setMute(false);

    if (activateAudioEngine(engine) != kIOReturnSuccess)
        goto fail;
    engine_ = engine;           /* keep our reference for sleep/wake */
    return true;

fail:
    LOG("cannot create audio engine");
    engine->release();
    return false;
}

void CMI8788AudioDevice::stop(IOService *provider)
{
    super::stop(provider);      /* stops and detaches the engine first */
    if (chipAttached_)
        chip_.shutdown();
}

void CMI8788AudioDevice::free()
{
    OSSafeReleaseNULL(engine_);
    if (chipAttached_) {
        chip_.detach();
        chipAttached_ = false;
    }
    OSSafeReleaseNULL(pci_);
    super::free();
}

/* Idle <-> Active needs nothing: the chip stays powered while the system is
 * awake. Only system sleep loses the CMI8788's register state.
 *
 * IOAudioFamily leaves a running engine marked running across sleep, so pause
 * it here and resume it after the chip is restored (what ALSA's suspend /
 * resume of open PCM streams does on Linux). resumeAudioEngine() clears the
 * buffers and restarts the DMA through performAudioEngineStart(). */
IOReturn CMI8788AudioDevice::performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                                     IOAudioDevicePowerState newPowerState,
                                                     UInt32 *microsecondsUntilComplete)
{
    if (!chipAttached_)
        return kIOReturnSuccess;
    if (newPowerState == kIOAudioDeviceSleep && oldPowerState != kIOAudioDeviceSleep) {
        if (engine_ && engine_->getState() == kIOAudioEngineRunning) {
            engine_->pauseAudioEngine();
            enginePausedForSleep_ = true;
        }
        chip_.suspend();
    } else if (oldPowerState == kIOAudioDeviceSleep && newPowerState != kIOAudioDeviceSleep) {
        chip_.resume();
        if (enginePausedForSleep_ && engine_) {
            engine_->resumeAudioEngine();
            enginePausedForSleep_ = false;
        }
    }
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::volumeChangeHandler(OSObject *target, IOAudioControl *control,
                                                 SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device || !control)
        return kIOReturnBadArgument;
    if (newValue < 0 || newValue > CMI8788Chip::kVolumeSteps)
        return kIOReturnBadArgument;
    switch (control->getChannelID()) {
    case kIOAudioControlChannelIDDefaultLeft:
        device->volume_[0] = (UInt8)newValue;
        break;
    case kIOAudioControlChannelIDDefaultRight:
        device->volume_[1] = (UInt8)newValue;
        break;
    default:
        device->volume_[0] = device->volume_[1] = (UInt8)newValue;
        break;
    }
    device->chip_.setVolume(device->volume_[0], device->volume_[1]);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::muteChangeHandler(OSObject *target, IOAudioControl *control,
                                               SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device)
        return kIOReturnBadArgument;
    device->chip_.setMute(newValue != 0);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::outputChangeHandler(OSObject *target, IOAudioControl *control,
                                                 SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device)
        return kIOReturnBadArgument;
    switch (newValue) {
    case kSelectSpeakers:
        device->chip_.setOutput(CMI8788Chip::kOutputSpeakers);
        break;
    case kSelectHeadphones:
        device->chip_.setOutput(CMI8788Chip::kOutputHeadphones);
        break;
    case kSelectFrontPanel:
        device->chip_.setOutput(CMI8788Chip::kOutputFrontPanel);
        break;
    default:
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}
