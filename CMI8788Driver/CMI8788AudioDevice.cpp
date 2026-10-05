/* SPDX-License-Identifier: GPL-2.0-only */
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <IOKit/IOCommandGate.h>

#include "CMI8788AudioDevice.h"
#include "CMI8788AudioEngine.h"

#define LOG(fmt, ...) IOLog("CMI8788AudioDevice: " fmt "\n", ##__VA_ARGS__)

/* Output selector values (shown as data sources in Sound preferences). */
enum {
    kSelectSpeakers   = kIOAudioOutputPortSubTypeLine,        /* 'line' */
    kSelectHeadphones = kIOAudioOutputPortSubTypeHeadphones,  /* 'hdpn' */
    kSelectFrontPanel = 'fhdp',
};

/* Input selector values (shown as inputs in Sound preferences). */
enum {
    kSelectLineIn     = kIOAudioInputPortSubTypeLine,               /* 'line' */
    kSelectMic        = kIOAudioInputPortSubTypeExternalMicrophone, /* 'emic' */
    kSelectFrontMic   = 'fmic',
};

/* Card settings that CoreAudio has no UI for (the STX menu bar app, stxctl).
 * Readable from the I/O registry; writable via the personality or
 * setProperties. */
#define kSettingImpedance  "HeadphoneImpedance"   /* number, ohms */
#define kSettingMonitor    "InputMonitor"         /* "off" | "half" | "full" */
#define kSettingMonitorLvl "InputMonitorLevel"    /* "half" | "full": level used when on */
#define kSettingInput      "InputSource"          /* "line" | "mic" | "frontmic" */
#define kInputNameKey      "InputSourceName"      /* read-only: selected input's name */
#define kSettingOutput     "OutputDestination"    /* "headphones" | "line" | "frontpanel" */
#define kOutputNameKey     "OutputDestinationName" /* read-only: selected output's name */
#define kSettingFilter     "DACFilter"            /* "sharp" | "slow" */
#define kSettingDeemphasis "Deemphasis"           /* boolean */
#define kSettingSPDIF      "SPDIFOutput"          /* boolean: S/PDIF mirrors the analog out */

static const UInt8  kInitialVolume = CMI8788Chip::kVolumeSteps - 2 * 30;  /* -30 dB */
static const SInt32 kInitialOutput = kSelectHeadphones;
static const UInt8  kInitialMicGain = CMI8788Chip::kMicGainSteps - 8;   /* 0 dB + 20 dB boost */

OSDefineMetaClassAndStructors(CMI8788AudioDevice, IOAudioDevice);

/* Add a selection with an explicit port kind (CoreAudio's data source kind,
 * what Sound preferences shows as "Type"). The selection value must be unique
 * per selector, so this is how a second microphone or headphone jack gets a
 * proper type. Kinds with a label: 'hdpn' Headphone port, 'lino' / 'lini'
 * Audio line-out / line-in port, 'emic' Microphone port. */
enum { kKindLineOut = 'lino', kKindLineIn = 'lini' };
static void addSelectionOfKind(IOAudioSelectorControl *selector, SInt32 value,
                               const char *description, UInt32 kind)
{
    OSString *desc = OSString::withCString(description);
    OSNumber *transport = OSNumber::withNumber(kind, 32);
    if (desc && transport)
        selector->addAvailableSelection(value, desc, kIOAudioSelectorControlTransportValueKey,
                                        transport);
    OSSafeReleaseNULL(desc);
    OSSafeReleaseNULL(transport);
}

/* "HeadphoneImpedance" (ohms) picks the st_hp_volume_offset setting, like the
 * Linux "Headphones Impedance" control or the Windows "HP Amp Gain". It comes
 * from the personality at load, or at runtime through setProperties(). Without
 * it we keep the Linux default of -18 dB (< 32 ohms). */
