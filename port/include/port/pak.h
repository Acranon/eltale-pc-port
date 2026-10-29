#ifndef ELTALE_PORT_PAK_H
#define ELTALE_PORT_PAK_H

/* NULL selects a volatile, formatted Controller Pak. An existing image must
 * be exactly 32768 bytes; a missing image is formatted on first use. */
int Port_PakLoad(const char *path);

#endif
