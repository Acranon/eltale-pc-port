/* The mouse camera (port/include/port/mouse_camera.h; docs/CONTROLS.md, "Mouse camera").
 *
 * Hold the right mouse button and drag (or turn on mouse look, or hold the camera keys) to turn the view
 * around the point the camera looks at; the wheel zooms; the middle button (or the reset key) gives the
 * view back to the game. Modelled on Quest 64 Enhanced's (the same engine):
 * - once turned, the view's yaw and pitch are the player's own (absolute) until reset, while the game's
 *   follow camera keeps following underneath;
 * - the stick moves the player relative to the view (Player_StateMove adds Port_MouseCameraMoveYaw);
 * - the eye stays above the floor (under the eye, else under the target) and, on maps with a sky
 *   (outdoors), in front of the first wall between the target and the eye; never closer than
 *   CAMERA_MIN_DISTANCE; an eye on its target falls back to the game's;
 * - billboards are culled by the view's yaw and, while the view is the player's, not by direction at all
 *   (game/native_draw/native_draw.c nd_billboard_cone), since a pitched view sees past the game's cone;
 * - only in play (core_game_mode 1), never in a scripted camera (mode 3: scenes place it), nor in a
 *   battle's or a conversation's focus view (flags 0x40).
 * Everything here acts in camera_state between Camera_Update's mode handler and the end of the tick's
 * drawing, then is undone, so the game's logic and its quick saves never see the turned eye. */
#include "common.h"
#include "game/camera/camera.h"
#include "game/core/main.h"
#include "game/map/collision.h"
#include "game/map/map.h"
#include "game/math/math.h"
#include "game/input/input.h"
#include "port/mouse_camera.h"
#include "port/spell_aim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CAM_MODE_SCRIPTED 3
#define CAM_MODE_COUNT 6
#define CAM_FLAG_FOCUS 0x40 /* a battle focus or a conversation's view (Camera_StartBattleFocus, Camera_SetTargetView) */
#define MAP_HAS_SKY 4       /* map_env_flags: the backdrop is drawn (outdoors) */

#define ORBIT_YAW_PER_COUNT 0.006f
#define ORBIT_PITCH_PER_COUNT 0.004f
#define ORBIT_PITCH_MIN -0.35f
#define ORBIT_PITCH_MAX 1.4f
#define ZOOM_STEP 1.12f
#define ZOOM_MIN 0.35f
#define ZOOM_MAX 2.5f
#define PI_F 3.14159265f
#define CAMERA_FLOOR_MARGIN 8.0f
#define CAMERA_WALL_STEPS 12
#define CAMERA_WALL_RADIUS 6.0f
#define CAMERA_MIN_DISTANCE 40.0f
#define COLL_NO_FLOOR 0x8000
#define COLL_CAMERA_FLOORS 0x600 /* the game camera's flags (0x602) without the wall push */
#define COLL_WALLS 0x601         /* floors and the walls the player collides with */
/* Click to aim spells (port/spell_aim.h): while aiming the eye rises to this pitch over the point looked at, its
 * height only (x, z and the yaw stay the game's: the map's grid search, the emitters and the billboards' yaw see the
 * same), eased in and out over LIFT_TICKS; indoors less (the ceilings) */
#define AIM_PITCH 0.82f
#define AIM_PITCH_INDOORS 0.66f
#define AIM_AHEAD 32.0f /* the point looked at moves this far ahead (the monsters stay in view) */
#define LIFT_TICKS 8

/* this tick's input (Port_CameraInputSet) */
static float in_dx, in_dy;
static int in_wheel, in_flags;

/* the player's view: absolute yaw and pitch once turned; the zoom on the game's distance */
static int free_camera;
static float free_yaw, free_pitch;
static float orbit_zoom = 1.0f;

/* this tick: the view is applied; the turn from the game's yaw (for movement) */
static int view_active;
/* the view's turn from the game's yaw, for the stick (from one Camera_Update to the next) */
static float move_yaw_offset;
/* what was replaced, to put back (and what was written, to see whether something else moved it since) */
static int replaced;
static float game_eye[3], view_eye[3];
static float game_yaw, view_yaw_written;
/* the aim's raise: 0 the game's height, 1 raised (port/spell_aim.h); the point looked at moved ahead for it (x, z) */
static float lift_t;
static int at_moved;
static float game_at[2], view_at[2];

