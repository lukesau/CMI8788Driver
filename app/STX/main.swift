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
    case monitor = "InputMonitor"           // "off" | "half" | "full"
    case filter = "DACFilter"               // "sharp" | "slow"
    case deemphasis = "Deemphasis"          // Bool
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
let monitorChoices = [
    Choice(title: "Off", value: "off"),
    Choice(title: "On, −6 dB", value: "half"),
    Choice(title: "On, 0 dB", value: "full"),
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

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem.button?.title = "STX"
        menu.delegate = self
        statusItem.menu = menu
        watchForCard()
        applySaved()
    }

    // MARK: persistence

    /// Push every saved choice to the driver (no-op if the card is absent).
    func applySaved() {
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
        } else {
            NSSound.beep()
        }
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

        let ohms = (Driver.read(.impedance) as? NSNumber).map { impedanceBand($0.intValue) }
        addSubmenu("Headphone Impedance", .impedance, impedanceChoices) { $0 as? Int == ohms }
        let monitor = Driver.read(.monitor) as? String ?? "off"
        addSubmenu("Input Monitoring", .monitor, monitorChoices) { $0 as? String == monitor }
        let filter = Driver.read(.filter) as? String ?? "sharp"
        addSubmenu("DAC Filter", .filter, filterChoices) { $0 as? String == filter }

        let deemph = (Driver.read(.deemphasis) as? NSNumber)?.boolValue ?? false
        let item = NSMenuItem(title: "De-emphasis", action: #selector(toggleDeemphasis(_:)),
                              keyEquivalent: "")
        item.target = self
        item.state = deemph ? .on : .off
        menu.addItem(item)
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
