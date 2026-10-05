/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware layer for the C-Media CMI8788 on the Asus Xonar Essence STX / STX II.
 *
 * Ported from the Linux snd-oxygen / snd-virtuoso drivers (oxygen_io.c,
 * oxygen_lib.c, oxygen_pcm.c, xonar_pcm179x.c, xonar_lib.c),
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>.
 *
 * Plain C++ (not an OSObject); owned by CMI8788AudioDevice. Register access
 * goes through the I/O-port BAR0 of the IOPCIDevice.
 */
#ifndef CMI8788_CHIP_H
#define CMI8788_CHIP_H

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "OxygenRegs.h"

class CMI8788Chip
{
public:
    enum Output {
        kOutputSpeakers = 0,     /* rear RCA line out */
        kOutputHeadphones,       /* rear 6.3 mm jack via the TPA6120 amp */
        kOutputFrontPanel,       /* front-panel header */
    };

    enum Input {
        kInputLine = 0,          /* rear jack, straight to the CS5381 */
        kInputMic,               /* rear jack via the CM9780 mic preamp */
        kInputFrontMic,          /* front-panel header via the CM9780 */
    };

    /* DAC volume steps: 0.5 dB each, 0 = -60 dB, kVolumeSteps = 0 dB. */
    static const UInt8 kVolumeSteps = 120;
    /* CM9780 mic gain steps: 1.5 dB each, 0 = -34.5 dB, 31 = +12 dB (plus the
     * fixed +20 dB mic boost). */
    static const UInt8 kMicGainSteps = 31;

    bool attach(IOPCIDevice *pci);
    void detach();

    /* Identification (oxygen_search_pci_id). Returns the subsystem device ID
     * from the EEPROM, 0 if the card is not an STX / STX II. */
    UInt16 identify();
    const char *modelName() const { return modelName_; }

    /* Bring-up (oxygen_init + xonar_stx_init). */
    void configurePCIeBridge();
    void initChip();
    void initModel(UInt16 subdevice);
    void shutdown();

    /* Sleep / wake (oxygen_pci_suspend / oxygen_pci_resume). */
    void suspend();
    void resume();

    /* Runtime controls. */
    void setPlaybackRate(UInt32 rate);
    void setCaptureRate(UInt32 rate);
    void setVolume(UInt8 left, UInt8 right);   /* 0..kVolumeSteps */
    void setMute(bool mute);
    void setOutput(Output output);
    void setHeadphoneGainOffset(SInt8 halfDecibels);
    void setInput(Input input);
    void setMicGain(UInt8 steps);              /* 0..kMicGainSteps */

    enum Monitor { kMonitorOff = 0, kMonitorHalf, kMonitorFull };
    void setInputMonitor(Monitor monitor);     /* line/mic in -> outputs, in hardware */
    void setDACFilterSlow(bool slow);          /* PCM1792A sharp / slow roll-off */
    void setDeemphasis(bool on);
    void setSPDIFOutput(bool on);              /* S/PDIF mirrors the analog stereo */
    bool hasExternalPower();

    /* DMA (multichannel playback, recording channel B). */
    void setDMABuffer(UInt8 channel, UInt32 busAddress, UInt32 bytes, UInt32 periodBytes);
    UInt32 dmaPosition(UInt8 channel);
    void flushDMA(UInt8 channelMask);
    void startDMA(UInt8 channelMask);
    void stopDMA(UInt8 channelMask);

    /* Interrupts. ackInterrupts() is safe from primary interrupt context. */
    UInt16 interruptStatus() { return read16(OXYGEN_INTERRUPT_STATUS); }
    void ackInterrupts(UInt16 status);
    void enableInterrupts(UInt16 mask);
    void disableInterrupts(UInt16 mask);

    /* Diagnostics: the shadow of every register written so far. */
    const UInt8 *shadowRegisters() const { return saved_; }

