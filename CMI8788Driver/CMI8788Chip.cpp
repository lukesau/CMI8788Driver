/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware layer for the CMI8788 on the Xonar Essence STX / STX II.
 * Ported from Linux snd-oxygen / snd-virtuoso, (c) Clemens Ladisch.
 * Function comments name the Linux function each one mirrors.
 */
#include "CMI8788Chip.h"

/* model_xonar_st (xonar_pcm179x.c) */
static const UInt8  kDACMclks       = OXYGEN_MCLKS(512, 128, 128);
static const UInt8  kADCMclks       = OXYGEN_MCLKS(256, 128, 128);
static const UInt16 kDACI2SFormat   = OXYGEN_I2S_FORMAT_I2S;
static const UInt16 kADCI2SFormat   = OXYGEN_I2S_FORMAT_LJUST;
static const UInt8  kDACVolumeMin   = 255 - 2 * 60;

#define LOG(fmt, ...) IOLog("CMI8788: " fmt "\n", ##__VA_ARGS__)

bool CMI8788Chip::attach(IOPCIDevice *pci)
{
    /* The CMI8788 registers live in an I/O-port BAR (Linux: IORESOURCE_IO). */
    UInt32 bar0 = pci->configRead32(kIOPCIConfigBaseAddress0);
    if (!(bar0 & 1)) {
        LOG("BAR0 is not an I/O port range (0x%08x)", bar0);
        return false;
    }
    map_ = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!map_ || map_->getLength() < OXYGEN_IO_SIZE) {
        LOG("cannot map BAR0");
        OSSafeReleaseNULL(map_);
        return false;
    }
    lock_ = IOSimpleLockAlloc();
    if (!lock_) {
        OSSafeReleaseNULL(map_);
        return false;
    }
    pci_ = pci;
    pci_->retain();
    pci_->setIOEnable(true);
    pci_->setBusMasterEnable(true);
    LOG("I/O base 0x%llx", (unsigned long long)map_->getPhysicalAddress());
    return true;
}

void CMI8788Chip::detach()
{
    OSSafeReleaseNULL(map_);
    if (lock_) {
        IOSimpleLockFree(lock_);
        lock_ = nullptr;
    }
    OSSafeReleaseNULL(pci_);
}

/* ---- buses (oxygen_io.c) ------------------------------------------------- */

/* oxygen_write_i2c */
void CMI8788Chip::writeI2C(UInt8 device, UInt8 map, UInt8 data)
{
    /* should not need more than about 300 us */
    IOSleep(1);
    write8(OXYGEN_2WIRE_MAP, map);
    write8(OXYGEN_2WIRE_DATA, data);
    write8(OXYGEN_2WIRE_CONTROL, device | OXYGEN_2WIRE_DIR_WRITE);
}

/* oxygen_ac97_wait, polling instead of the AC'97 interrupt (Linux re-reads
 * the status after its timeout for the same reason). */
bool CMI8788Chip::waitAC97(UInt8 mask)
{
    UInt8 status = 0;
    for (int i = 0; i < 100; ++i) {
        /* reading the status register also clears the bits */
        status |= read8(OXYGEN_AC97_INTERRUPT_STATUS);
        if (status & mask)
            return true;
        IODelay(10);
    }
    return false;
}

/* oxygen_write_ac97: ~10% of AC'97 accesses fail, so retry and require two
 * completed writes. */
void CMI8788Chip::writeAC97(unsigned codec, unsigned index, UInt16 data)
{
    UInt32 reg = data | (index << OXYGEN_AC97_REG_ADDR_SHIFT) |
                 OXYGEN_AC97_REG_DIR_WRITE | (codec << OXYGEN_AC97_REG_CODEC_SHIFT);
    unsigned succeeded = 0;
    for (int count = 5; count > 0; --count) {
        IODelay(5);
        write32(OXYGEN_AC97_REGS, reg);
        if (waitAC97(OXYGEN_AC97_INT_WRITE_DONE) && ++succeeded >= 2) {
            savedAC97_[codec & 1][(index / 2) & 0x3f] = data;
            return;
        }
    }
    LOG("AC'97 write timeout (codec %u, reg 0x%02x)", codec, index);
}

/* oxygen_read_ac97: require two consecutive reads of the same value. */
UInt16 CMI8788Chip::readAC97(unsigned codec, unsigned index)
{
    UInt32 reg = (index << OXYGEN_AC97_REG_ADDR_SHIFT) |
                 OXYGEN_AC97_REG_DIR_READ | (codec << OXYGEN_AC97_REG_CODEC_SHIFT);
    UInt32 lastRead = 0xffffffff;
    for (int count = 5; count > 0; --count) {
        IODelay(5);
        write32(OXYGEN_AC97_REGS, reg);
        IODelay(10);
        if (waitAC97(OXYGEN_AC97_INT_READ_DONE)) {
            UInt16 value = read16(OXYGEN_AC97_REGS);
            if (value == lastRead)
                return value;
            lastRead = value;
            /* make two failed reads unable to return the same value */
            reg ^= 0xffff;
        }
    }
    LOG("AC'97 read timeout (codec %u, reg 0x%02x)", codec, index);
    return 0;
}

void CMI8788Chip::writeAC97Masked(unsigned codec, unsigned index, UInt16 data, UInt16 mask)
{
    UInt16 value = readAC97(codec, index);
    writeAC97(codec, index, (value & ~mask) | (data & mask));
}

