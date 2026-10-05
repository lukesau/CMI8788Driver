#!/bin/sh
# Remove CMI8788Driver and the STX app. Run with: sudo sh uninstall.sh
# (OpenCore users: also remove CMI8788Driver.kext from EFI/OC/Kexts and its
# Kernel > Add entry in config.plist.)
if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi
user=$(stat -f %Su /dev/console)
[ -n "$user" ] && [ "$user" != root ] && launchctl bootout gui/"$(id -u "$user")"/com.lukesau.stx 2>/dev/null
pkill -x STX 2>/dev/null
kextunload -b com.lukesau.driver.CMI8788Driver 2>/dev/null
rm -rf /Applications/STX.app /Library/LaunchAgents/com.lukesau.stx.plist /usr/local/bin/stxctl \
       /Library/Extensions/CMI8788Driver.kext "/Library/Application Support/CMI8788Driver"
touch /Library/Extensions
if [ -x /usr/bin/kmutil ] && [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 11 ]; then
    kmutil install --update-all >/dev/null 2>&1 || true
else
    kextcache -i / >/dev/null 2>&1 || true
fi
pkgutil --forget com.lukesau.stx.app >/dev/null 2>&1
pkgutil --forget com.lukesau.driver.CMI8788Driver >/dev/null 2>&1
echo "CMI8788Driver and STX removed."
