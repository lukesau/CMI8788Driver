// SPDX-License-Identifier: GPL-2.0-only
//
// STX: menu bar app for the CMI8788Driver card settings that Sound preferences
// has no UI for (headphone impedance, input monitoring, DAC filter,
// de-emphasis). Talks to the driver through its I/O registry properties, saves
// the choices, and reapplies them whenever the card (re)appears: at login,
// after wake, after a driver reload.
//
// Objective-C with manual retain/release so one binary runs on OS X 10.9 and
// later (Swift can't target 10.9, and ARC for 10.9 needs libarclite, which
// current toolchains no longer ship). Objects made at launch live until exit.

#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>

static NSString *const kDriverClass = @"CMI8788AudioDevice";
static NSString *const kKextSupportDir = @"/Library/Application Support/CMI8788Driver";

/* The kext copy for OpenCore: the 10.9 build before 10.15, else the 10.15+ one. */
static NSString *kextSupportPath(void)
{
    BOOL preCatalina = NSAppKitVersionNumber < 1894;  /* NSAppKitVersionNumber10_15 */
    return [kKextSupportDir stringByAppendingPathComponent:
            preCatalina ? @"CMI8788Driver-10.9.kext" : @"CMI8788Driver.kext"];
}

// The driver's settings (I/O registry property keys), also the saved defaults keys.
static NSString *const kImpedance = @"HeadphoneImpedance";   // NSNumber, ohms
static NSString *const kOutput = @"OutputDestination";       // "headphones" | "line" | "frontpanel"
static NSString *const kInput = @"InputSource";              // "line" | "mic" | "frontmic"
static NSString *const kMonitorLevel = @"InputMonitorLevel"; // "half" | "full": level used when on
static NSString *const kMonitor = @"InputMonitor";           // "off" | "half" | "full"
static NSString *const kFilter = @"DACFilter";               // "sharp" | "slow"
static NSString *const kDeemphasis = @"Deemphasis";          // Bool
static NSString *const kSPDIF = @"SPDIFOutput";              // Bool: S/PDIF mirrors the analog output

static NSArray *allSettings(void)
{
    return @[kImpedance, kOutput, kInput, kMonitorLevel, kMonitor, kFilter, kDeemphasis, kSPDIF];
}

// One menu choice: { title, value sent to the driver }.
#define CHOICE(t, v) @[t, v]

static NSArray *impedanceChoices(void)
{
    // Representative impedances for the driver's four gain-offset bands.
    return @[CHOICE(@"Under 32 Ω (−18 dB)", @16), CHOICE(@"32–64 Ω (−12 dB)", @32),
             CHOICE(@"64–300 Ω (−6 dB)", @64), CHOICE(@"300–600 Ω (0 dB)", @300)];
}

static NSArray *outputChoices(void)
{
    return @[CHOICE(@"Headphones", @"headphones"), CHOICE(@"Line Out", @"line"),
             CHOICE(@"Front Panel Headphones", @"frontpanel")];
}

static NSArray *inputChoices(void)
{
    return @[CHOICE(@"Line In", @"line"), CHOICE(@"Microphone", @"mic"),
             CHOICE(@"Front Panel Microphone", @"frontmic")];
}

static NSArray *monitorLevelChoices(void)
{
    return @[CHOICE(@"−6 dB", @"half"), CHOICE(@"0 dB", @"full")];
}

static NSArray *filterChoices(void)
{
    return @[CHOICE(@"Sharp Roll-off", @"sharp"), CHOICE(@"Slow Roll-off", @"slow")];
}

static int impedanceBand(int ohms)
{
    if (ohms < 32)
        return 16;
    if (ohms < 64)
        return 32;
    if (ohms < 300)
        return 64;
    return 300;
}

#pragma mark - driver

/* The driver's registry entry, or 0 if the card isn't present. */
static io_service_t driverService(void)
{
    return IOServiceGetMatchingService(kIOMasterPortDefault,
                                       IOServiceMatching([kDriverClass UTF8String]));
}

static id driverProperty(NSString *key)
{
    io_service_t dev = driverService();
    if (!dev)
        return nil;
    CFTypeRef value = IORegistryEntryCreateCFProperty(dev, (CFStringRef)key, kCFAllocatorDefault, 0);
    IOObjectRelease(dev);
    return [(id)value autorelease];
}

static NSString *driverString(NSString *key, NSString *fallback)
{
    id value = driverProperty(key);
    return [value isKindOfClass:[NSString class]] ? value : fallback;
}

static BOOL driverWrite(NSDictionary *settings)
{
    io_service_t dev = driverService();
    if (!dev || settings.count == 0) {
        if (dev)
            IOObjectRelease(dev);
        return NO;
    }
    kern_return_t kr = IORegistryEntrySetCFProperties(dev, (CFDictionaryRef)settings);
    IOObjectRelease(dev);
    return kr == KERN_SUCCESS;
}