/* oxygen_read_eeprom */
UInt16 CMI8788Chip::readEEPROM(unsigned index)
{
    write8(OXYGEN_EEPROM_CONTROL, (UInt8)(index | OXYGEN_EEPROM_DIR_READ));
    for (int timeout = 0; timeout < 100; ++timeout) {
        IODelay(1);
        if (!(read8(OXYGEN_EEPROM_STATUS) & OXYGEN_EEPROM_BUSY))
            break;
    }
    return read16(OXYGEN_EEPROM_DATA);
}

/* ---- bring-up ------------------------------------------------------------ */

/* oxygen_search_pci_id: the subsystem ID in config space can be wrong if the
 * EEPROM was overwritten, so read it from the EEPROM like Linux does. */
UInt16 CMI8788Chip::identify()
{
    /* make sure the EEPROM pins are not used for SPI */
    clearBits8(OXYGEN_FUNCTION, OXYGEN_FUNCTION_ENABLE_SPI_4_5);
    UInt16 subdevice = readEEPROM(2);
    UInt16 configSub = pci_->configRead16(kIOPCIConfigSubSystemID);
    LOG("subsystem ID: EEPROM 0x%04x, config space 0x%04x", subdevice, configSub);

    switch (subdevice) {
    case PCI_SUBDEVICE_XONAR_STX:
        modelName_ = "Xonar Essence STX";
        return subdevice;
    case PCI_SUBDEVICE_XONAR_STX_II:
        modelName_ = "Xonar Essence STX II";
        return subdevice;
    default:
        return 0;
    }
}

/* configure_pcie_bridge: the STX's CMI8788 is a PCI chip behind a PCIe bridge
 * whose defaults cause dropouts. */
void CMI8788Chip::configurePCIeBridge()
{
    IOService *parent = OSDynamicCast(IOService, pci_->getParentEntry(gIOServicePlane));
    IOPCIDevice *bridge = parent ? OSDynamicCast(IOPCIDevice, parent->getProvider()) : nullptr;
    if (!bridge)
        return;

    UInt16 vendor = bridge->configRead16(kIOPCIConfigVendorID);
    UInt16 device = bridge->configRead16(kIOPCIConfigDeviceID);
    UInt32 tmp;

    if (vendor == 0x10b5 && (device == 0x8111 || device == 0x8112)) {
        /* PLX PEX8111/PEX8112 */
        tmp = bridge->configRead32(0x48);
        tmp |= 1;           /* enable blind prefetching */
        tmp |= 1 << 11;     /* enable beacon generation */
        bridge->configWrite32(0x48, tmp);
        bridge->configWrite32(0x84, 0x0c);
        tmp = bridge->configRead32(0x88);
        tmp &= ~(7u << 27);
        tmp |= 2u << 27;    /* set prefetch size to 128 bytes */
        bridge->configWrite32(0x88, tmp);
    } else if (vendor == 0x12d8 && device == 0xe110) {
        /* Pericom PI7C9X110: park the PCI arbiter to the sound chip */
        bridge->configWrite32(0x40, bridge->configRead32(0x40) | 1);
    } else if (vendor == 0x104c && device == 0x8240) {
        /* TI XIO2001 */
        tmp = bridge->configRead32(0xe8);
        tmp &= ~0xfu;           /* request length limit: 64 bytes */
        tmp &= ~(0xfu << 8);
        tmp |= 1u << 8;         /* request count limit: one buffer */
        bridge->configWrite32(0xe8, tmp);
    } else {
        LOG("unknown PCIe bridge %04x:%04x, left as is", vendor, device);
        return;
    }
    LOG("configured PCIe bridge %04x:%04x", vendor, device);
}

/* oxygen_init, with model_xonar_st's flags and device_config
 * (PLAYBACK_0_TO_I2S | CAPTURE_0_FROM_I2S_2; S/PDIF left disabled). */
