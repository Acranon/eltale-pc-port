#ifndef ELTALE_PORT_CONTROLLER_H
#define ELTALE_PORT_CONTROLLER_H

/* Input script rows: VI index, held buttons, signed stick X, signed stick Y.
 * State holds until the next row. Lines beginning with # are ignored. `repeat FROM PERIOD` plays the rows
 * from VI FROM again every PERIOD VIs. `mouse VI DX DY WHEEL FLAGS` rows give the mouse camera's input
 * (port/mouse_camera.h) once, on the read at that VI (docs/CONTROLS.md). */
int Port_ControllerLoadScript(const char *path);

/* Hold Start on the first controller read, so the game opens its Controller Pak manager. */
void Port_ControllerHoldStartAtBoot(void);

#endif
