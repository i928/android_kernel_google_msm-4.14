ccflags-y += -O3

# Decompression is the hot path (EROFS lz4 partitions, zram): unroll its loops
# and schedule it for the big cores. -mtune only changes instruction
# scheduling; the ISA stays the arch default (A55/A75/A76 are all ARMv8.2), so
# the code is still correct on the little cores.
CFLAGS_lz4_decompress.o += -funroll-loops
ifeq ($(CONFIG_ARCH_SDM845),y)		# crosshatch: 4x A75 + 4x A55
CFLAGS_lz4_decompress.o += -mtune=cortex-a75
endif
ifeq ($(CONFIG_ARCH_SM6150),y)		# sunfish: 2x A76 + 6x A55
CFLAGS_lz4_decompress.o += -mtune=cortex-a76
endif

obj-$(CONFIG_LZ4_COMPRESS) += lz4_compress.o
obj-$(CONFIG_LZ4HC_COMPRESS) += lz4hc_compress.o
obj-$(CONFIG_LZ4_DECOMPRESS) += lz4_decompress.o
