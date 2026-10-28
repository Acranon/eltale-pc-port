#ifndef ELTALE_PORT_ULTRA64_H
#define ELTALE_PORT_ULTRA64_H

/* Use the pinned SDK declarations with host-width scalar types. The SDK's
 * standard-header lookalikes are searched after libc (see the build flags). */
/* The SDK declares bcopy/bcmp with an N64 int length; libc uses size_t. */
#define bcopy n64_bcopy
#define bcmp n64_bcmp
#include "../../decomp/include/ultra64/ultra64.h"
#undef bcopy
#undef bcmp

#endif