void CMI8788Chip::initChip()
{
    if (!(read8(OXYGEN_REVISION) & OXYGEN_REVISION_2))
        setBits8(OXYGEN_MISC, OXYGEN_MISC_PCI_MEM_W_1_CLOCK);

    UInt16 ac97 = read16(OXYGEN_AC97_CONTROL);
    hasAC97_0_ = (ac97 & OXYGEN_AC97_CODEC_0) != 0;
    hasAC97_1_ = (ac97 & OXYGEN_AC97_CODEC_1) != 0;

    write8Masked(OXYGEN_FUNCTION,
                 OXYGEN_FUNCTION_RESET_CODEC | OXYGEN_FUNCTION_2WIRE,
                 OXYGEN_FUNCTION_RESET_CODEC | OXYGEN_FUNCTION_2WIRE_SPI_MASK |
                 OXYGEN_FUNCTION_ENABLE_SPI_4_5);
    write8(OXYGEN_DMA_STATUS, 0);
    write8(OXYGEN_DMA_PAUSE, 0);
    write8(OXYGEN_PLAY_CHANNELS, OXYGEN_PLAY_CHANNELS_2 | OXYGEN_DMA_A_BURST_8 |
           OXYGEN_DMA_MULTICH_BURST_8);
    write16(OXYGEN_INTERRUPT_MASK, 0);
    write8Masked(OXYGEN_MISC, 0,
                 OXYGEN_MISC_WRITE_PCI_SUBID | OXYGEN_MISC_REC_C_FROM_SPDIF |
                 OXYGEN_MISC_REC_B_FROM_AC97 | OXYGEN_MISC_REC_A_FROM_MULTICH |
                 OXYGEN_MISC_MIDI);
    write8(OXYGEN_REC_FORMAT, (OXYGEN_FORMAT_16 << OXYGEN_REC_FORMAT_A_SHIFT) |
                              (OXYGEN_FORMAT_16 << OXYGEN_REC_FORMAT_B_SHIFT) |
                              (OXYGEN_FORMAT_16 << OXYGEN_REC_FORMAT_C_SHIFT));
    write8(OXYGEN_PLAY_FORMAT, (OXYGEN_FORMAT_16 << OXYGEN_SPDIF_FORMAT_SHIFT) |
                               (OXYGEN_FORMAT_16 << OXYGEN_MULTICH_FORMAT_SHIFT));
    write8(OXYGEN_REC_CHANNELS, OXYGEN_REC_CHANNELS_2_2_2);
    write16(OXYGEN_I2S_MULTICH_FORMAT, OXYGEN_RATE_48000 | kDACI2SFormat |
            OXYGEN_I2S_MCLK(kDACMclks) | OXYGEN_I2S_BITS_16 |
            OXYGEN_I2S_MASTER | OXYGEN_I2S_BCLK_64);
    write16(OXYGEN_I2S_A_FORMAT, OXYGEN_I2S_MASTER | OXYGEN_I2S_MUTE_MCLK);
    write16(OXYGEN_I2S_B_FORMAT, OXYGEN_RATE_48000 | kADCI2SFormat |
            OXYGEN_I2S_MCLK(kADCMclks) | OXYGEN_I2S_BITS_16 |
            OXYGEN_I2S_MASTER | OXYGEN_I2S_BCLK_64);
    write16(OXYGEN_I2S_C_FORMAT, OXYGEN_I2S_MASTER | OXYGEN_I2S_MUTE_MCLK);
    clearBits32(OXYGEN_SPDIF_CONTROL, OXYGEN_SPDIF_OUT_ENABLE | OXYGEN_SPDIF_LOOPBACK);
    clearBits32(OXYGEN_SPDIF_CONTROL, OXYGEN_SPDIF_SENSE_MASK | OXYGEN_SPDIF_LOCK_MASK |
                OXYGEN_SPDIF_RATE_MASK);
    write32(OXYGEN_SPDIF_OUTPUT_BITS, OXYGEN_SPDIF_C | OXYGEN_SPDIF_ORIGINAL |
            (0x02 /* IEC958_AES1_CON_PCM_CODER */ << OXYGEN_SPDIF_CATEGORY_SHIFT));
    write16(OXYGEN_2WIRE_BUS_STATUS, OXYGEN_2WIRE_LENGTH_8 |
            OXYGEN_2WIRE_INTERRUPT_MASK | OXYGEN_2WIRE_SPEED_STANDARD);
    clearBits8(OXYGEN_MPU401_CONTROL, OXYGEN_MPU401_LOOPBACK);
    write8(OXYGEN_GPI_INTERRUPT_MASK, 0);
    write16(OXYGEN_GPIO_INTERRUPT_MASK, 0);
    write16(OXYGEN_PLAY_ROUTING, OXYGEN_PLAY_MULTICH_I2S_DAC | OXYGEN_PLAY_SPDIF_SPDIF |
            (0 << OXYGEN_PLAY_DAC0_SOURCE_SHIFT) | (1 << OXYGEN_PLAY_DAC1_SOURCE_SHIFT) |
            (2 << OXYGEN_PLAY_DAC2_SOURCE_SHIFT) | (3 << OXYGEN_PLAY_DAC3_SOURCE_SHIFT));
    write8(OXYGEN_REC_ROUTING, OXYGEN_REC_A_ROUTE_I2S_ADC_1 | OXYGEN_REC_B_ROUTE_I2S_ADC_2 |
           OXYGEN_REC_C_ROUTE_SPDIF);
    write8(OXYGEN_ADC_MONITOR, 0);
    write8(OXYGEN_A_MONITOR_ROUTING,
           (0 << OXYGEN_A_MONITOR_ROUTE_0_SHIFT) | (1 << OXYGEN_A_MONITOR_ROUTE_1_SHIFT) |
           (2 << OXYGEN_A_MONITOR_ROUTE_2_SHIFT) | (3 << OXYGEN_A_MONITOR_ROUTE_3_SHIFT));

    if (hasAC97_0_ || hasAC97_1_)
        write8(OXYGEN_AC97_INTERRUPT_MASK, OXYGEN_AC97_INT_READ_DONE | OXYGEN_AC97_INT_WRITE_DONE);
    else
        write8(OXYGEN_AC97_INTERRUPT_MASK, 0);
    write32(OXYGEN_AC97_OUT_CONFIG, 0);
    write32(OXYGEN_AC97_IN_CONFIG, 0);
    if (!(hasAC97_0_ || hasAC97_1_))
        setBits16(OXYGEN_AC97_CONTROL, OXYGEN_AC97_CLOCK_DISABLE);
    if (!hasAC97_0_) {
        setBits16(OXYGEN_AC97_CONTROL, OXYGEN_AC97_NO_CODEC_0);
    } else {
        /* CM9780: on the STX it only matters for the mic path; GPO0 = 0
         * routes the line-in jack straight to the CS5381 ADC. */
        writeAC97(0, AC97_RESET, 0);
        IOSleep(1);
        writeAC97Masked(0, CM9780_GPIO_SETUP, CM9780_GPIO0IO | CM9780_GPIO1IO,
                        CM9780_GPIO0IO | CM9780_GPIO1IO);
        writeAC97Masked(0, CM9780_MIXER, CM9780_BSTSEL | CM9780_STRO_MIC |
                        CM9780_MIX2FR | CM9780_PCBSW,
                        CM9780_BSTSEL | CM9780_STRO_MIC | CM9780_MIX2FR | CM9780_PCBSW);
        writeAC97Masked(0, CM9780_JACK, CM9780_RSOE | CM9780_CBOE | CM9780_SSOE |
                        CM9780_FROE | CM9780_MIC2MIC | CM9780_LI2LI,
                        CM9780_RSOE | CM9780_CBOE | CM9780_SSOE | CM9780_FROE |
                        CM9780_MIC2MIC | CM9780_LI2LI);
        writeAC97(0, AC97_MASTER, 0x0000);
        writeAC97(0, AC97_PC_BEEP, 0x8000);
        writeAC97(0, AC97_MIC, 0x8808);
        writeAC97(0, AC97_LINE, 0x0808);
        writeAC97(0, AC97_CD, 0x8808);
        writeAC97(0, AC97_VIDEO, 0x8808);
        writeAC97(0, AC97_AUX, 0x8808);
        writeAC97(0, AC97_REC_GAIN, 0x8000);
        writeAC97(0, AC97_CENTER_LFE_MASTER, 0x8080);
        writeAC97(0, AC97_SURROUND_MASTER, 0x8080);
        writeAC97Masked(0, CM9780_GPIO_STATUS, 0, CM9780_GPO0);
        /* power down unused ADCs and DACs */
        writeAC97Masked(0, AC97_POWERDOWN, AC97_PD_PR0 | AC97_PD_PR1,
                        AC97_PD_PR0 | AC97_PD_PR1);
        writeAC97Masked(0, AC97_EXTENDED_STATUS, AC97_EA_PRI | AC97_EA_PRJ | AC97_EA_PRK,
                        AC97_EA_PRI | AC97_EA_PRJ | AC97_EA_PRK);
    }
    if (hasAC97_1_) {
        setBits32(OXYGEN_AC97_OUT_CONFIG, OXYGEN_AC97_CODEC1_SLOT3 | OXYGEN_AC97_CODEC1_SLOT4);
        writeAC97(1, AC97_RESET, 0);
        IOSleep(1);
        writeAC97(1, AC97_MASTER, 0x0000);
        writeAC97(1, AC97_HEADPHONE, 0x8000);
        writeAC97(1, AC97_PC_BEEP, 0x8000);
        writeAC97(1, AC97_MIC, 0x8808);
        writeAC97(1, AC97_LINE, 0x8808);
        writeAC97(1, AC97_CD, 0x8808);
        writeAC97(1, AC97_VIDEO, 0x8808);
        writeAC97(1, AC97_AUX, 0x8808);
        writeAC97(1, AC97_PCM, 0x0808);
        writeAC97(1, AC97_REC_SEL, 0x0000);
        writeAC97(1, AC97_REC_GAIN, 0x0000);
        writeAC97Masked(1, 0x6a, 0x0040, 0x0040);
    }
    LOG("chip initialized (revision 0x%04x, AC'97 codecs: %d%d)",
        read16(OXYGEN_REVISION), hasAC97_0_, hasAC97_1_);
}

