/*
 * Moonlight webOS - Compatibility definitions
 *
 * Provides byteswap macros for old GCC 4.5.2
 */

#ifndef WEBOS_COMPAT_H
#define WEBOS_COMPAT_H

#include <stdint.h>

/* GCC 4.5.2 doesn't have __builtin_bswap16/32/64, define them manually */
#ifndef BSWAP16
#define BSWAP16(x) \
    ((uint16_t)((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8)))
#endif

#ifndef BSWAP32
#define BSWAP32(x) \
    ((uint32_t)((((x) & 0xff000000) >> 24) | \
                (((x) & 0x00ff0000) >> 8)  | \
                (((x) & 0x0000ff00) << 8)  | \
                (((x) & 0x000000ff) << 24)))
#endif

#ifndef BSWAP64
#define BSWAP64(x) \
    ((uint64_t)BSWAP32((uint32_t)(x)) << 32 | BSWAP32((uint32_t)((x) >> 32)))
#endif

#endif /* WEBOS_COMPAT_H */