void Port_CameraInputSet(float dx, float dy, int wheel, int flags) {
    in_dx = dx;
    in_dy = dy;
    in_wheel = wheel;
    in_flags = flags;
}

static float wrap_angle(float a) {
    while (a > PI_F) a -= 2.0f * PI_F;
    while (a < -PI_F) a += 2.0f * PI_F;
    return a;
}

/* Coll_ResolveFloorAndWalls, only where its grid lookup stays inside the grid (it reads the cell before it
 * tests the bounds; the turned eye can be far outside) */
static int query(float x, float y, float z, u16 flags, float radius, CollResult *out) {
    MapGrid *g = map_subarea_grid;
    if (!g) return 0;
    if (g->searchPattern != 0) {
        s32 ix = (x - g->originX) / g->cellW, iz = (z - g->originZ) / g->cellD;
        if (ix < 0 || iz < 0 || ix >= g->w || iz >= g->h) return 0;
    }
    out->x = radius;
    Coll_ResolveFloorAndWalls(x, y, z, flags, out);
    return 1;
}

static int floor_height(float x, float y, float z, float *floor_y) {
    CollResult r;
    if (!query(x, y, z, COLL_CAMERA_FLOORS, 4.0f, &r) || (r.flags & COLL_NO_FLOOR)) return 0;
    *floor_y = r.y;
    return 1;
}

/* Outdoors: the eye in front of the first wall between the target and the eye. Indoors the view may pass
 * through walls (their halls are too tight for a camera that collides). */
static void keep_eye_out_of_walls(Camera *cam) {
    float dir[3], len, free_t = 0.0f;
    int i;

    if (!(map_env_flags & MAP_HAS_SKY)) return;
    dir[0] = cam->eyeX - cam->atX;
    dir[1] = cam->eyeY - cam->atY;
    dir[2] = cam->eyeZ - cam->atZ;
    len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < CAMERA_MIN_DISTANCE) return;
    for (i = 1; i <= CAMERA_WALL_STEPS; i++) {
        float t = (float)i / CAMERA_WALL_STEPS, p[3], dx, dz;
        CollResult r;
        p[0] = cam->atX + dir[0] * t;
        p[1] = cam->atY + dir[1] * t;
        p[2] = cam->atZ + dir[2] * t;
        if (!query(p[0], p[1], p[2], COLL_WALLS, CAMERA_WALL_RADIUS, &r)) break; /* off the grid: stop here */
        dx = r.x - p[0];
        dz = r.z - p[2];
        if (dx * dx + dz * dz > 0.01f) {
            /* a wall: the last clear point, but not nearer than the minimum (a view inside the player is worse
             * than one through a wall) */
            float min_t = CAMERA_MIN_DISTANCE / len;
            t = free_t > min_t ? free_t : min_t;
            cam->eyeX = cam->atX + dir[0] * t;
            cam->eyeY = cam->atY + dir[1] * t;
            cam->eyeZ = cam->atZ + dir[2] * t;
            return;
        }
        free_t = t;
    }
}

/* The eye above the floor under it, or (no floor there: past a cliff, through a wall) above the target's */
static void keep_eye_above_floor(Camera *cam) {
    float probe_y = cam->eyeY > cam->atY ? cam->eyeY : cam->atY, floor_y;
    if (!floor_height(cam->eyeX, probe_y, cam->eyeZ, &floor_y) && !floor_height(cam->atX, cam->atY, cam->atZ, &floor_y))
        return;
    if (cam->eyeY < floor_y + CAMERA_FLOOR_MARGIN) cam->eyeY = floor_y + CAMERA_FLOOR_MARGIN;
}