/* xonar_stx_init + xonar_st_init_common + pcm1796_init (+ the STX II
 * daughterboard probe from get_xonar_pcm179x_model). */
void CMI8788Chip::initModel(UInt16 subdevice)
{
    if (subdevice == PCI_SUBDEVICE_XONAR_STX_II) {
        clearBits16(OXYGEN_GPIO_CONTROL, GPIO_DB_MASK);
        if ((read16(OXYGEN_GPIO_DATA) & GPIO_DB_MASK) == GPIO_DB_H6)
            LOG("H6 daughterboard detected; only the front channels are supported");
    }

    /* xonar_st_init_i2c */
    write16(OXYGEN_2WIRE_BUS_STATUS, OXYGEN_2WIRE_LENGTH_8 |
            OXYGEN_2WIRE_INTERRUPT_MASK | OXYGEN_2WIRE_SPEED_STANDARD);

    /* xonar_init_ext_power. OXYGEN_INT_GPIO itself is unmasked by the device
     * once the interrupt handler is registered (the line may be shared). */
    setBits8(OXYGEN_GPI_INTERRUPT_MASK, GPI_EXT_POWER);
    if (!hasExternalPower())
        LOG("WARNING: external power connector is not plugged in");

    /* pcm1796_init */
    dacVolume_[0] = dacVolume_[1] = kDACVolumeMin;
    dacMute_ = true;
    pcm1796Regs_[18 - PCM1796_REG_BASE] = PCM1796_FMT_24_I2S | PCM1796_ATLD | PCM1796_MUTE;
    pcm1796Regs_[19 - PCM1796_REG_BASE] = PCM1796_FLT_SHARP | PCM1796_ATS_1;
    pcm1796Regs_[20 - PCM1796_REG_BASE] = PCM1796_OS_128;
    pcm1796RegistersInit();
    currentRate_ = 48000;

    /* xonar_st_init_common: line out selected, line-in input */
    setBits16(OXYGEN_GPIO_CONTROL, GPIO_INPUT_ROUTE | GPIO_ST_HP_REAR | GPIO_ST_MAGIC | GPIO_ST_HP);
    clearBits16(OXYGEN_GPIO_DATA, GPIO_INPUT_ROUTE | GPIO_ST_HP_REAR | GPIO_ST_HP);
    hpActive_ = false;

    /* xonar_init_cs53x1 */
    setBits16(OXYGEN_GPIO_CONTROL, GPIO_CS53x1_M_MASK);
    write16Masked(OXYGEN_GPIO_DATA, GPIO_CS53x1_M_SINGLE, GPIO_CS53x1_M_MASK);

    prepareOutput();       /* the owner closes the relay after kAntiPopDelayMs */
    LOG("%s initialized", modelName_);
}

