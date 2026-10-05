/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * stxctl: card settings for CMI8788Driver (Xonar Essence STX / STX II) that
 * Sound preferences has no UI for. Changes apply immediately and last until the
 * driver reloads; set the same keys in the kext's Info.plist (or use the STX
 * menu bar app) to keep them.
 *
 *   stxctl status
 *   stxctl impedance <ohms>          headphone gain offset ("HP Amp Gain"):
 *                                    <32 -18 dB, <64 -12 dB, <300 -6 dB, else 0 dB
 *   stxctl input line|mic|frontmic   input selection (as in Sound preferences)
 *   stxctl monitor off|half|full     hardware input monitoring (half = -6 dB)
 *   stxctl filter sharp|slow         DAC digital filter roll-off
 *   stxctl deemphasis on|off
 *   stxctl spdif on|off              S/PDIF output mirrors the analog stereo
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
            "usage: stxctl status\n"
            "       stxctl impedance <ohms>\n"
            "       stxctl input line|mic|frontmic\n"
            "       stxctl monitor off|half|full\n"
            "       stxctl filter sharp|slow\n"
            "       stxctl deemphasis on|off\n"
            "       stxctl spdif on|off\n");
    return 2;
}

static void printProperty(io_service_t dev, CFStringRef key, const char *label)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0);
    char buf[64] = "not set";
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        int n = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
        snprintf(buf, sizeof buf, "%d", n);
    } else if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
    } else if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) {
        snprintf(buf, sizeof buf, "%s", CFBooleanGetValue((CFBooleanRef)v) ? "on" : "off");
    }
    printf("%-20s %s\n", label, buf);
    if (v)
        CFRelease(v);
}

static int set(io_service_t dev, CFStringRef key, CFTypeRef value)
{
    kern_return_t kr = IORegistryEntrySetCFProperty(dev, key, value);
    CFRelease(value);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "stxctl: failed (0x%x)\n", kr);
        return 1;
    }
    return 0;
}

static CFStringRef str(const char *s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    io_service_t dev = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching("CMI8788AudioDevice"));
    if (!dev) {
        fprintf(stderr, "stxctl: no CMI8788AudioDevice (driver not loaded or card not found)\n");
        return 1;
    }

    int rc;
    const char *cmd = argv[1], *arg = argc == 3 ? argv[2] : NULL;
    if (!strcmp(cmd, "status") && argc == 2) {
        printProperty(dev, CFSTR("HeadphoneImpedance"), "headphone impedance");
        printProperty(dev, CFSTR("InputSourceName"), "input");
        printProperty(dev, CFSTR("InputMonitor"), "input monitor");
        printProperty(dev, CFSTR("InputMonitorLevel"), "monitor level");
        printProperty(dev, CFSTR("DACFilter"), "DAC filter");
        printProperty(dev, CFSTR("Deemphasis"), "de-emphasis");
        printProperty(dev, CFSTR("SPDIFOutput"), "S/PDIF output");
        rc = 0;
    } else if (!strcmp(cmd, "impedance") && arg) {
        char *end;
        long ohms = strtol(arg, &end, 10);
        if (*end || ohms <= 0 || ohms > 100000)
            rc = usage();
        else {
            int v = (int)ohms;
            rc = set(dev, CFSTR("HeadphoneImpedance"),
                     CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v));
        }
    } else if (!strcmp(cmd, "input") && arg &&
               (!strcmp(arg, "line") || !strcmp(arg, "mic") || !strcmp(arg, "frontmic"))) {
        rc = set(dev, CFSTR("InputSource"), str(arg));
    } else if (!strcmp(cmd, "monitor") && arg &&
               (!strcmp(arg, "off") || !strcmp(arg, "half") || !strcmp(arg, "full"))) {
        rc = set(dev, CFSTR("InputMonitor"), str(arg));
    } else if (!strcmp(cmd, "filter") && arg && (!strcmp(arg, "sharp") || !strcmp(arg, "slow"))) {
        rc = set(dev, CFSTR("DACFilter"), str(arg));
    } else if (!strcmp(cmd, "deemphasis") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        rc = set(dev, CFSTR("Deemphasis"),
                 CFRetain(!strcmp(arg, "on") ? kCFBooleanTrue : kCFBooleanFalse));
    } else if (!strcmp(cmd, "spdif") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        rc = set(dev, CFSTR("SPDIFOutput"),
                 CFRetain(!strcmp(arg, "on") ? kCFBooleanTrue : kCFBooleanFalse));
    } else {
        rc = usage();
    }
    if (rc == 0 && strcmp(cmd, "status"))
        printf("ok\n");
    IOObjectRelease(dev);
    return rc;
}