    /* Register access (oxygen_io.c). */
    UInt8  read8(UInt8 reg)  { return pci_->ioRead8(reg, map_); }
    UInt16 read16(UInt8 reg) { return pci_->ioRead16(reg, map_); }
    UInt32 read32(UInt8 reg) { return pci_->ioRead32(reg, map_); }
    /* Writes are shadowed so resume() can restore them, like Linux's
     * saved_registers. */
    void write8(UInt8 reg, UInt8 value)
    {
        pci_->ioWrite8(reg, value, map_);
        saved_[reg] = value;
    }
    void write16(UInt8 reg, UInt16 value)
    {
        pci_->ioWrite16(reg, value, map_);
        saved_[reg] = (UInt8)value;
        saved_[(UInt8)(reg + 1)] = (UInt8)(value >> 8);
    }
    void write32(UInt8 reg, UInt32 value)
    {
        pci_->ioWrite32(reg, value, map_);
        for (unsigned i = 0; i < 4; ++i)
            saved_[(UInt8)(reg + i)] = (UInt8)(value >> (8 * i));
    }
    void write8Masked(UInt8 reg, UInt8 value, UInt8 mask)    { write8(reg, (read8(reg) & ~mask) | (value & mask)); }
    void write16Masked(UInt8 reg, UInt16 value, UInt16 mask) { write16(reg, (read16(reg) & ~mask) | (value & mask)); }
    void write32Masked(UInt8 reg, UInt32 value, UInt32 mask) { write32(reg, (read32(reg) & ~mask) | (value & mask)); }
    void setBits8(UInt8 reg, UInt8 bits)     { write8Masked(reg, bits, bits); }
    void setBits16(UInt8 reg, UInt16 bits)   { write16Masked(reg, bits, bits); }
    void setBits32(UInt8 reg, UInt32 bits)   { write32Masked(reg, bits, bits); }
    void clearBits8(UInt8 reg, UInt8 bits)   { write8Masked(reg, 0, bits); }
    void clearBits16(UInt8 reg, UInt16 bits) { write16Masked(reg, 0, bits); }
    void clearBits32(UInt8 reg, UInt32 bits) { write32Masked(reg, 0, bits); }

private:
    void writeI2C(UInt8 device, UInt8 map, UInt8 data);
    bool waitAC97(UInt8 mask);
    void writeAC97(unsigned codec, unsigned index, UInt16 data);
    UInt16 readAC97(unsigned codec, unsigned index);
    void writeAC97Masked(unsigned codec, unsigned index, UInt16 data, UInt16 mask);
    void restoreAC97(unsigned codec);
    UInt16 readEEPROM(unsigned index);

    void pcm1796Write(UInt8 reg, UInt8 value);
    void pcm1796WriteCached(UInt8 reg, UInt8 value);
    void pcm1796RegistersInit();
    void updateDACVolume();
    void enableOutput();
    void disableOutput();
    void updateSPDIFSource();

    IOPCIDevice *pci_ = nullptr;
    IOMemoryMap *map_ = nullptr;
    IOSimpleLock *lock_ = nullptr;      /* guards interruptMask_ */
    UInt16 interruptMask_ = 0;

    UInt8 saved_[OXYGEN_IO_SIZE] = {};
    UInt16 savedAC97_[2][0x40] = {};

    const char *modelName_ = "unknown";
    bool hasAC97_0_ = false;
    bool hasAC97_1_ = false;

    /* xonar_pcm179x state for the single PCM1792A. */
    UInt8 pcm1796Regs_[5] = {};         /* registers 16..20 */
    UInt8 dacVolume_[2] = {};           /* raw register values, 135..255 */
    bool dacMute_ = true;
    bool hpActive_ = false;
    SInt8 hpGainOffset_ = 2 * -18;      /* Linux default: "< 32 ohms" */
    UInt32 currentRate_ = 48000;
    bool spdifOut_ = false;
};

#endif /* CMI8788_CHIP_H */