void Port_MouseCameraRestore(void) {
    Camera *cam = &camera_state;
    Port_SpellAimViewDrawn(); /* (the view this picture was drawn with: the aim's pointer) */
    view_active = 0;
    if (at_moved && cam->atX == view_at[0] && cam->atZ == view_at[1]) cam->atX = game_at[0], cam->atZ = game_at[1];
    at_moved = 0;
    if (!replaced) return;
    replaced = 0;
    /* something else moved the eye since (a scene): keep that */
    if (cam->eyeX != view_eye[0] || cam->eyeY != view_eye[1] || cam->eyeZ != view_eye[2]) return;
    cam->eyeX = game_eye[0];
    cam->eyeY = game_eye[1];
    cam->eyeZ = game_eye[2];
    if (cam->yaw == view_yaw_written) cam->yaw = game_yaw;
}

static void aim_lift(Camera *cam);
static void mouse_view(Camera *cam);

void Port_MouseCameraApply(void) {
    Camera *cam = &camera_state;
    view_active = 0;
    move_yaw_offset = 0.0f;
    /* the aim's raise, eased (a tick at a time; never in the scenes the game films, nor outside play) */
    if (Port_SpellAimLiftWanted()) lift_t += 1.0f / LIFT_TICKS;
    else lift_t -= 1.0f / LIFT_TICKS;
    if (lift_t < 0.0f) lift_t = 0.0f;
    if (lift_t > 1.0f) lift_t = 1.0f;
    /* (the cast's focus view, Camera_StartBattleFocus, takes the camera at once: the raise goes with it, a cut, rather
     * than easing down over a camera the game is moving itself, which the pictures between ticks blend badly) */
    if (core_game_mode != 1 || cam->mode == CAM_MODE_SCRIPTED || cam->mode < 0 || cam->mode >= CAM_MODE_COUNT ||
        (cam->flags & CAM_FLAG_FOCUS))
        lift_t = 0.0f;
    mouse_view(cam);
    aim_lift(cam);
}

static void mouse_view(Camera *cam) {
    float dx = in_dx, dy = in_dy, vx, vy, vz, h, dist, yaw0, view_yaw;
    int wheel = in_wheel, flags = in_flags;

    in_dx = in_dy = 0.0f;
    in_wheel = in_flags = 0;
    if (core_game_mode != 1 || cam->mode == CAM_MODE_SCRIPTED || cam->mode < 0 || cam->mode >= CAM_MODE_COUNT ||
        (cam->flags & CAM_FLAG_FOCUS))
        return;

    vx = cam->eyeX - cam->atX;
    vy = cam->eyeY - cam->atY;
    vz = cam->eyeZ - cam->atZ;
    h = sqrtf(vx * vx + vz * vz);
    dist = sqrtf(h * h + vy * vy);
    yaw0 = Math_Atan2(vx, vz);

    if (flags & PORT_CAMERA_RESET) {
        free_camera = 0;
        orbit_zoom = 1.0f;
    }
    if ((dx != 0.0f || dy != 0.0f) && !free_camera) {
        free_camera = 1;
        free_yaw = yaw0;
        free_pitch = Math_Atan2(vy, h);
    }
    if (free_camera) {
        free_yaw = wrap_angle(free_yaw - dx * ORBIT_YAW_PER_COUNT);
        free_pitch += dy * ORBIT_PITCH_PER_COUNT;
        if (free_pitch < ORBIT_PITCH_MIN) free_pitch = ORBIT_PITCH_MIN;
        else if (free_pitch > ORBIT_PITCH_MAX) free_pitch = ORBIT_PITCH_MAX;
    }
    for (; wheel > 0; wheel--) orbit_zoom /= ZOOM_STEP;
    for (; wheel < 0; wheel++) orbit_zoom *= ZOOM_STEP;
    if (orbit_zoom < ZOOM_MIN) orbit_zoom = ZOOM_MIN;
    else if (orbit_zoom > ZOOM_MAX) orbit_zoom = ZOOM_MAX;

    if (!free_camera && orbit_zoom == 1.0f) return;
    if (dist < 1.0f) return; /* no direction to turn */

    dist *= orbit_zoom;
    if (free_camera) {
        view_yaw = free_yaw;
        h = dist * cosf(free_pitch);
        vy = dist * sinf(free_pitch);
        vx = h * sinf(free_yaw);
        vz = h * cosf(free_yaw);
    } else {
        view_yaw = yaw0;
        vx *= orbit_zoom;
        vy *= orbit_zoom;
        vz *= orbit_zoom;
    }

    game_eye[0] = cam->eyeX;
    game_eye[1] = cam->eyeY;
    game_eye[2] = cam->eyeZ;
    cam->eyeX = cam->atX + vx;
    cam->eyeY = cam->atY + vy;
    cam->eyeZ = cam->atZ + vz;
    keep_eye_out_of_walls(cam);
    keep_eye_above_floor(cam);

    /* an eye on its target makes no view: the game's */
    vx = cam->eyeX - cam->atX;
    vy = cam->eyeY - cam->atY;
    vz = cam->eyeZ - cam->atZ;
    if (vx * vx + vy * vy + vz * vz < 1.0f || (vx == 0.0f && vz == 0.0f)) {
        cam->eyeX = game_eye[0];
        cam->eyeY = game_eye[1];
        cam->eyeZ = game_eye[2];
        return;
    }
    view_eye[0] = cam->eyeX;
    view_eye[1] = cam->eyeY;
    view_eye[2] = cam->eyeZ;
    /* the yaw billboards face, the compass turns by and objects are culled against: the view's (camera_state's
     * yaw is the game's eye-to-target heading, so turned by as much as the view turned) */
    game_yaw = cam->yaw;
    view_yaw_written = wrap_angle(game_yaw + Math_Atan2(vx, vz) - yaw0);
    cam->yaw = view_yaw_written;
    replaced = 1;
    view_active = 1;
    move_yaw_offset = free_camera ? wrap_angle(view_yaw - yaw0) : 0.0f;
    {
        static int log = -1;
        if (log < 0) log = getenv("ELTALE_MOUSE_LOG") != NULL;
        if (log)
            fprintf(stderr, "P1_MOUSE_CAMERA free=%d yaw=%.3f pitch=%.3f zoom=%.2f game_yaw=%.3f stick=%d,%d eye=%.1f,%.1f,%.1f at=%.1f,%.1f,%.1f\n",
                    free_camera, free_yaw, free_pitch, orbit_zoom, yaw0, input_stick_x, input_stick_y, cam->eyeX, cam->eyeY, cam->eyeZ, cam->atX, cam->atY,
                    cam->atZ);
    }
}

