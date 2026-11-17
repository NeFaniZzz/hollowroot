# Cross-compile for aarch64 Android with the NDK.
#   make NDK=/path/to/ndk
NDK ?= $(ANDROID_NDK_HOME)
API ?= 35
CLANG ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(API)-clang
CFLAGS ?= -O2 -Wall -s

OUT := build

all: $(OUT)/loader $(OUT)/guardian $(OUT)/property_seed $(OUT)/dump_properties $(OUT)/test_pivot

$(OUT):
	mkdir -p $(OUT)

# loader and guardian are static, they run next to a half-stopped host userspace
$(OUT)/loader: src/loader.c | $(OUT)
	$(CLANG) $(CFLAGS) -static -o $@ $<

$(OUT)/guardian: src/guardian.c | $(OUT)
	$(CLANG) $(CFLAGS) -static -o $@ $<

$(OUT)/property_seed: src/property_seed.c | $(OUT)
	$(CLANG) $(CFLAGS) -o $@ $<

$(OUT)/dump_properties: tools/dump_properties.c | $(OUT)
	$(CLANG) $(CFLAGS) -o $@ $<

$(OUT)/test_pivot: tests/test_pivot.c | $(OUT)
	$(CLANG) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
