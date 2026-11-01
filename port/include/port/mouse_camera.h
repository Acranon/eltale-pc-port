/* The mouse camera (port/p0/mouse_camera.c, docs/CONTROLS.md): the player turns and zooms the view with the
 * mouse (or the camera keys) in play, in every camera mode but the scripted ones.
 *
 * The game's follow camera keeps its own eye and velocity from tick to tick, so the view is not written
 * into its state for good: Camera_Update calls Port_MouseCameraApply after the mode handler, which puts
 * the turned eye (and the yaw it looks along) in camera_state for the tick's drawing, so the matrices,
 * billboards, the compass and the native renderer's recording all see the view. The end of
 * Gfx_BuildFrameDisplayList calls Port_MouseCameraRestore, which puts the game's own eye and yaw back
 * before any game logic runs, so the game's camera, battles and quick saves only ever see the game's.
 *
 * With no mouse or camera key input nothing here changes anything: the game runs byte for byte as it
 * did. */
#ifndef ELTALE_PORT_MOUSE_CAMERA_H
#define ELTALE_PORT_MOUSE_CAMERA_H

/* The camera input of this tick (port/os/controller.c: the host's, or an input script's `mouse` rows):
 * turn (dx right, dy down, in mouse counts at the default sensitivity), wheel steps (+ zooms in) and
 * flags (PORT_CAMERA_*). Each controller read replaces it; the camera takes it once. */
enum { PORT_CAMERA_RESET = 1 };
void Port_CameraInputSet(float dx, float dy, int wheel, int flags);

/* Camera_Update, after the mode handler: the view for this tick's drawing. */
void Port_MouseCameraApply(void);
/* The end of Gfx_BuildFrameDisplayList: the game's own eye and yaw back. */
void Port_MouseCameraRestore(void);

/* Player_StateMove: what to add to camera_state.followYaw so the stick moves the player relative to
 * the view (0 while the view is the game's). */
float Port_MouseCameraMoveYaw(void);
/* The view is turned or zoomed away from the game's this tick (nd_camera's EL_RDCAM_ORBIT; the
 * billboard cone is opened) */
int Port_MouseCameraActive(void);

#endif