/* While aiming a spell: the eye raised (its height only) and the point looked at moved ahead along the view (x, z), so
 * the view looks down on the ground ahead at a steeper pitch; after the mouse view if any. The yaw and the eye's x and z
 * stay as they are: what the game reads of the camera in a tick (the map's grid search round the eye, the emitters'
 * half-plane, the billboards' yaw) sees the same, so this changes only the picture. */
static void aim_lift(Camera *cam) {
    const float e = lift_t * lift_t * (3.0f - 2.0f * lift_t);
    float vx, vy, vz, h, pitch, target, ahead;
    if (e <= 0.0f) return;
    vx = cam->eyeX - cam->atX;
    vy = cam->eyeY - cam->atY;
    vz = cam->eyeZ - cam->atZ;
    h = sqrtf(vx * vx + vz * vz);
    if (h < 1.0f) return;
    pitch = atan2f(vy, h);
    target = (map_env_flags & MAP_HAS_SKY) ? AIM_PITCH : AIM_PITCH_INDOORS;
    if (pitch >= target) return;
    if (!replaced) {
        game_eye[0] = cam->eyeX;
        game_eye[1] = cam->eyeY;
        game_eye[2] = cam->eyeZ;
        game_yaw = view_yaw_written = cam->yaw;
        replaced = 1;
    }
    ahead = AIM_AHEAD * e;
    game_at[0] = cam->atX, game_at[1] = cam->atZ;
    cam->atX -= vx / h * ahead;
    cam->atZ -= vz / h * ahead;
    view_at[0] = cam->atX, view_at[1] = cam->atZ;
    at_moved = 1;
    cam->eyeY = cam->atY + (h + ahead) * tanf(pitch + (target - pitch) * e);
    view_eye[0] = cam->eyeX;
    view_eye[1] = cam->eyeY;
    view_eye[2] = cam->eyeZ;
    view_active = 1;
}

float Port_MouseCameraMoveYaw(void) { return move_yaw_offset; }

int Port_MouseCameraActive(void) { return view_active; }