SInt8 CMI8788AudioDevice::gainOffsetForImpedance(UInt32 value)
{
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
    chip_.setHeadphoneGainOffset(gainOffsetForImpedance(0));

    setDeviceName(chip_.modelName());
    setDeviceShortName("Xonar STX");
    setManufacturerName("ASUS");

    if (!createAudioEngine()) {
        chip_.shutdown();
        return false;
    }

    /* the engine's interrupt handler exists now: watch the power connector */
    chip_.enableInterrupts(OXYGEN_INT_GPIO);

    /* Boot-time defaults for the card settings come from the personality
     * (Info.plist); the same keys can be changed later through setProperties. */
    OSDictionary *defaults = OSDictionary::withCapacity(4);
    if (defaults) {
        static const char *const keys[] = { kSettingImpedance, kSettingOutput, kSettingInput,
                                            kSettingMonitorLvl,
                                            kSettingMonitor, kSettingFilter, kSettingDeemphasis,
                                            kSettingSPDIF };
        for (const char *key : keys) {
            OSObject *value = getProperty(key);
            if (value)
                defaults->setObject(key, value);
        }
        if (!defaults->getObject(kSettingMonitor))
            setMonitor(false, false, NULL);
        if (!defaults->getObject(kSettingFilter))
            setProperty(kSettingFilter, "sharp");
        if (!defaults->getObject(kSettingDeemphasis))
            setProperty(kSettingDeemphasis, false);
        if (!defaults->getObject(kSettingSPDIF))
            setProperty(kSettingSPDIF, false);
        applySettings(defaults);
        defaults->release();
    }
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
        addSelectionOfKind(selector, kSelectHeadphones, "Headphones",
                           kIOAudioOutputPortSubTypeHeadphones);
        addSelectionOfKind(selector, kSelectSpeakers, "Line Out", kKindLineOut);
        addSelectionOfKind(selector, kSelectFrontPanel, "Front Panel Headphones",
                           kIOAudioOutputPortSubTypeHeadphones);
        selector->setValueChangeHandler(outputChangeHandler, this);
        engine->addDefaultAudioControl(selector);
        outputSelector_ = selector;     /* keep our reference to sync it */
    }

    {
        IOAudioSelectorControl *selector = IOAudioSelectorControl::createInputSelector(
            kSelectLineIn, kIOAudioControlChannelIDAll, "All");
        if (!selector)
            goto fail;
        addSelectionOfKind(selector, kSelectLineIn, "Line In", kKindLineIn);
        addSelectionOfKind(selector, kSelectMic, "Microphone",
                           kIOAudioInputPortSubTypeExternalMicrophone);
        addSelectionOfKind(selector, kSelectFrontMic, "Front Panel Microphone",
                           kIOAudioInputPortSubTypeExternalMicrophone);
        selector->setValueChangeHandler(inputChangeHandler, this);
        engine->addDefaultAudioControl(selector);
        inputSelector_ = selector;      /* keep our reference to sync it */
    }

    /* CM9780 mic gain incl. the +20 dB boost: -14.5 dB .. +32 dB. Has no
     * effect on line-in, which bypasses the codec. */
    control = IOAudioLevelControl::createVolumeControl(
        kInitialMicGain, 0, CMI8788Chip::kMicGainSteps, -(29 << 15), 32 << 16,
        kIOAudioControlChannelIDAll, "All", 0, kIOAudioControlUsageInput);
    if (!control)
        goto fail;
    control->setValueChangeHandler(micGainChangeHandler, this);
    engine->addDefaultAudioControl(control);
    control->release();

    /* Hardware input monitoring as CoreAudio's standard play-through controls
     * (mute on = monitoring off; level 0 = -6 dB, 1 = 0 dB). */
    passThruMute_ = IOAudioToggleControl::createPassThruMuteControl(
        true, kIOAudioControlChannelIDAll, "All", 0);
    if (!passThruMute_)
        goto fail;
    passThruMute_->setValueChangeHandler(passThruMuteHandler, this);
    engine->addDefaultAudioControl(passThruMute_);
    passThruLevel_ = IOAudioLevelControl::createPassThruVolumeControl(
        0, 0, 1, -(6 << 16), 0, kIOAudioControlChannelIDAll, "All", 0);
    if (!passThruLevel_)
        goto fail;
    passThruLevel_->setValueChangeHandler(passThruLevelHandler, this);
    engine->addDefaultAudioControl(passThruLevel_);

    /* Controls don't call their handlers for the initial value. */
    volume_[0] = volume_[1] = kInitialVolume;
    setInputSource(kSelectLineIn, NULL);
    chip_.setMicGain(kInitialMicGain);
    setOutputDestination(kSelectHeadphones, NULL);
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
    OSSafeReleaseNULL(passThruMute_);
    OSSafeReleaseNULL(passThruLevel_);
    OSSafeReleaseNULL(inputSelector_);
    OSSafeReleaseNULL(outputSelector_);
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

/* Apply any recognized settings in the dictionary and publish their
 * normalized values. Runs on the work loop (or before the device is
 * published). Unknown keys are ignored; an invalid value fails the call. */
