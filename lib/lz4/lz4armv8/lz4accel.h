#include <linux/types.h>
#include <asm/simd.h>

#define LZ4_FAST_MARGIN                (128)

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
#include <asm/neon.h>
#include <asm/cputype.h>

asmlinkage int _lz4_decompress_asm(uint8_t **dst_ptr, uint8_t *dst_begin,
				   uint8_t *dst_end, const uint8_t **src_ptr,
				   const uint8_t *src_end, bool dip);

asmlinkage int _lz4_decompress_asm_noprfm(uint8_t **dst_ptr, uint8_t *dst_begin,
					  uint8_t *dst_end, const uint8_t **src_ptr,
					  const uint8_t *src_end, bool dip);

/* TEST: 0 = per-CPU selected asm, 1 = noprfm, 2 = v2 (lib/lz4/lz4.c asm_variant) */
asmlinkage int _lz4_decompress_asm_v2(uint8_t **dst_ptr, uint8_t *dst_begin,
				      uint8_t *dst_end, const uint8_t **src_ptr,
				      const uint8_t *src_end, bool dip);
extern int lz4_asm_variant;

static inline int lz4_decompress_accel_enable(void)
{
	return	may_use_simd();
}

extern int (*lz4_decompress_asm_fn[])(uint8_t **dst_ptr, uint8_t *dst_begin,
	uint8_t *dst_end, const uint8_t **src_ptr,
	const uint8_t *src_end, bool dip);

static inline ssize_t lz4_decompress_asm(
	uint8_t **dst_ptr, uint8_t *dst_begin, uint8_t *dst_end,
	const uint8_t **src_ptr, const uint8_t *src_end, bool dip)
{
	int ret;

	kernel_neon_begin();
	switch (READ_ONCE(lz4_asm_variant)) {
	case 1:
		ret = _lz4_decompress_asm_noprfm(dst_ptr, dst_begin, dst_end,
						 src_ptr, src_end, dip);
		break;
	case 2:
		ret = _lz4_decompress_asm_v2(dst_ptr, dst_begin, dst_end,
					     src_ptr, src_end, dip);
		break;
	default:
		ret = lz4_decompress_asm_fn[smp_processor_id()](dst_ptr,
				dst_begin, dst_end, src_ptr, src_end, dip);
	}
	kernel_neon_end();
	return (ssize_t)ret;
}

#define __ARCH_HAS_LZ4_ACCELERATOR

#else

static inline int lz4_decompress_accel_enable(void)
{
	return	0;
}

static inline ssize_t lz4_decompress_asm(
	uint8_t **dst_ptr, uint8_t *dst_begin, uint8_t *dst_end,
	const uint8_t **src_ptr, const uint8_t *src_end, bool dip)
{
	return 0;
}
#endif
