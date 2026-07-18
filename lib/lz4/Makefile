ccflags-y += -O3 \
    -DLZ4_FREESTANDING=1 \
    -DLZ4_FAST_DEC_LOOP=1

#
# freestanding LZ4/LZ4HC intentionally keep some large local work buffers to
# avoid heap allocation in kernel contexts where that may be undesirable.
# Silence the frame-size warning for those translation units only.
#
CFLAGS_REMOVE_lz4.o += -Wframe-larger-than=${CONFIG_FRAME_WARN}
CFLAGS_REMOVE_lz4hc.o += -Wframe-larger-than=${CONFIG_FRAME_WARN}

obj-y += lz4.o lz4hc.o

obj-$(CONFIG_ARM64) += $(addprefix lz4armv8/, lz4accel.o lz4armv8.o)