/* oxygen_shutdown + xonar_st_cleanup */
void CMI8788Chip::shutdown()
{
    if (!pci_)
        return;
    disableOutput();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    interruptMask_ = 0;
    write16(OXYGEN_DMA_STATUS, 0);
    write16(OXYGEN_INTERRUPT_MASK, 0);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
}

/* ---- sleep / wake (oxygen_lib.c) ---------------------------------------- */

/* Registers oxygen_pci_resume writes back from the shadow copy. */
static const UInt32 kRegistersToRestore[OXYGEN_IO_SIZE / 32] = {
    0xffffffff, 0x00ff077f, 0x00011d08, 0x007f00ff,
    0x00300000, 0x00000fe4, 0x0ff7001f, 0x00000000
};
static const UInt32 kAC97RegistersToRestore[2][0x40 / 32] = {
    { 0x18284fa2, 0x03060000 },
    { 0x00007fa6, 0x00200000 }
};

static inline bool isBitSet(const UInt32 *bitmap, unsigned bit)
{
    return (bitmap[bit / 32] >> (bit % 32)) & 1;
}

/* oxygen_restore_ac97 */
void CMI8788Chip::restoreAC97(unsigned codec)
{
    writeAC97(codec, AC97_RESET, 0);
    IOSleep(1);
    for (unsigned i = 1; i < 0x40; ++i)
        if (isBitSet(kAC97RegistersToRestore[codec], i))
            writeAC97(codec, i * 2, savedAC97_[codec][i]);
}

/* oxygen_pci_suspend + xonar_st_suspend. The engine has already been paused
 * by IOAudioFamily; interruptMask_ is kept so resume() can restore it. */
void CMI8788Chip::suspend()
{
    disableOutput();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    write16(OXYGEN_DMA_STATUS, 0);
    write16(OXYGEN_INTERRUPT_MASK, 0);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
    LOG("suspended");
}

/* oxygen_pci_resume + xonar_stx_resume. IOAudioFamily restarts the engine
 * afterwards through performAudioEngineStart. */
void CMI8788Chip::resume()
{
    write16(OXYGEN_DMA_STATUS, 0);
    write16(OXYGEN_INTERRUPT_MASK, 0);
    /* the bridge's vendor registers are outside what IOPCIFamily restores */
    configurePCIeBridge();
    for (unsigned i = 0; i < OXYGEN_IO_SIZE; ++i)
        if (isBitSet(kRegistersToRestore, i))
            write8((UInt8)i, saved_[i]);
    if (hasAC97_0_)
        restoreAC97(0);
    if (hasAC97_1_)
        restoreAC97(1);

    pcm1796RegistersInit();
    prepareOutput();       /* the owner closes the relay after kAntiPopDelayMs */

    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    write16(OXYGEN_INTERRUPT_MASK, interruptMask_);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
    if (!hasExternalPower())
        LOG("WARNING: external power connector is not plugged in");
    LOG("resumed");
}

/* xonar_enable_output, first half: make the relay pin an output (still open).
 * The anti-pop delay (xonar_stx_init: 800 ms) runs before finishEnableOutput. */
void CMI8788Chip::prepareOutput()
{
    setBits16(OXYGEN_GPIO_CONTROL, GPIO_ST_OUTPUT_ENABLE);
}

/* xonar_enable_output, second half: close the output relay. */
void CMI8788Chip::finishEnableOutput()
{
    setBits16(OXYGEN_GPIO_DATA, GPIO_ST_OUTPUT_ENABLE);
}

void CMI8788Chip::disableOutput()
{
    clearBits16(OXYGEN_GPIO_DATA, GPIO_ST_OUTPUT_ENABLE);
}

bool CMI8788Chip::hasExternalPower()
{
    return (read8(OXYGEN_GPI_DATA) & GPI_EXT_POWER) != 0;
}

/* ---- PCM1792A (xonar_pcm179x.c) ----------------------------------------- */

/* pcm1796_write (the STX uses I2C, one DAC) */
void CMI8788Chip::pcm1796Write(UInt8 reg, UInt8 value)
{
    writeI2C(I2C_DEVICE_PCM1796(0), reg, value);
    if ((unsigned)(reg - PCM1796_REG_BASE) < sizeof(pcm1796Regs_))
        pcm1796Regs_[reg - PCM1796_REG_BASE] = value;
}

void CMI8788Chip::pcm1796WriteCached(UInt8 reg, UInt8 value)
{
    if (value != pcm1796Regs_[reg - PCM1796_REG_BASE])
        pcm1796Write(reg, value);
}

/* pcm1796_registers_init */
void CMI8788Chip::pcm1796RegistersInit()
{
    SInt8 offset = hpActive_ ? hpGainOffset_ : 0;
    IOSleep(1);
    /* set ATLD before ATL/ATR */
    pcm1796Write(18, pcm1796Regs_[18 - PCM1796_REG_BASE]);
    pcm1796Write(16, (UInt8)(dacVolume_[0] + offset));
    pcm1796Write(17, (UInt8)(dacVolume_[1] + offset));
    pcm1796Write(19, pcm1796Regs_[19 - PCM1796_REG_BASE]);
    pcm1796Write(20, pcm1796Regs_[20 - PCM1796_REG_BASE]);
    pcm1796Write(21, 0);
}

/* update_pcm1796_volume */
void CMI8788Chip::updateDACVolume()
{
    SInt8 offset = hpActive_ ? hpGainOffset_ : 0;
    pcm1796WriteCached(16, (UInt8)(dacVolume_[0] + offset));
    pcm1796WriteCached(17, (UInt8)(dacVolume_[1] + offset));
}

