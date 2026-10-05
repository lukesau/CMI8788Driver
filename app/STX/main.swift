// SPDX-License-Identifier: GPL-2.0-only
//
// STX: menu bar app for the CMI8788Driver card settings that Sound preferences
// has no UI for (headphone impedance, input monitoring, DAC filter,
// de-emphasis). Talks to the driver through its I/O registry properties, saves
// the choices, and reapplies them whenever the card (re)appears: at login,
// after wake, after a driver reload.

import AppKit
import IOKit

let driverClass = "CMI8788AudioDevice"
let kextSupportPath = "/Library/Application Support/CMI8788Driver/CMI8788Driver.kext"

enum Setting: String, CaseIterable {
    case impedance = "HeadphoneImpedance"   // NSNumber, ohms
    case input = "InputSource"              // "line" | "mic" | "frontmic"
    case monitorLevel = "InputMonitorLevel" // "half" | "full": level used when on
    case monitor = "InputMonitor"           // "off" | "half" | "full"
    case filter = "DACFilter"               // "sharp" | "slow"
    case deemphasis = "Deemphasis"          // Bool
    case spdif = "SPDIFOutput"              // Bool: S/PDIF mirrors the analog output
}

/// One menu choice: the label, and the value sent to the driver.
struct Choice {
    let title: String
    let value: Any
}

// Representative impedances for the driver's four gain-offset bands.
let impedanceChoices = [
    Choice(title: "Under 32 Ω (−18 dB)", value: 16),
    Choice(title: "32–64 Ω (−12 dB)", value: 32),
    Choice(title: "64–300 Ω (−6 dB)", value: 64),
    Choice(title: "300–600 Ω (0 dB)", value: 300),
]
let inputChoices = [
    Choice(title: "Line In", value: "line"),
    Choice(title: "Microphone", value: "mic"),
    Choice(title: "Front Panel Microphone", value: "frontmic"),
]
let monitorLevelChoices = [
    Choice(title: "−6 dB", value: "half"),
    Choice(title: "0 dB", value: "full"),
]
let filterChoices = [
    Choice(title: "Sharp Roll-off", value: "sharp"),
    Choice(title: "Slow Roll-off", value: "slow"),
]

func impedanceBand(_ ohms: Int) -> Int {
    switch ohms {
    case ..<32: return 16
    case ..<64: return 32
    case ..<300: return 64
    default: return 300
    }
}