IOReturn CMI8788AudioDevice::applySettings(OSDictionary *settings)
{
    IOReturn result = kIOReturnUnsupported;

    if (OSNumber *ohms = OSDynamicCast(OSNumber, settings->getObject(kSettingImpedance))) {
        UInt32 value = ohms->unsigned32BitValue();
        if (value == 0 || value > 100000)
            return kIOReturnBadArgument;
        SInt8 offset = gainOffsetForImpedance(value);
        chip_.setHeadphoneGainOffset(offset);
        setProperty(kSettingImpedance, value, 32);
        LOG("headphone impedance %u ohms -> gain offset %d.%d dB", value, offset / 2,
            (offset % 2) ? 5 : 0);
        result = kIOReturnSuccess;
    }
    if (OSString *output = OSDynamicCast(OSString, settings->getObject(kSettingOutput))) {
        if (output->isEqualTo("headphones"))
            setOutputDestination(kSelectHeadphones, NULL);
        else if (output->isEqualTo("line"))
            setOutputDestination(kSelectSpeakers, NULL);
        else if (output->isEqualTo("frontpanel"))
            setOutputDestination(kSelectFrontPanel, NULL);
        else
            return kIOReturnBadArgument;
        result = kIOReturnSuccess;
    }
    if (OSString *input = OSDynamicCast(OSString, settings->getObject(kSettingInput))) {
        if (input->isEqualTo("line"))
            setInputSource(kSelectLineIn, NULL);
        else if (input->isEqualTo("mic"))
            setInputSource(kSelectMic, NULL);
        else if (input->isEqualTo("frontmic"))
            setInputSource(kSelectFrontMic, NULL);
        else
            return kIOReturnBadArgument;
        result = kIOReturnSuccess;
    }
    if (OSString *level = OSDynamicCast(OSString, settings->getObject(kSettingMonitorLvl))) {
        if (!level->isEqualTo("half") && !level->isEqualTo("full"))
            return kIOReturnBadArgument;
        setMonitor(monitorOn_, level->isEqualTo("full"), NULL);
        result = kIOReturnSuccess;
    }
    if (OSString *mode = OSDynamicCast(OSString, settings->getObject(kSettingMonitor))) {
        if (mode->isEqualTo("off"))
            setMonitor(false, monitorFull_, NULL);
        else if (mode->isEqualTo("half"))
            setMonitor(true, false, NULL);
        else if (mode->isEqualTo("full"))
            setMonitor(true, true, NULL);
        else
            return kIOReturnBadArgument;
        result = kIOReturnSuccess;
    }
    if (OSString *filter = OSDynamicCast(OSString, settings->getObject(kSettingFilter))) {
        if (!filter->isEqualTo("sharp") && !filter->isEqualTo("slow"))
            return kIOReturnBadArgument;
        chip_.setDACFilterSlow(filter->isEqualTo("slow"));
        setProperty(kSettingFilter, filter);
        result = kIOReturnSuccess;
    }
    if (OSBoolean *spdif = OSDynamicCast(OSBoolean, settings->getObject(kSettingSPDIF))) {
        chip_.setSPDIFOutput(spdif->isTrue());
        setProperty(kSettingSPDIF, spdif);
        result = kIOReturnSuccess;
    }
    if (OSBoolean *deemph = OSDynamicCast(OSBoolean, settings->getObject(kSettingDeemphasis))) {
        chip_.setDeemphasis(deemph->isTrue());
        setProperty(kSettingDeemphasis, deemph);
        result = kIOReturnSuccess;
    }
    return result;
}

IOReturn CMI8788AudioDevice::applySettingsAction(OSObject *owner, void *arg0, void *, void *, void *)
{
    return ((CMI8788AudioDevice *)owner)->applySettings((OSDictionary *)arg0);
}

/* Runtime settings from user space (the STX app, stxctl). Like the Linux and
 * Windows mixers, any local user may change these; the loudest result is the
 * 0 dB headphone offset, which the volume slider can reach anyway. */
IOReturn CMI8788AudioDevice::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict)
        return kIOReturnBadArgument;
    if (!chipAttached_ || !getCommandGate())
        return kIOReturnNotReady;
    return getCommandGate()->runAction(applySettingsAction, dict);
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
    if (newValue != kSelectSpeakers && newValue != kSelectHeadphones && newValue != kSelectFrontPanel)
        return kIOReturnBadArgument;
    device->setOutputDestination(newValue, control);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::inputChangeHandler(OSObject *target, IOAudioControl *control,
                                                SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device)
        return kIOReturnBadArgument;
    if (newValue != kSelectLineIn && newValue != kSelectMic && newValue != kSelectFrontMic)
        return kIOReturnBadArgument;
    device->setInputSource(newValue, control);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::micGainChangeHandler(OSObject *target, IOAudioControl *control,
                                                  SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device || newValue < 0 || newValue > CMI8788Chip::kMicGainSteps)
        return kIOReturnBadArgument;
    device->chip_.setMicGain((UInt8)newValue);
    return kIOReturnSuccess;
}