void CMI8788Chip::setVolume(UInt8 left, UInt8 right)
{
    if (left > kVolumeSteps)  left = kVolumeSteps;
    if (right > kVolumeSteps) right = kVolumeSteps;
    dacVolume_[0] = kDACVolumeMin + left;
    dacVolume_[1] = kDACVolumeMin + right;
    updateDACVolume();
}

/* update_pcm1796_mute */
void CMI8788Chip::setMute(bool mute)
{
    UInt8 value = pcm1796Regs_[18 - PCM1796_REG_BASE];
    dacMute_ = mute;
    if (mute)
        value |= PCM1796_MUTE;
    else
        value &= ~PCM1796_MUTE;
    pcm1796WriteCached(18, value);
}

/* st_output_switch_put */
void CMI8788Chip::setOutput(Output output)
{
    UInt16 gpio = read16(OXYGEN_GPIO_DATA);
    switch (output) {
    case kOutputSpeakers:
        gpio &= ~(GPIO_ST_HP | GPIO_ST_HP_REAR);
        break;
    case kOutputHeadphones:
        gpio |= GPIO_ST_HP | GPIO_ST_HP_REAR;
        break;
    case kOutputFrontPanel:
        gpio = (gpio | GPIO_ST_HP) & ~GPIO_ST_HP_REAR;
        break;
    }
    write16(OXYGEN_GPIO_DATA, gpio);
    hpActive_ = (gpio & GPIO_ST_HP) != 0;
    updateDACVolume();
}

/* st_hp_volume_offset_put: 0 / -6 / -12 / -18 dB in half-dB units */
void CMI8788Chip::setHeadphoneGainOffset(SInt8 halfDecibels)
{
    hpGainOffset_ = halfDecibels;
    updateDACVolume();
}

/* ---- capture source (oxygen_mixer.c ac97_switch_put, mic_fmic_source_put,
 *      xonar_line_mic_ac97_switch) -------------------------------------- */

void CMI8788Chip::setInput(Input input)
{
    if (!hasAC97_0_) {
        LOG("no CM9780 codec, input stays on line-in");
        return;
    }
    if (input == kInputLine) {
        /* mic muted, line unmuted: jack -> line-in -> CS5381 directly */
        writeAC97Masked(0, AC97_MIC, 0x8000, 0x8000);
        writeAC97Masked(0, AC97_LINE, 0, 0x8000);
        clearBits16(OXYGEN_GPIO_DATA, GPIO_INPUT_ROUTE);
        writeAC97Masked(0, CM9780_GPIO_STATUS, 0, CM9780_GPO0);
    } else {
        /* line muted: jack -> mic-in; CM9780 output -> CS5381 */
        writeAC97Masked(0, AC97_LINE, 0x8000, 0x8000);
        setBits16(OXYGEN_GPIO_DATA, GPIO_INPUT_ROUTE);
        writeAC97Masked(0, CM9780_JACK, input == kInputFrontMic ? CM9780_FMIC2MIC : 0,
                        CM9780_FMIC2MIC);
        /* unmute, +20 dB boost (bit 6) */
        writeAC97Masked(0, AC97_MIC, 0x0040, 0x8040);
        writeAC97Masked(0, CM9780_GPIO_STATUS, CM9780_GPO0, CM9780_GPO0);
    }
}

/* ac97_volume_put on AC97_MIC: register value 0 = +12 dB, 0x1f = -34.5 dB */
void CMI8788Chip::setMicGain(UInt8 steps)
{
    if (!hasAC97_0_)
        return;
    if (steps > kMicGainSteps)
        steps = kMicGainSteps;
    writeAC97Masked(0, AC97_MIC, (UInt16)(kMicGainSteps - steps), 0x001f);
}

/* monitor_put with OXYGEN_ADC_MONITOR_B (the STX records from I2S ADC 2): the
 * ADC is mixed into the DAC path in hardware, at 0 dB or (HALF_VOL) -6 dB. */
void CMI8788Chip::setInputMonitor(Monitor monitor)
{
    UInt8 bits = 0;
    if (monitor == kMonitorHalf)
        bits = OXYGEN_ADC_MONITOR_B | OXYGEN_ADC_MONITOR_B_HALF_VOL;
    else if (monitor == kMonitorFull)
        bits = OXYGEN_ADC_MONITOR_B;
    write8Masked(OXYGEN_ADC_MONITOR, bits, OXYGEN_ADC_MONITOR_B | OXYGEN_ADC_MONITOR_B_HALF_VOL);
}

/* ---- S/PDIF output (oxygen_mixer.c) ------------------------------------- */

/* Consumer channel status: copyright not asserted, original, PCM coder
 * category (oxygen_init's spdif_bits). */
static const UInt32 kSPDIFBits = OXYGEN_SPDIF_C | OXYGEN_SPDIF_ORIGINAL |
                                 (0x02 /* IEC958_AES1_CON_PCM_CODER */ << OXYGEN_SPDIF_CATEGORY_SHIFT);

/* oxygen_spdif_rate: IEC 60958-3 sample-frequency code (channel status
 * byte 3) for an OXYGEN_RATE_* value. */
static UInt32 spdifRateBits(UInt16 oxygenRate)
{
    UInt32 code;
    switch (oxygenRate) {
    case OXYGEN_RATE_32000:  code = 0x3; break;
    case OXYGEN_RATE_44100:  code = 0x0; break;
    case OXYGEN_RATE_64000:  code = 0xb; break;
    case OXYGEN_RATE_88200:  code = 0x8; break;
    case OXYGEN_RATE_96000:  code = 0xa; break;
    case OXYGEN_RATE_176400: code = 0xc; break;
    case OXYGEN_RATE_192000: code = 0xe; break;
    default:                 code = 0x2; break;   /* 48000 */
    }
    return code << OXYGEN_SPDIF_CS_RATE_SHIFT;
}

