# Builds CMI8788Driver.kext without Xcode (Command Line Tools are enough).
#
#   make            build the kexts (10.15+ and 10.9), the STX app and stxctl
#   make kext       just build/CMI8788Driver.kext (MINOS=10.9 SDK=... BUILD=... for others)
#   make remote     rsync this checkout to $(REMOTE) and build there
#   make load       copy to /tmp, chown root:wheel, kextutil it (needs SIP off)
#   make unload     kextunload it
#   make pkg        installer package (STX app + tools, kext) in dist/
#   make dist       release zip and installer in dist/
#   make clean

PRODUCT   := CMI8788Driver
BUNDLE_ID := com.lukesau.driver.$(PRODUCT)
VERSION   := 0.2.1
MINOS     ?= 10.15
# OSBundleLibraries kpi version = the Darwin version of MINOS
# (10.x -> x+4, 11-15 -> +9, 26+ -> -1 since macOS jumped from 15 to 26).
KPI_VERSION := $(shell echo $(MINOS) | awk -F. '{ print ($$1 == 10 ? $$2 + 4 : $$1 >= 26 ? $$1 - 1 : $$1 + 9) ".0" }')

SRC_DIR   := CMI8788Driver
BUILD     ?= build
KEXT      := $(BUILD)/$(PRODUCT).kext

# Prefer the 10.15 SDK from Command Line Tools 12.4; fall back to whatever xcrun finds.
SDK ?= $(firstword $(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX10.15.sdk \
                              hackintosh/SDKs/MacOSX10.15.sdk) \
                   $(shell xcrun --show-sdk-path 2>/dev/null))
# The 10.9 SDK (from Xcode 6.1.1) builds the 10.9 kext, and the app and stxctl,
# which target 10.9 so one copy runs on every macOS the driver supports.
SDK_10_9 ?= $(firstword $(wildcard hackintosh/SDKs/MacOSX10.9.sdk sdks/MacOSX10.9.sdk))
TOOLS_MINOS := 10.9
KERNEL_HEADERS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

CXX := xcrun clang++
CC  := xcrun clang

ARCHFLAGS := -arch x86_64 -mmacosx-version-min=$(MINOS)
KERNFLAGS := -mkernel -fapple-kext -nostdinc -fno-builtin -fno-common \
             -isystem $(KERNEL_HEADERS) \
             -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
# IOAudioFamily is deprecated (but still ships), so silence its deprecation noise.
WARNFLAGS := -Wall -Wno-unused-parameter -Wno-\#warnings -Wno-deprecated-declarations
CXXFLAGS  := $(ARCHFLAGS) $(KERNFLAGS) $(WARNFLAGS) -std=gnu++14 -fno-exceptions -fno-rtti -O2 -g
CFLAGS    := $(ARCHFLAGS) $(filter-out -fapple-kext,$(KERNFLAGS)) $(WARNFLAGS) -O2 -g
# -isysroot so libkmod/libkmodc++ come from the target SDK, not the host's newest one.
LDFLAGS   := $(ARCHFLAGS) -isysroot $(SDK) -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD)/obj/%.o,$(SRCS)) $(BUILD)/obj/kmod_info.o

REMOTE     ?= hackintosh
REMOTE_DIR ?= CMI8788Driver

.PHONY: all kext kext-10.9 need-sdk-10.9 clean remote load unload pkg dist

STXCTL    := $(BUILD)/stxctl
APP       := $(BUILD)/STX.app
KEXT_10_9 := $(BUILD)/10.9/$(PRODUCT).kext

all: $(KEXT) kext-10.9 $(STXCTL) $(APP)
kext: $(KEXT)

kext-10.9: need-sdk-10.9
	$(MAKE) kext MINOS=10.9 SDK=$(abspath $(SDK_10_9)) BUILD=$(BUILD)/10.9

need-sdk-10.9:
	@test -d "$(SDK_10_9)" || { echo "need the 10.9 SDK (Xcode 6.1.1): set SDK_10_9=/path/to/MacOSX10.9.sdk"; exit 1; }

$(STXCTL): tools/stxctl.c | need-sdk-10.9 $(BUILD)/obj
	$(CC) -arch x86_64 -mmacosx-version-min=$(TOOLS_MINOS) -isysroot $(SDK_10_9) -O2 -Wall \
	    -framework IOKit -framework CoreFoundation $< -o $@

# Menu bar app, built without Xcode. Ad-hoc signed.
$(APP): app/STX/main.m app/STX/Info.plist app/STX/AppIcon.icns | need-sdk-10.9 $(BUILD)/obj
	@rm -rf $@
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp app/STX/AppIcon.icns $@/Contents/Resources/
	$(CC) -arch x86_64 -mmacosx-version-min=$(TOOLS_MINOS) -isysroot $(SDK_10_9) -O2 -Wall \
	    -fno-objc-arc -framework AppKit -framework IOKit app/STX/main.m -o $@/Contents/MacOS/STX
	sed -e 's/$${MODULE_VERSION}/$(VERSION)/g' app/STX/Info.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	codesign -s - -f $@