#pragma mark - app

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate> {
    NSStatusItem *statusItem_;
    NSMenu *menu_;
    IONotificationPortRef notifyPort_;
    io_iterator_t matchIterator_;
    /* When the saved settings were last pushed to the driver. Changes made
     * elsewhere are only adopted once the device has settled after that, so
     * a freshly loaded driver's defaults never overwrite your choices. */
    NSDate *lastApplied_;
}
- (void)applySaved;
@end

static void cardMatched(void *refcon, io_iterator_t iterator)
{
    BOOL found = NO;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        found = YES;
        IOObjectRelease(service);
    }
    if (found && refcon)
        [(AppDelegate *)refcon applySaved];
}

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    // One menu bar item only: the LaunchAgent and macOS's "reopen apps at
    // login" can both start us.
    pid_t me = [[NSProcessInfo processInfo] processIdentifier];
    NSString *bundleID = [[NSBundle mainBundle] bundleIdentifier] ?: @"com.lukesau.stx";
    for (NSRunningApplication *other in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleID]) {
        if (other.processIdentifier != me) {
            [NSApp terminate:nil];
            return;
        }
    }
    lastApplied_ = [[NSDate distantPast] retain];
    statusItem_ = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
    menu_ = [[NSMenu alloc] init];
    menu_.delegate = self;
    if (![statusItem_ respondsToSelector:@selector(button)])
        [statusItem_ setHighlightMode:YES];     // 10.9: no button, highlight by hand
    statusItem_.menu = menu_;
    [self watchForCard];
    [self applySaved];
    [self updateTitle];
    // Monitoring can also be switched by stxctl or another app (CoreAudio
    // play-through), so keep the indicator current.
    [NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(tick:)
                                   userInfo:nil repeats:YES];
}

- (void)tick:(NSTimer *)timer
{
    [self adoptOutsideChanges];
    [self updateTitle];
}

/* "STX ●" while input monitoring is on, so it's never on unnoticed. */
- (void)updateTitle
{
    NSString *monitor = driverString(kMonitor, @"off");
    NSString *title = [monitor isEqualToString:@"off"] ? @"STX" : @"STX ●";
    if ([statusItem_ respondsToSelector:@selector(button)])
        [[statusItem_ performSelector:@selector(button)] setTitle:title];  // 10.10+, not in the 10.9 SDK
    else
        [statusItem_ setTitle:title];
}

#pragma mark persistence

/* Save settings changed outside the app (Sound preferences, stxctl, other
 * apps' play-through) as the new choices, once the device has settled. */
- (void)adoptOutsideChanges
{
    if ([[NSDate date] timeIntervalSinceDate:lastApplied_] <= 5)
        return;
    io_service_t dev = driverService();
    if (!dev)
        return;
    IOObjectRelease(dev);
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    for (NSString *key in allSettings()) {
        id current = driverProperty(key);
        if (current && ![[defaults objectForKey:key] isEqual:current])
            [defaults setObject:current forKey:key];
    }
}

/* Push every saved choice to the driver (no-op if the card is absent). */
- (void)applySaved
{
    [lastApplied_ release];
    lastApplied_ = [[NSDate date] retain];
    NSMutableDictionary *settings = [NSMutableDictionary dictionary];
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    for (NSString *key in allSettings()) {
        id value = [defaults objectForKey:key];
        if (value)
            settings[key] = value;
    }
    driverWrite(settings);
}

- (void)choose:(NSString *)key value:(id)value
{
    if (driverWrite(@{key: value})) {
        NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
        [defaults setObject:value forKey:key];
        // the driver normalizes related keys (monitor on/level): save those too
        for (NSString *related in @[kMonitor, kMonitorLevel]) {
            if ([related isEqualToString:key])
                continue;
            id current = driverProperty(related);
            if (current)
                [defaults setObject:current forKey:related];
        }
    } else {
        NSBeep();
    }
    [self updateTitle];
}

/* Reapply the saved settings whenever the driver publishes a device:
 * after a driver reload, and after wake if the device is re-created. */
- (void)watchForCard
{
    notifyPort_ = IONotificationPortCreate(kIOMasterPortDefault);
    if (!notifyPort_)
        return;
    IONotificationPortSetDispatchQueue(notifyPort_, dispatch_get_main_queue());
    IOServiceAddMatchingNotification(notifyPort_, kIOMatchedNotification,
                                     IOServiceMatching([kDriverClass UTF8String]),
                                     cardMatched, self, &matchIterator_);
    cardMatched(self, matchIterator_);  // arm the notification
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserver:self selector:@selector(didWake:) name:NSWorkspaceDidWakeNotification object:nil];
}

- (void)didWake:(NSNotification *)notification
{
    // The chip state is restored by the driver itself; reapplying is cheap
    // insurance in case the device was re-created.
    [self performSelector:@selector(applySaved) withObject:nil afterDelay:3];
}

