/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CMI8788Driver_CMI8788AudioDevice_h
#define CMI8788Driver_CMI8788AudioDevice_h

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/IOTimerEventSource.h>

#include "CMI8788Chip.h"


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
    IOReturn setProperties(OSObject *properties) override;
    IOReturn performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                     IOAudioDevicePowerState newPowerState,
                                     UInt32 *microsecondsUntilComplete) override;

private:
    bool createAudioEngine();
    static SInt8 gainOffsetForImpedance(UInt32 ohms);
    IOReturn applySettings(OSDictionary *settings);
    static IOReturn applySettingsAction(OSObject *owner, void *arg0, void *, void *, void *);

    static IOReturn volumeChangeHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);
    static IOReturn muteChangeHandler(OSObject *target, IOAudioControl *control,
                                      SInt32 oldValue, SInt32 newValue);
    static IOReturn outputChangeHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);
    static IOReturn inputChangeHandler(OSObject *target, IOAudioControl *control,
                                       SInt32 oldValue, SInt32 newValue);
    static IOReturn micGainChangeHandler(OSObject *target, IOAudioControl *control,
                                         SInt32 oldValue, SInt32 newValue);
    static IOReturn passThruMuteHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);
    static IOReturn passThruLevelHandler(OSObject *target, IOAudioControl *control,
                                         SInt32 oldValue, SInt32 newValue);
    void setMonitor(bool on, bool full, IOAudioControl *changedControl);
    void setInputSource(SInt32 selection, IOAudioControl *changedControl);
    void setOutputDestination(SInt32 selection, IOAudioControl *changedControl);
    void scheduleOutputEnable();
    static void outputEnableTimerFired(OSObject *owner, IOTimerEventSource *timer);

    CMI8788Chip chip_;
    IOPCIDevice *pci_;
    /* Input monitoring: on/off plus the level it uses when on (remembered
     * while off). Mirrored to CoreAudio's play-through controls. */
    bool monitorOn_;
    bool monitorFull_;
    IOAudioControl *passThruMute_;
    IOAudioControl *passThruLevel_;
    IOAudioControl *inputSelector_;
    IOAudioControl *outputSelector_;
    IOTimerEventSource *outputEnableTimer_;   /* anti-pop relay delay */
    bool chipAttached_;
    UInt8 volume_[2];
};

#endif