$(BUILD)/obj/%.o: $(SRC_DIR)/%.cpp $(wildcard $(SRC_DIR)/*.h) | $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Xcode normally generates this: the kmod_info the kernel reads to start the kext.
$(BUILD)/obj/kmod_info.c: Makefile | $(BUILD)/obj
	@printf '%s\n' \
	  '#include <mach/mach_types.h>' \
	  'extern kern_return_t _start(kmod_info_t *, void *);' \
	  'extern kern_return_t _stop(kmod_info_t *, void *);' \
	  '__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($(BUNDLE_ID), "$(VERSION)", _start, _stop)' \
	  '__private_extern__ kmod_start_func_t *_realmain = 0;' \
	  '__private_extern__ kmod_stop_func_t *_antimain = 0;' \
	  '__private_extern__ int _kext_apple_cc = __APPLE_CC__;' > $@

$(BUILD)/obj/kmod_info.o: $(BUILD)/obj/kmod_info.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) $(SRC_DIR)/$(PRODUCT)-Info.plist
	@rm -rf $@
	@mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OBJS) -o $@/Contents/MacOS/$(PRODUCT)
	sed -e 's/$${EXECUTABLE_NAME}/$(PRODUCT)/g' \
	    -e 's/$${PRODUCT_NAME:rfc1034identifier}/$(PRODUCT)/g' \
	    -e 's/$${PRODUCT_NAME}/$(PRODUCT)/g' \
	    -e 's/$${MODULE_VERSION}/$(VERSION)/g' \
	    -e 's/$${KPI_VERSION}/$(KPI_VERSION)/g' \
	    $(SRC_DIR)/$(PRODUCT)-Info.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist

$(BUILD)/obj:
	@mkdir -p $@

clean:
	rm -rf $(BUILD) dist

# Installer: component packages (app + tools, kext for 10.15+, kext for 10.9)
# under one product with a choice per component; see installer/distribution.xml.
PKG        := dist/$(PRODUCT)-$(VERSION).pkg
PKGBUILD   := $(BUILD)/pkg
APP_ROOT   := $(PKGBUILD)/app-root
KEXT_ROOT  := $(PKGBUILD)/kext-root
KEXT9_ROOT := $(PKGBUILD)/kext9-root
SUPPORT    := $(APP_ROOT)/Library/Application Support/$(PRODUCT)

pkg: all
	rm -rf $(PKGBUILD)
	mkdir -p $(APP_ROOT)/Applications $(APP_ROOT)/Library/LaunchAgents $(APP_ROOT)/usr/local/bin \
	         "$(SUPPORT)" $(KEXT_ROOT)/Library/Extensions $(KEXT9_ROOT)/Library/Extensions \
	         $(PKGBUILD)/resources dist
	cp -R $(APP) $(APP_ROOT)/Applications/
	cp installer/com.lukesau.stx.plist $(APP_ROOT)/Library/LaunchAgents/
	cp $(STXCTL) $(APP_ROOT)/usr/local/bin/
	cp -R $(KEXT) installer/uninstall.sh "$(SUPPORT)/"
	cp -R $(KEXT_10_9) "$(SUPPORT)/$(PRODUCT)-10.9.kext"
	cp -R $(KEXT) $(KEXT_ROOT)/Library/Extensions/
	cp -R $(KEXT_10_9) $(KEXT9_ROOT)/Library/Extensions/
	for c in app kext kext9; do \
	    pkgbuild --analyze --root $(PKGBUILD)/$$c-root $(PKGBUILD)/$$c.plist >/dev/null && \
	    n=$$(plutil -convert json -o - $(PKGBUILD)/$$c.plist | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))') && \
	    i=0; while [ $$i -lt $$n ]; do \
	        plutil -replace $$i.BundleIsRelocatable -bool NO $(PKGBUILD)/$$c.plist; i=$$((i+1)); done; \
	done
	pkgbuild --root $(APP_ROOT) --component-plist $(PKGBUILD)/app.plist --scripts installer/scripts-app \
	    --identifier com.lukesau.stx.app --version $(VERSION) --install-location / $(PKGBUILD)/STX-app.pkg
	pkgbuild --root $(KEXT_ROOT) --component-plist $(PKGBUILD)/kext.plist --scripts installer/scripts-kext \
	    --identifier $(BUNDLE_ID) --version $(VERSION) --install-location / $(PKGBUILD)/$(PRODUCT)-kext.pkg
	pkgbuild --root $(KEXT9_ROOT) --component-plist $(PKGBUILD)/kext9.plist --scripts installer/scripts-kext \
	    --identifier $(BUNDLE_ID).10.9 --version $(VERSION) --install-location / $(PKGBUILD)/$(PRODUCT)-kext-10.9.pkg
	cp installer/resources/* $(PKGBUILD)/resources/
	cp COPYING $(PKGBUILD)/resources/COPYING.txt
	productbuild --distribution installer/distribution.xml --resources $(PKGBUILD)/resources \
	    --package-path $(PKGBUILD) --version $(VERSION) $(PKG)

# Always a clean build from the checked-in Info.plist (no Debug key).
DIST_ZIP := dist/$(PRODUCT)-$(VERSION).zip
dist:
	rm -rf $(BUILD) dist
	$(MAKE) all
	mkdir -p dist/$(PRODUCT)-$(VERSION)
	cp -R $(KEXT) $(STXCTL) $(APP) installer/uninstall.sh README.md COPYING dist/$(PRODUCT)-$(VERSION)/
	cp -R $(KEXT_10_9) dist/$(PRODUCT)-$(VERSION)/$(PRODUCT)-10.9.kext
	cd dist && ditto -c -k --norsrc --keepParent $(PRODUCT)-$(VERSION) $(PRODUCT)-$(VERSION).zip
	$(MAKE) pkg
	shasum -a 256 $(DIST_ZIP) $(PKG)

remote:
	rsync -a --delete --exclude .git --exclude hackintosh --exclude build --exclude dist ./ $(REMOTE):$(REMOTE_DIR)/
	ssh $(REMOTE) 'cd $(REMOTE_DIR) && make'

load: $(KEXT)
	sudo rm -rf /tmp/$(PRODUCT).kext
	sudo cp -R $(KEXT) /tmp/
	sudo chown -R root:wheel /tmp/$(PRODUCT).kext
	sudo kextutil -v 1 /tmp/$(PRODUCT).kext

unload:
	sudo kextunload -b $(BUNDLE_ID)
