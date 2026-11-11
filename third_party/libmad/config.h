/* Linnet freestanding configuration: fixed point, single CPU, caller storage. */
#ifndef LINNET_LIBMAD_CONFIG_H
#define LINNET_LIBMAD_CONFIG_H
#define HAVE_CONFIG_H 1
#define NDEBUG 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define MEM_ALIGN_ATTR __attribute__((aligned(4)))
#define ICODE_ATTR
#define ICONST_ATTR
#define IBSS_ATTR
#if defined(__arm__) && !defined(__thumb__)
/* ARMv4 SMULL/SMLAL keeps full precision across dot-product accumulation. */
#define FPM_ARM
#else
#define FPM_64BIT
#endif
#endif
