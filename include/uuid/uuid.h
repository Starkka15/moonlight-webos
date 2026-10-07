/*
 * Moonlight webOS - minimal stand-in for libuuid
 *
 * libgamestream only needs random (version 4) UUIDs as text, and webOS
 * has no libuuid, so this provides just those two calls.
 */

#ifndef WEBOS_UUID_H
#define WEBOS_UUID_H

#include <stdio.h>
#include <stdlib.h>

#define UUID_STR_LEN 37
#ifndef UUID_STRLEN
#define UUID_STRLEN 37
#endif

typedef unsigned char uuid_t[16];

static inline void uuid_generate_random(uuid_t out)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    int i;

    if (f != NULL) {
        got = fread(out, 1, 16, f);
        fclose(f);
    }
    for (i = (int) got; i < 16; i++) {
        out[i] = (unsigned char) rand();
    }

    out[6] = (out[6] & 0x0f) | 0x40; /* version 4 */
    out[8] = (out[8] & 0x3f) | 0x80; /* RFC 4122 variant */
}

static inline void uuid_unparse(const uuid_t uu, char *out)
{
    sprintf(out,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7],
            uu[8], uu[9], uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}

#endif /* WEBOS_UUID_H */