/* Apply a monitoring state from any source and keep the published properties
 * and CoreAudio's play-through controls in step (without re-entering the
 * control that triggered the change). */
void CMI8788AudioDevice::setMonitor(bool on, bool full, IOAudioControl *changedControl)
{
    monitorOn_ = on;
    monitorFull_ = full;
    chip_.setInputMonitor(!on ? CMI8788Chip::kMonitorOff
                              : full ? CMI8788Chip::kMonitorFull : CMI8788Chip::kMonitorHalf);
    setProperty(kSettingMonitor, !on ? "off" : full ? "full" : "half");
    setProperty(kSettingMonitorLvl, full ? "full" : "half");

    OSNumber *n;
    if (passThruMute_ && changedControl != passThruMute_ && (n = OSNumber::withNumber(!on, 32))) {
        passThruMute_->hardwareValueChanged(n);
        n->release();
    }
    if (passThruLevel_ && changedControl != passThruLevel_ && (n = OSNumber::withNumber(full, 32))) {
        passThruLevel_->hardwareValueChanged(n);
        n->release();
    }
}

/* Select the input from any source (Sound preferences via the selector
 * control, the STX app / stxctl via setProperties) and keep the published
 * properties and CoreAudio's selector in step. */
void CMI8788AudioDevice::setInputSource(SInt32 selection, IOAudioControl *changedControl)
{
    chip_.setInput(selection == kSelectMic ? CMI8788Chip::kInputMic
                   : selection == kSelectFrontMic ? CMI8788Chip::kInputFrontMic
                   : CMI8788Chip::kInputLine);
    setProperty(kSettingInput, selection == kSelectMic ? "mic"
                               : selection == kSelectFrontMic ? "frontmic" : "line");
    setProperty(kInputNameKey, selection == kSelectMic ? "Microphone"
                               : selection == kSelectFrontMic ? "Front Panel Microphone"
                               : "Line In");
    OSNumber *n;
    if (inputSelector_ && changedControl != inputSelector_ &&
        (n = OSNumber::withNumber((UInt32)selection, 32))) {
        inputSelector_->hardwareValueChanged(n);
        n->release();
    }
}

IOReturn CMI8788AudioDevice::passThruMuteHandler(OSObject *target, IOAudioControl *control,
                                                 SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device)
        return kIOReturnBadArgument;
    device->setMonitor(newValue == 0, device->monitorFull_, control);
    return kIOReturnSuccess;
}

IOReturn CMI8788AudioDevice::passThruLevelHandler(OSObject *target, IOAudioControl *control,
                                                  SInt32 oldValue, SInt32 newValue)
{
    CMI8788AudioDevice *device = OSDynamicCast(CMI8788AudioDevice, target);
    if (!device || newValue < 0 || newValue > 1)
        return kIOReturnBadArgument;
    device->setMonitor(device->monitorOn_, newValue == 1, control);
    return kIOReturnSuccess;
}

/* Select the output from any source (Sound preferences via the selector
 * control, the STX app / stxctl via setProperties), keeping the published
 * properties and CoreAudio's selector in step. */
void CMI8788AudioDevice::setOutputDestination(SInt32 selection, IOAudioControl *changedControl)
{
    chip_.setOutput(selection == kSelectSpeakers ? CMI8788Chip::kOutputSpeakers
                    : selection == kSelectFrontPanel ? CMI8788Chip::kOutputFrontPanel
                    : CMI8788Chip::kOutputHeadphones);
    setProperty(kSettingOutput, selection == kSelectSpeakers ? "line"
                                : selection == kSelectFrontPanel ? "frontpanel" : "headphones");
    setProperty(kOutputNameKey, selection == kSelectSpeakers ? "Line Out"
                                : selection == kSelectFrontPanel ? "Front Panel Headphones"
                                : "Headphones");
    OSNumber *n;
    if (outputSelector_ && changedControl != outputSelector_ &&
        (n = OSNumber::withNumber((UInt32)selection, 32))) {
        outputSelector_->hardwareValueChanged(n);
        n->release();
    }
}
