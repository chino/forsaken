/*
  OpenXR head mounted display support.

  Everything here only exists in builds made with `make VR=1` (which defines
  VR_OPENXR), and does nothing at run time unless the game is started with
  -vr. When the OpenXR runtime or headset is missing, vr_init() logs why and
  the game carries on rendering to the desktop window as usual.

  The design, in short:

  - The game keeps rendering in its usual "logical" screen space (the window
    size in render_info.ThisMode). While a VR frame is in flight everything is
    drawn into offscreen framebuffers instead of the window, and the GL
    renderer maps the logical viewport onto whichever framebuffer is bound
    (see vr_scale_rect()).

  - The 3D world is rendered once per eye (RenderCurrentCameraInVR() in
    oct2.c) with the eye's pose from the runtime composed with the ship's
    orientation, and the runtime's asymmetric projection.

  - Everything 2D (HUD, menus, loading screens) lands on a "panel" that is
    submitted as an OpenXR quad layer: head locked as a HUD while flying, or
    fixed in the room for menus that have no 3D scene behind them.

  - The desktop window mirrors the left eye with the panel laid over it.

  - The controllers appear to the game as an extra joystick, so they show up
    in the existing joystick configuration menus and are saved with the pilot.

  The OpenXR plumbing (instance, session, swapchains, frame loop and actions)
  is based on the OpenXR port in Forsaken VR (GameOrDie007/Forsaken-VR),
  a fork of ForsakenX released under the same license.
*/

#ifndef VR_INCLUDED
#define VR_INCLUDED

#ifdef VR_OPENXR

#include "main.h"

#define VR_EYES 2

// one eye's view this frame, in OpenXR's right handed convention:
// position in metres, orientation as a quaternion (x, y, z, w) and the
// tangents of the field of view half angles (left and down are negative)
typedef struct {
	float pos[3];
	float quat[4];
	float tan_left, tan_right, tan_up, tan_down;
} vr_view_t;

// -vr was passed
extern bool vr_requested;

// world units per metre: how big the world feels. -vrscale:N
extern float vr_world_scale;

// percentage of the runtime's recommended eye resolution. -vrres:N
extern int vr_res_scale;

// bring up OpenXR. call once the GL context is current.
// returns false (having logged why) when the game should stay on the desktop.
bool vr_init( void );
void vr_shutdown( void );

// a session exists (it may not be running at the moment)
bool vr_active( void );

// around RenderScene(): wait for the runtime, locate the eyes and redirect
// drawing; then submit the layers and draw the desktop mirror
void vr_begin_frame( void );
void vr_end_frame( void );

// the eyes were located this frame and should be rendered
bool vr_views_valid( void );
const vr_view_t * vr_view( int eye );

// bracket the rendering of one eye
void vr_eye_begin( int eye );
void vr_eye_end( int eye );

// map a viewport in logical (window) coordinates onto the framebuffer that
// is currently bound; *surface_h is the height used to flip y for GL.
// vr_unscale_rect() is the inverse.
void vr_scale_rect( int * x, int * y, int * w, int * h, int * surface_h );
void vr_unscale_rect( int * x, int * y, int * w, int * h, int * surface_h );

// height of the framebuffer currently bound for drawing
int vr_surface_height( void );

// controllers, presented to the game as one joystick
#define VR_JOYSTICK_NAME "OpenXR Controllers"

enum {
	VR_AXIS_LEFT_X,		// left thumbstick, right is positive
	VR_AXIS_LEFT_Y,		// left thumbstick, down (pulled back) is positive
	VR_AXIS_RIGHT_X,
	VR_AXIS_RIGHT_Y,
	VR_AXIS_LEFT_TRIGGER,	// 0 to 1
	VR_AXIS_RIGHT_TRIGGER,
	VR_AXES
};

enum {
	VR_BUTTON_RIGHT_TRIGGER,
	VR_BUTTON_LEFT_TRIGGER,
	VR_BUTTON_RIGHT_GRIP,
	VR_BUTTON_LEFT_GRIP,
	VR_BUTTON_A,
	VR_BUTTON_B,
	VR_BUTTON_X,
	VR_BUTTON_Y,
	VR_BUTTON_LEFT_STICK,
	VR_BUTTON_RIGHT_STICK,
	VR_BUTTON_MENU,
	VR_BUTTONS
};

extern const char * vr_axis_names[ VR_AXES ];
extern const char * vr_button_names[ VR_BUTTONS ];

typedef struct {
	float axis[ VR_AXES ];		// -1 to 1, triggers 0 to 1
	bool  button[ VR_BUTTONS ];
} vr_controllers_t;

// read the controllers; false when there is nothing to read
// (no session, or the application does not have input focus)
bool vr_read_controllers( vr_controllers_t * out );

// recentre the view on where the player is looking now
void vr_recenter( void );

#endif // VR_OPENXR

#endif // VR_INCLUDED