/* oxygen_update_spdif_source, mirror mode only (no separate S/PDIF PCM):
 * with the switch on, S/PDIF carries DAC channels 0/1 at the playback rate. */
void CMI8788Chip::updateSPDIFSource()
{
    UInt32 oldControl = read32(OXYGEN_SPDIF_CONTROL), newControl;
    UInt16 oldRouting = read16(OXYGEN_PLAY_ROUTING), newRouting;
    UInt16 rate = OXYGEN_RATE_44100;

    if (spdifOut_) {
        newRouting = (UInt16)((oldRouting & ~OXYGEN_PLAY_SPDIF_MASK) | OXYGEN_PLAY_SPDIF_MULTICH_01);
        rate = read16(OXYGEN_I2S_MULTICH_FORMAT) & OXYGEN_I2S_RATE_MASK;
        newControl = (oldControl & ~(UInt32)OXYGEN_SPDIF_OUT_RATE_MASK) |
                     ((UInt32)rate << OXYGEN_SPDIF_OUT_RATE_SHIFT) | OXYGEN_SPDIF_OUT_ENABLE;
    } else {
        newControl = oldControl & ~(UInt32)OXYGEN_SPDIF_OUT_ENABLE;
        newRouting = oldRouting;
    }
    if (oldRouting != newRouting) {
        write32(OXYGEN_SPDIF_CONTROL, newControl & ~(UInt32)OXYGEN_SPDIF_OUT_ENABLE);
        write16(OXYGEN_PLAY_ROUTING, newRouting);
    }
    if (newControl & OXYGEN_SPDIF_OUT_ENABLE)
        write32(OXYGEN_SPDIF_OUTPUT_BITS, spdifRateBits(rate) | kSPDIFBits);
    write32(OXYGEN_SPDIF_CONTROL, newControl);
}

/* spdif_switch_put */
void CMI8788Chip::setSPDIFOutput(bool on)
{
    spdifOut_ = on;
    updateSPDIFSource();
}

/* rolloff_put */
void CMI8788Chip::setDACFilterSlow(bool slow)
{
    UInt8 reg = pcm1796Regs_[19 - PCM1796_REG_BASE] & ~PCM1796_FLT_MASK;
    pcm1796WriteCached(19, reg | (slow ? PCM1796_FLT_SLOW : PCM1796_FLT_SHARP));
}

/* deemph_put; the de-emphasis frequency follows the sample rate (DMF) */
void CMI8788Chip::setDeemphasis(bool on)
{
    UInt8 reg = pcm1796Regs_[18 - PCM1796_REG_BASE];
    pcm1796WriteCached(18, on ? (reg | PCM1796_DME) : (reg & ~PCM1796_DME));
}

/* ---- sample rate / format (oxygen_pcm.c) --------------------------------- */

static UInt16 oxygenRate(UInt32 rate)
{
    switch (rate) {
    case 32000:  return OXYGEN_RATE_32000;
    case 44100:  return OXYGEN_RATE_44100;
    case 64000:  return OXYGEN_RATE_64000;
    case 88200:  return OXYGEN_RATE_88200;
    case 96000:  return OXYGEN_RATE_96000;
    case 176400: return OXYGEN_RATE_176400;
    case 192000: return OXYGEN_RATE_192000;
    default:     return OXYGEN_RATE_48000;
    }
}

/* get_mclk */
static UInt16 mclkForRate(UInt8 mclks, UInt32 rate)
{
    unsigned shift = rate <= 48000 ? 0 : rate <= 96000 ? 2 : 4;
    return (UInt16)OXYGEN_I2S_MCLK(mclks >> shift);
}

/* oxygen_multich_hw_params (2 channels, S32_LE -> 24-bit) + set_pcm1796_params
 * + oxygen_update_dac_routing (stereo -> front+surround, the Linux default) */
void CMI8788Chip::setPlaybackRate(UInt32 rate)
{
    write8Masked(OXYGEN_PLAY_CHANNELS, OXYGEN_PLAY_CHANNELS_2, OXYGEN_PLAY_CHANNELS_MASK);
    write8Masked(OXYGEN_PLAY_FORMAT, OXYGEN_FORMAT_24 << OXYGEN_MULTICH_FORMAT_SHIFT,
                 OXYGEN_MULTICH_FORMAT_MASK);
    write16Masked(OXYGEN_I2S_MULTICH_FORMAT,
                  oxygenRate(rate) | kDACI2SFormat | mclkForRate(kDACMclks, rate) |
                  OXYGEN_I2S_BITS_24,
                  OXYGEN_I2S_RATE_MASK | OXYGEN_I2S_FORMAT_MASK |
                  OXYGEN_I2S_MCLK_MASK | OXYGEN_I2S_BITS_MASK);

    /* set_pcm1796_params: oversampling + de-emphasis frequency */
    IOSleep(1);
    currentRate_ = rate;
    pcm1796WriteCached(20, rate <= 48000 ? PCM1796_OS_128 : PCM1796_OS_64);
    UInt8 reg18 = pcm1796Regs_[18 - PCM1796_REG_BASE] & ~PCM1796_DMF_MASK;
    if (rate == 48000)
        reg18 |= PCM1796_DMF_48;
    else if (rate == 44100)
        reg18 |= PCM1796_DMF_441;
    else if (rate == 32000)
        reg18 |= PCM1796_DMF_32;
    pcm1796WriteCached(18, reg18);

    write16Masked(OXYGEN_PLAY_ROUTING,
                  (0 << OXYGEN_PLAY_DAC0_SOURCE_SHIFT) | (0 << OXYGEN_PLAY_DAC1_SOURCE_SHIFT) |
                  (2 << OXYGEN_PLAY_DAC2_SOURCE_SHIFT) | (3 << OXYGEN_PLAY_DAC3_SOURCE_SHIFT),
                  OXYGEN_PLAY_DAC0_SOURCE_MASK | OXYGEN_PLAY_DAC1_SOURCE_MASK |
                  OXYGEN_PLAY_DAC2_SOURCE_MASK | OXYGEN_PLAY_DAC3_SOURCE_MASK);

    /* oxygen_multich_hw_params -> oxygen_update_spdif_source: follow the rate */
    updateSPDIFSource();
}

