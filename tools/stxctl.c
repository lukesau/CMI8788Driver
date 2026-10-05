/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * stxctl: runtime settings for CMI8788Driver (Xonar Essence STX / STX II).
 *
 *   stxctl status
 *   sudo stxctl impedance <ohms>    headphone gain offset, like the Linux
 *                                   "Headphones Impedance" / Windows "HP Amp
 *                                   Gain": <32 -18 dB, <64 -12 dB, <300 -6 dB,
 *                                   300+ 0 dB. Not persistent: set the
 *                                   HeadphoneImpedance key in the kext's
 *                                   Info.plist for that.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static io_service_t findDevice(void)
{
    return IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("CMI8788AudioDevice"));
}

static int usage(void)
{
    fprintf(stderr, "usage: stxctl status\n       sudo stxctl impedance <ohms>\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    io_service_t dev = findDevice();
    if (!dev) {
        fprintf(stderr, "stxctl: no CMI8788AudioDevice (driver not loaded or card not found)\n");
        return 1;
    }

    int rc = 0;
    if (!strcmp(argv[1], "status")) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(dev, CFSTR("HeadphoneImpedance"), kCFAllocatorDefault, 0);
        int ohms = 0;
        if (v && CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &ohms);
        if (ohms)
            printf("headphone impedance: %d ohms\n", ohms);
        else
            printf("headphone impedance: not set (-18 dB default)\n");
        if (v)
            CFRelease(v);
    } else if (!strcmp(argv[1], "impedance") && argc == 3) {
        char *end;
        long ohms = strtol(argv[2], &end, 10);
        if (*end || ohms <= 0 || ohms > 100000)
            return usage();
        int value = (int)ohms;
        CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
        kern_return_t kr = IORegistryEntrySetCFProperty(dev, CFSTR("HeadphoneImpedance"), num);
        CFRelease(num);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "stxctl: failed (0x%x)%s\n", kr, kr == kIOReturnNotPrivileged ? ": run with sudo" : "");
            rc = 1;
        } else {
            printf("headphone impedance set to %ld ohms\n", ohms);
        }
    } else {
        rc = usage();
    }
    IOObjectRelease(dev);
    return rc;
}
