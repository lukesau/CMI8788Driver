/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CMI8788Driver_CMI8788AudioDevice_h
#define CMI8788Driver_CMI8788AudioDevice_h

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioControl.h>

#include "CMI8788Chip.h"

class CMI8788AudioEngine;

//! Matches the CMI8788 on a Xonar Essence STX / STX II, owns the hardware
//! layer, and publishes one engine plus its volume / mute / output controls.
class CMI8788AudioDevice : public IOAudioDevice
{
    OSDeclareDefaultStructors(CMI8788AudioDevice);
    typedef IOAudioDevice super;

public:
    bool initHardware(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                     IOAudioDevicePowerState newPowerState,
                                     UInt32 *microsecondsUntilComplete) override;

private:
    bool createAudioEngine();
    SInt8 headphoneGainOffset();

    static IOReturn volumeChangeHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);
    static IOReturn muteChangeHandler(OSObject *target, IOAudioControl *control,
                                      SInt32 oldValue, SInt32 newValue);
    static IOReturn outputChangeHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);

    CMI8788Chip chip_;
    IOPCIDevice *pci_;
    CMI8788AudioEngine *engine_;
    bool enginePausedForSleep_;
    bool chipAttached_;
    UInt8 volume_[2];
};

#endif
