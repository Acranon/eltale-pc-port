#ifndef ELTALE_PORT_CONTROLLER_H
#define ELTALE_PORT_CONTROLLER_H

/* Input script rows: VI index, held buttons, signed stick X, signed stick Y.
 * State holds until the next row. Lines beginning with # are ignored. */
int Port_ControllerLoadScript(const char *path);

#endif
