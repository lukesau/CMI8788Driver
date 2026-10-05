# Builds CMI8788Driver.kext without Xcode (Command Line Tools are enough).
#
#   make            build build/CMI8788Driver.kext (run on the Catalina box)
#   make remote     rsync this checkout to $(REMOTE) and build there
#   make load       copy to /tmp, chown root:wheel, kextutil it (needs SIP off)
#   make unload     kextunload it
#   make clean

PRODUCT   := CMI8788Driver
BUNDLE_ID := com.lukesau.driver.$(PRODUCT)
VERSION   := 1.0
MINOS     := 10.15

SRC_DIR   := CMI8788Driver
BUILD     := build
KEXT      := $(BUILD)/$(PRODUCT).kext

# Prefer the 10.15 SDK from Command Line Tools 12.4; fall back to whatever xcrun finds.
SDK ?= $(firstword $(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX10.15.sdk) \
                   $(shell xcrun --show-sdk-path 2>/dev/null))
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
LDFLAGS   := $(ARCHFLAGS) -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD)/obj/%.o,$(SRCS)) $(BUILD)/obj/kmod_info.o

REMOTE     ?= hackintosh
REMOTE_DIR ?= CMI8788Driver

.PHONY: all clean remote load unload

all: $(KEXT)

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
	    $(SRC_DIR)/$(PRODUCT)-Info.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist

$(BUILD)/obj:
	@mkdir -p $@

clean:
	rm -rf $(BUILD)

remote:
	rsync -a --delete --exclude .git --exclude hackintosh --exclude build ./ $(REMOTE):$(REMOTE_DIR)/
	ssh $(REMOTE) 'cd $(REMOTE_DIR) && make'

load: $(KEXT)
	sudo rm -rf /tmp/$(PRODUCT).kext
	sudo cp -R $(KEXT) /tmp/
	sudo chown -R root:wheel /tmp/$(PRODUCT).kext
	sudo kextutil -v 1 /tmp/$(PRODUCT).kext

unload:
	sudo kextunload -b $(BUNDLE_ID)
