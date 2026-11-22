/*
  OpenXR headset support, built in when the OpenXR headers are found and enabled with -vr.
  Falls back to the desktop when no runtime or headset is available.
  See docs/VR.md. Based on Forsaken VR (GameOrDie007/Forsaken-VR).
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

// -vr was passed or VR = true is in the config
extern bool vr_requested;

// world units per metre: how big the world feels. -vrscale:N
extern float vr_world_scale;

// percentage of the runtime's recommended eye resolution. -vrres:N
extern int vr_res_scale;

// comfort: the controllers' turn and pitch rate as a percentage of the
// game's (VRTurnSpeed), ease the ship back level when not rolling even if
// the pilot's autoleveling is off (VRAutoLevel) and the idle drift of our
// own ship (VRShipBob)
extern int vr_turn_speed;
extern bool vr_auto_level;
extern bool vr_ship_bob;

// the HUD stays put in the cockpit instead of following the head (VRHudCockpit)
extern bool vr_hud_cockpit;

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