final class Driver {
    /// The driver's registry entry, or 0 if the card isn't present.
    static func service() -> io_service_t {
        IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(driverClass))
    }

    static func read(_ setting: Setting) -> Any? {
        property(setting.rawValue)
    }

    static func property(_ key: String) -> Any? {
        let dev = service()
        guard dev != 0 else { return nil }
        defer { IOObjectRelease(dev) }
        return IORegistryEntryCreateCFProperty(dev, key as CFString,
                                               kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    /// The model the driver detected from the card's EEPROM, e.g.
    /// "Xonar Essence STX II" (IOAudioFamily's kIOAudioDeviceNameKey).
    static var modelName: String {
        property("IOAudioDeviceName") as? String ?? "Xonar Essence STX"
    }

    @discardableResult
    static func write(_ settings: [String: Any]) -> Bool {
        let dev = service()
        guard dev != 0, !settings.isEmpty else { return false }
        defer { IOObjectRelease(dev) }
        return IORegistryEntrySetCFProperties(dev, settings as CFDictionary) == KERN_SUCCESS
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let menu = NSMenu()
    var notifyPort: IONotificationPortRef?
    var matchIterator: io_iterator_t = 0
    /// When the saved settings were last pushed to the driver. Changes made
    /// elsewhere are only adopted once the device has settled after that, so
    /// a freshly loaded driver's defaults never overwrite your choices.
    var lastApplied = Date.distantPast

    func applicationDidFinishLaunching(_ notification: Notification) {
        // One menu bar item only: the LaunchAgent and macOS's "reopen apps at
        // login" can both start us.
        let me = ProcessInfo.processInfo.processIdentifier
        let others = NSRunningApplication.runningApplications(
            withBundleIdentifier: Bundle.main.bundleIdentifier ?? "com.lukesau.stx")
            .filter { $0.processIdentifier != me }
        if !others.isEmpty {
            NSApp.terminate(nil)
            return
        }
        menu.delegate = self
        statusItem.menu = menu
        watchForCard()
        applySaved()
        updateTitle()
        // Monitoring can also be switched by stxctl or another app (CoreAudio
        // play-through), so keep the indicator current.
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.adoptOutsideChanges()
            self?.updateTitle()
        }
    }

    /// "STX ●" while input monitoring is on, so it's never on unnoticed.
    func updateTitle() {
        let monitoring = (Driver.read(.monitor) as? String).map { $0 != "off" } ?? false
        statusItem.button?.title = monitoring ? "STX ●" : "STX"
    }

    // MARK: persistence

    /// Save settings changed outside the app (Sound preferences, stxctl, other
    /// apps' play-through) as the new choices, once the device has settled.
    func adoptOutsideChanges() {
        guard Date().timeIntervalSince(lastApplied) > 5, Driver.service() != 0 else { return }
        for setting in Setting.allCases {
            guard let current = Driver.read(setting) else { continue }
            let saved = UserDefaults.standard.object(forKey: setting.rawValue)
            if !(saved as AnyObject).isEqual(current) {
                UserDefaults.standard.set(current, forKey: setting.rawValue)
            }
        }
    }

    /// Push every saved choice to the driver (no-op if the card is absent).
    func applySaved() {
        lastApplied = Date()
        var settings: [String: Any] = [:]
        for setting in Setting.allCases {
            if let value = UserDefaults.standard.object(forKey: setting.rawValue) {
                settings[setting.rawValue] = value
            }
        }
        Driver.write(settings)
    }

    func choose(_ setting: Setting, _ value: Any) {
        if Driver.write([setting.rawValue: value]) {
            UserDefaults.standard.set(value, forKey: setting.rawValue)
            // the driver normalizes related keys (monitor on/level): save those too
            for related in [Setting.monitor, .monitorLevel] where related != setting {
                if let current = Driver.read(related) {
                    UserDefaults.standard.set(current, forKey: related.rawValue)
                }
            }
        } else {
            NSSound.beep()
        }
        updateTitle()
    }

    /// Reapply the saved settings whenever the driver publishes a device:
    /// after a driver reload, and after wake if the device is re-created.
    func watchForCard() {
        notifyPort = IONotificationPortCreate(kIOMasterPortDefault)
        guard let port = notifyPort else { return }
        IONotificationPortSetDispatchQueue(port, DispatchQueue.main)
        let me = Unmanaged.passUnretained(self).toOpaque()
        let callback: IOServiceMatchingCallback = { refcon, iterator in
            var service = IOIteratorNext(iterator)
            var found = false
            while service != 0 {
                found = true
                IOObjectRelease(service)
                service = IOIteratorNext(iterator)
            }
            if found, let refcon = refcon {
                Unmanaged<AppDelegate>.fromOpaque(refcon).takeUnretainedValue().applySaved()
            }
        }
        IOServiceAddMatchingNotification(port, kIOMatchedNotification, IOServiceMatching(driverClass),
                                         callback, me, &matchIterator)
        callback(me, matchIterator)   // arm the notification
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(didWake), name: NSWorkspace.didWakeNotification, object: nil)
    }

    @objc func didWake() {
        // The chip state is restored by the driver itself; reapplying is cheap
        // insurance in case the device was re-created.
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { self.applySaved() }
    }

    // MARK: menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        guard Driver.service() != 0 else {
            menu.addItem(disabled("No Xonar Essence STX / STX II found"))
            menu.addItem(disabled("Is CMI8788Driver loaded?"))
            addFooter()
            return
        }
        menu.addItem(disabled(Driver.modelName))
        menu.addItem(.separator())

        let input = Driver.read(.input) as? String ?? "line"
        addSubmenu("Input", .input, inputChoices) { $0 as? String == input }
        let monitor = Driver.read(.monitor) as? String ?? "off"
        let source = Driver.property("InputSourceName") as? String ?? "Line In"
        let toggle = NSMenuItem(title: "Monitor Input (\(source))",
                                action: #selector(toggleMonitor(_:)), keyEquivalent: "")
        toggle.target = self
        toggle.state = monitor != "off" ? .on : .off
        toggle.toolTip = "Play the input straight to the outputs, in hardware (no latency)."
        menu.addItem(toggle)
        let level = Driver.read(.monitorLevel) as? String ?? "half"
        addSubmenu("Monitoring Level", .monitorLevel, monitorLevelChoices) { $0 as? String == level }
        menu.addItem(.separator())

        let ohms = (Driver.read(.impedance) as? NSNumber).map { impedanceBand($0.intValue) }
        addSubmenu("Headphone Impedance", .impedance, impedanceChoices) { $0 as? Int == ohms }
        let filter = Driver.read(.filter) as? String ?? "sharp"
        addSubmenu("DAC Filter", .filter, filterChoices) { $0 as? String == filter }

        let deemph = (Driver.read(.deemphasis) as? NSNumber)?.boolValue ?? false
        let item = NSMenuItem(title: "De-emphasis", action: #selector(toggleDeemphasis(_:)),
                              keyEquivalent: "")
        item.target = self
        item.state = deemph ? .on : .off
        menu.addItem(item)

        let spdif = (Driver.read(.spdif) as? NSNumber)?.boolValue ?? false
        let spdifItem = NSMenuItem(title: "S/PDIF Output", action: #selector(toggleSPDIF(_:)),
                                   keyEquivalent: "")
        spdifItem.target = self
        spdifItem.state = spdif ? .on : .off
        spdifItem.toolTip = "Send the same stereo as the analog outputs to the coaxial/optical jack."
        menu.addItem(spdifItem)
        addFooter()
    }

    func addSubmenu(_ title: String, _ setting: Setting, _ choices: [Choice],
                    isCurrent: (Any) -> Bool) {
        let submenu = NSMenu()
        for choice in choices {
            let item = NSMenuItem(title: choice.title, action: #selector(pick(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = [setting.rawValue, choice.value]
            item.state = isCurrent(choice.value) ? .on : .off
            submenu.addItem(item)
        }
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        parent.submenu = submenu
        menu.addItem(parent)
    }

    func addFooter() {
        menu.addItem(.separator())
        if FileManager.default.fileExists(atPath: kextSupportPath) {
            menu.addItem(action("Show Kext for OpenCore…", #selector(showKext)))
        }
        menu.addItem(action("Sound Preferences…", #selector(openSound)))
        menu.addItem(.separator())
        menu.addItem(action("Quit STX", #selector(quit)))
    }

    func disabled(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    func action(_ title: String, _ selector: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: selector, keyEquivalent: "")
        item.target = self
        return item
    }

    @objc func pick(_ sender: NSMenuItem) {
        guard let pair = sender.representedObject as? [Any], pair.count == 2,
              let key = pair[0] as? String, let setting = Setting(rawValue: key) else { return }
        choose(setting, pair[1])
    }

    @objc func toggleMonitor(_ sender: NSMenuItem) {
        let level = Driver.read(.monitorLevel) as? String ?? "half"
        choose(.monitor, sender.state == .on ? "off" : level)
    }

    @objc func toggleSPDIF(_ sender: NSMenuItem) {
        choose(.spdif, sender.state != .on)
    }

    @objc func toggleDeemphasis(_ sender: NSMenuItem) {
        choose(.deemphasis, sender.state != .on)
    }

    @objc func showKext() {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: kextSupportPath)])
    }

    @objc func openSound() {
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Library/PreferencePanes/Sound.prefPane"))
    }

    @objc func quit() {
        NSApp.terminate(nil)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