/* oxygen_rec_b_hw_params + xonar_set_cs53x1_params */
void CMI8788Chip::setCaptureRate(UInt32 rate)
{
    write8Masked(OXYGEN_REC_FORMAT, OXYGEN_FORMAT_24 << OXYGEN_REC_FORMAT_B_SHIFT,
                 OXYGEN_REC_FORMAT_B_MASK);
    write16Masked(OXYGEN_I2S_B_FORMAT,
                  oxygenRate(rate) | kADCI2SFormat | mclkForRate(kADCMclks, rate) |
                  OXYGEN_I2S_BITS_24,
                  OXYGEN_I2S_RATE_MASK | OXYGEN_I2S_FORMAT_MASK |
                  OXYGEN_I2S_MCLK_MASK | OXYGEN_I2S_BITS_MASK);

    UInt16 mode = rate <= 54000 ? GPIO_CS53x1_M_SINGLE :
                  rate <= 108000 ? GPIO_CS53x1_M_DOUBLE : GPIO_CS53x1_M_QUAD;
    write16Masked(OXYGEN_GPIO_DATA, mode, GPIO_CS53x1_M_MASK);
}

/* ---- DMA (oxygen_hw_params / prepare / trigger / pointer) --------------- */

static UInt8 dmaBaseRegister(UInt8 channel)
{
    switch (channel) {
    case OXYGEN_CHANNEL_A:       return OXYGEN_DMA_A_ADDRESS;
    case OXYGEN_CHANNEL_B:       return OXYGEN_DMA_B_ADDRESS;
    case OXYGEN_CHANNEL_C:       return OXYGEN_DMA_C_ADDRESS;
    case OXYGEN_CHANNEL_SPDIF:   return OXYGEN_DMA_SPDIF_ADDRESS;
    case OXYGEN_CHANNEL_MULTICH: return OXYGEN_DMA_MULTICH_ADDRESS;
    default:                     return OXYGEN_DMA_AC97_ADDRESS;
    }
}

void CMI8788Chip::setDMABuffer(UInt8 channel, UInt32 busAddress, UInt32 bytes, UInt32 periodBytes)
{
    UInt8 base = dmaBaseRegister(channel);
    write32(base, busAddress);
    if (channel == OXYGEN_CHANNEL_MULTICH) {
        write32(OXYGEN_DMA_MULTICH_COUNT, bytes / 4 - 1);
        write32(OXYGEN_DMA_MULTICH_TCOUNT, periodBytes / 4 - 1);
    } else {
        write16((UInt8)(base + 4), (UInt16)(bytes / 4 - 1));
        write16((UInt8)(base + 6), (UInt16)(periodBytes / 4 - 1));
    }
}

/* oxygen_pointer: the address registers read back the current position */
UInt32 CMI8788Chip::dmaPosition(UInt8 channel)
{
    return read32(dmaBaseRegister(channel));
}

void CMI8788Chip::flushDMA(UInt8 channelMask)
{
    setBits8(OXYGEN_DMA_FLUSH, channelMask);
    clearBits8(OXYGEN_DMA_FLUSH, channelMask);
}

void CMI8788Chip::startDMA(UInt8 channelMask)
{
    setBits8(OXYGEN_DMA_STATUS, channelMask);
}

void CMI8788Chip::stopDMA(UInt8 channelMask)
{
    clearBits8(OXYGEN_DMA_STATUS, channelMask);
}

/* ---- interrupts (oxygen_interrupt) -------------------------------------- */

void CMI8788Chip::ackInterrupts(UInt16 status)
{
    UInt16 clear = status & (OXYGEN_CHANNEL_A | OXYGEN_CHANNEL_B | OXYGEN_CHANNEL_C |
                             OXYGEN_CHANNEL_SPDIF | OXYGEN_CHANNEL_MULTICH |
                             OXYGEN_CHANNEL_AC97 | OXYGEN_INT_SPDIF_IN_DETECT |
                             OXYGEN_INT_GPIO | OXYGEN_INT_AC97);
    if (!clear)
        return;
    /* toggling the mask bits acknowledges the interrupt */
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    write16(OXYGEN_INTERRUPT_MASK, interruptMask_ & ~clear);
    write16(OXYGEN_INTERRUPT_MASK, interruptMask_);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
}

void CMI8788Chip::enableInterrupts(UInt16 mask)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    interruptMask_ |= mask;
    write16(OXYGEN_INTERRUPT_MASK, interruptMask_);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
}

void CMI8788Chip::disableInterrupts(UInt16 mask)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    interruptMask_ &= ~mask;
    write16(OXYGEN_INTERRUPT_MASK, interruptMask_);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
}