#pragma mark menu

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];
    io_service_t dev = driverService();
    if (!dev) {
        [menu addItem:[self disabled:@"No Xonar Essence STX / STX II found"]];
        [menu addItem:[self disabled:@"Is CMI8788Driver loaded?"]];
        [self addFooter];
        return;
    }
    IOObjectRelease(dev);
    // The model the driver detected from the card's EEPROM, e.g. "Xonar Essence STX II".
    [menu addItem:[self disabled:driverString(@"IOAudioDeviceName", @"Xonar Essence STX")]];
    [menu addItem:[NSMenuItem separatorItem]];

    [self addSubmenu:@"Output" key:kOutput choices:outputChoices()
             current:driverString(kOutput, @"headphones")];
    [self addSubmenu:@"Input" key:kInput choices:inputChoices()
             current:driverString(kInput, @"line")];
    NSString *monitor = driverString(kMonitor, @"off");
    NSString *source = driverString(@"InputSourceName", @"Line In");
    NSMenuItem *toggle = [self action:[NSString stringWithFormat:@"Monitor Input (%@)", source]
                             selector:@selector(toggleMonitor:)];
    toggle.state = [monitor isEqualToString:@"off"] ? NSOffState : NSOnState;
    toggle.toolTip = @"Play the input straight to the outputs, in hardware (no latency).";
    [menu addItem:toggle];
    [self addSubmenu:@"Monitoring Level" key:kMonitorLevel choices:monitorLevelChoices()
             current:driverString(kMonitorLevel, @"half")];
    [menu addItem:[NSMenuItem separatorItem]];

    id ohms = driverProperty(kImpedance);
    NSNumber *band = [ohms isKindOfClass:[NSNumber class]] ? @(impedanceBand([ohms intValue])) : nil;
    [self addSubmenu:@"Headphone Impedance" key:kImpedance choices:impedanceChoices() current:band];
    [self addSubmenu:@"DAC Filter" key:kFilter choices:filterChoices()
             current:driverString(kFilter, @"sharp")];

    id deemph = driverProperty(kDeemphasis);
    NSMenuItem *item = [self action:@"De-emphasis" selector:@selector(toggleDeemphasis:)];
    item.state = [deemph respondsToSelector:@selector(boolValue)] && [deemph boolValue] ? NSOnState : NSOffState;
    [menu addItem:item];

    id spdif = driverProperty(kSPDIF);
    NSMenuItem *spdifItem = [self action:@"S/PDIF Output" selector:@selector(toggleSPDIF:)];
    spdifItem.state = [spdif respondsToSelector:@selector(boolValue)] && [spdif boolValue] ? NSOnState : NSOffState;
    spdifItem.toolTip = @"Send the same stereo as the analog outputs to the coaxial/optical jack.";
    [menu addItem:spdifItem];
    [self addFooter];
}

- (void)addSubmenu:(NSString *)title key:(NSString *)key choices:(NSArray *)choices current:(id)current
{
    NSMenu *submenu = [[[NSMenu alloc] init] autorelease];
    for (NSArray *choice in choices) {
        NSMenuItem *item = [self action:choice[0] selector:@selector(pick:)];
        item.representedObject = @[key, choice[1]];
        item.state = [choice[1] isEqual:current] ? NSOnState : NSOffState;
        [submenu addItem:item];
    }
    NSMenuItem *parent = [[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""] autorelease];
    parent.submenu = submenu;
    [menu_ addItem:parent];
}

- (void)addFooter
{
    [menu_ addItem:[NSMenuItem separatorItem]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:kextSupportPath()])
        [menu_ addItem:[self action:@"Show Kext for OpenCore…" selector:@selector(showKext:)]];
    [menu_ addItem:[self action:@"Sound Preferences…" selector:@selector(openSound:)]];
    [menu_ addItem:[NSMenuItem separatorItem]];
    [menu_ addItem:[self action:@"Quit STX" selector:@selector(quit:)]];
}

- (NSMenuItem *)disabled:(NSString *)title
{
    NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""] autorelease];
    item.enabled = NO;
    return item;
}

- (NSMenuItem *)action:(NSString *)title selector:(SEL)selector
{
    NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:title action:selector keyEquivalent:@""] autorelease];
    item.target = self;
    return item;
}

- (void)pick:(NSMenuItem *)sender
{
    NSArray *pair = sender.representedObject;
    if ([pair isKindOfClass:[NSArray class]] && pair.count == 2)
        [self choose:pair[0] value:pair[1]];
}

- (void)toggleMonitor:(NSMenuItem *)sender
{
    NSString *level = driverString(kMonitorLevel, @"half");
    [self choose:kMonitor value:sender.state == NSOnState ? @"off" : level];
}

- (void)toggleSPDIF:(NSMenuItem *)sender
{
    [self choose:kSPDIF value:@(sender.state != NSOnState)];
}

- (void)toggleDeemphasis:(NSMenuItem *)sender
{
    [self choose:kDeemphasis value:@(sender.state != NSOnState)];
}

- (void)showKext:(id)sender
{
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:kextSupportPath()]]];
}

- (void)openSound:(id)sender
{
    [[NSWorkspace sharedWorkspace] openFile:@"/System/Library/PreferencePanes/Sound.prefPane"];
}

- (void)quit:(id)sender
{
    [NSApp terminate:nil];
}

@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
