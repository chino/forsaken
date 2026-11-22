/*
  OpenXR head mounted display support. See vr.h for the overview.

  Based on the OpenXR port in Forsaken VR (GameOrDie007/Forsaken-VR),
  source/vr_openxr.c, a fork of ForsakenX under the same license.

  Only compiled in builds made with VR (see the Makefile). The OpenXR
  loader is opened at run time, so it is only needed with -vr.
*/

#ifdef VR_OPENXR

#include "render_gl_shared.h"
#include "vr.h"

#include <string.h>
#include <math.h>
#include <SDL.h>

#if defined( WIN32 ) || defined( _WIN32 )
  #include <windows.h>
  #define XR_USE_PLATFORM_WIN32
#elif !defined( MACOSX )
  #include <X11/Xlib.h>
  #include <GL/glx.h>
  #define XR_USE_PLATFORM_XLIB
#endif
#if !defined( WIN32 ) && !defined( _WIN32 )
  #include <dlfcn.h>
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// OpenXR structures are set up as { XR_TYPE_... } with the rest zeroed,
// which is what the specification's own examples do
#if defined( __GNUC__ )
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

/*===================================================================
  Settings
===================================================================*/

bool  vr_requested   = false;
float vr_world_scale = 30.0f;	// see -vrscale in main.c
int   vr_res_scale   = 100;		// see -vrres in main.c
int   vr_turn_speed  = 100;
bool  vr_auto_level  = false;
bool  vr_ship_bob    = true;
bool  vr_hud_cockpit = true;
bool  vr_vibration   = true;
bool  vr_steady_mirror = true;

// the panel the HUD and menus are drawn on, in metres
#define PANEL_WIDTH	2.0f
#define PANEL_DIST	1.75f

// hold the menu button this long to recentre (nanoseconds)
#define RECENTER_HOLD	600000000LL

const char * vr_axis_names[ VR_AXES ] = {
	"Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y",
	"Left Trigger", "Right Trigger"
};

const char * vr_button_names[ VR_BUTTONS ] = {
	"Right Trigger", "Left Trigger", "Right Grip", "Left Grip",
	"A", "B", "X", "Y", "Left Stick", "Right Stick", "Menu"
};

/*===================================================================
  GL entry points beyond 1.1, fetched at run time so this also works
  where the GL library only exports 1.1 (windows)
===================================================================*/

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                  0x8D40
#define GL_READ_FRAMEBUFFER             0x8CA8
#define GL_DRAW_FRAMEBUFFER             0x8CA9
#define GL_RENDERBUFFER                 0x8D41
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8             0x88F0
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8                 0x8C43
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                0x812F
#endif

static void   (APIENTRY *vgl_GenFramebuffers)( GLsizei, GLuint * );
static void   (APIENTRY *vgl_DeleteFramebuffers)( GLsizei, const GLuint * );
static void   (APIENTRY *vgl_BindFramebuffer)( GLenum, GLuint );
static void   (APIENTRY *vgl_FramebufferTexture2D)( GLenum, GLenum, GLenum, GLuint, GLint );
static void   (APIENTRY *vgl_FramebufferRenderbuffer)( GLenum, GLenum, GLenum, GLuint );
static GLenum (APIENTRY *vgl_CheckFramebufferStatus)( GLenum );
static void   (APIENTRY *vgl_GenRenderbuffers)( GLsizei, GLuint * );
static void   (APIENTRY *vgl_DeleteRenderbuffers)( GLsizei, const GLuint * );
static void   (APIENTRY *vgl_BindRenderbuffer)( GLenum, GLuint );
static void   (APIENTRY *vgl_RenderbufferStorage)( GLenum, GLenum, GLsizei, GLsizei );
static void   (APIENTRY *vgl_BlitFramebuffer)( GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum );
#if GL > 1
static void   (APIENTRY *vgl_UseProgram)( GLuint );
#endif

static bool load_gl( void )
{
#define LOAD( var, name ) \
	if( !( *(void **) &var = SDL_GL_GetProcAddress( name ) ) ) \
	{ \
		Msg( "openxr: OpenGL function %s is missing\n", name ); \
		return false; \
	}
	LOAD( vgl_GenFramebuffers,         "glGenFramebuffers" );
	LOAD( vgl_DeleteFramebuffers,      "glDeleteFramebuffers" );
	LOAD( vgl_BindFramebuffer,         "glBindFramebuffer" );
	LOAD( vgl_FramebufferTexture2D,    "glFramebufferTexture2D" );
	LOAD( vgl_FramebufferRenderbuffer, "glFramebufferRenderbuffer" );
	LOAD( vgl_CheckFramebufferStatus,  "glCheckFramebufferStatus" );
	LOAD( vgl_GenRenderbuffers,        "glGenRenderbuffers" );
	LOAD( vgl_DeleteRenderbuffers,     "glDeleteRenderbuffers" );
	LOAD( vgl_BindRenderbuffer,        "glBindRenderbuffer" );
	LOAD( vgl_RenderbufferStorage,     "glRenderbufferStorage" );
	LOAD( vgl_BlitFramebuffer,         "glBlitFramebuffer" );
#if GL > 1
	LOAD( vgl_UseProgram,              "glUseProgram" );
#endif
#undef LOAD
	return true;
}

/*===================================================================
  Offscreen targets
===================================================================*/

typedef struct {
	GLuint fbo, color, depth;
	int w, h;
} target_t;

static target_t eye_target;	// both eyes render here in turn, left last
static target_t panel_target;	// HUD, menus and everything else 2D
static GLuint   copy_fbo;	// wraps a swapchain image as a blit target

// size of the framebuffer currently bound for drawing; 0 = the window
static int surface_w = 0, surface_h = 0;

static void target_free( target_t * t )
{
	if( t->fbo )   vgl_DeleteFramebuffers( 1, &t->fbo );
	if( t->depth ) vgl_DeleteRenderbuffers( 1, &t->depth );
	if( t->color ) glDeleteTextures( 1, &t->color );
	memset( t, 0, sizeof( *t ) );
}

static bool target_create( target_t * t, int w, int h )
{
	GLenum status;

	memset( t, 0, sizeof( *t ) );
	t->w = w;
	t->h = h;

	glGenTextures( 1, &t->color );
	glBindTexture( GL_TEXTURE_2D, t->color );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glBindTexture( GL_TEXTURE_2D, 0 );

	vgl_GenRenderbuffers( 1, &t->depth );
	vgl_BindRenderbuffer( GL_RENDERBUFFER, t->depth );
	vgl_RenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h );
	vgl_BindRenderbuffer( GL_RENDERBUFFER, 0 );

	vgl_GenFramebuffers( 1, &t->fbo );
	vgl_BindFramebuffer( GL_FRAMEBUFFER, t->fbo );
	vgl_FramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0 );
	vgl_FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth );
	status = vgl_CheckFramebufferStatus( GL_FRAMEBUFFER );
	vgl_BindFramebuffer( GL_FRAMEBUFFER, 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		Msg( "openxr: offscreen target %dx%d incomplete (0x%x)\n", w, h, (unsigned) status );
		target_free( t );
		return false;
	}
	return true;
}

// Bind a framebuffer for drawing and keep the GL viewport covering the same
// logical rectangle it did on the previous one, so code that relies on the
// current viewport carries on unaware of the switch.
static void bind_target( GLuint fbo, int w, int h )
{
	GLint vp[4];
	int x, y, vw, vh, sh;

	glGetIntegerv( GL_VIEWPORT, vp );
	x = vp[0]; vw = vp[2]; vh = vp[3];
	sh = surface_h ? surface_h : render_info.ThisMode.h;
	y = sh - ( vp[1] + vh );
	vr_unscale_rect( &x, &y, &vw, &vh, &sh );

	vgl_BindFramebuffer( GL_FRAMEBUFFER, fbo );
	surface_w = w;
	surface_h = h;

	vr_scale_rect( &x, &y, &vw, &vh, &sh );
	glViewport( x, sh - ( y + vh ), vw, vh );
}

int vr_surface_height( void )
{
	return surface_h ? surface_h : render_info.ThisMode.h;
}

void vr_scale_rect( int * x, int * y, int * w, int * h, int * sh )
{
	float sx, sy;
	int x2, y2;

	if( !surface_w || !surface_h ||
	    render_info.ThisMode.w <= 0 || render_info.ThisMode.h <= 0 )
		return;

	sx = (float) surface_w / (float) render_info.ThisMode.w;
	sy = (float) surface_h / (float) render_info.ThisMode.h;
	x2 = (int) floorf( ( *x + *w ) * sx + 0.5f );
	y2 = (int) floorf( ( *y + *h ) * sy + 0.5f );
	*x = (int) floorf( *x * sx + 0.5f );
	*y = (int) floorf( *y * sy + 0.5f );
	*w = x2 - *x;
	*h = y2 - *y;
	*sh = surface_h;
}

void vr_unscale_rect( int * x, int * y, int * w, int * h, int * sh )
{
	float sx, sy;
	int x2, y2;

	if( !surface_w || !surface_h ||
	    render_info.ThisMode.w <= 0 || render_info.ThisMode.h <= 0 )
		return;

	sx = (float) render_info.ThisMode.w / (float) surface_w;
	sy = (float) render_info.ThisMode.h / (float) surface_h;
	x2 = (int) floorf( ( *x + *w ) * sx + 0.5f );
	y2 = (int) floorf( ( *y + *h ) * sy + 0.5f );
	*x = (int) floorf( *x * sx + 0.5f );
	*y = (int) floorf( *y * sy + 0.5f );
	*w = x2 - *x;
	*h = y2 - *y;
	*sh = render_info.ThisMode.h;
}

/*===================================================================
  OpenXR entry points, from the loader opened at run time so a VR
  build still runs where OpenXR is not installed
===================================================================*/

#define XR_GLOBAL_FUNCS \
	X( xrEnumerateInstanceExtensionProperties ) X( xrCreateInstance )

#define XR_INSTANCE_FUNCS \
	X( xrDestroyInstance ) X( xrResultToString ) X( xrGetInstanceProperties ) \
	X( xrGetSystem ) X( xrGetSystemProperties ) X( xrEnumerateViewConfigurationViews ) \
	X( xrCreateSession ) X( xrDestroySession ) X( xrBeginSession ) X( xrEndSession ) \
	X( xrCreateReferenceSpace ) X( xrDestroySpace ) X( xrLocateSpace ) X( xrLocateViews ) \
	X( xrCreateSwapchain ) X( xrDestroySwapchain ) X( xrEnumerateSwapchainFormats ) \
	X( xrEnumerateSwapchainImages ) X( xrAcquireSwapchainImage ) \
	X( xrWaitSwapchainImage ) X( xrReleaseSwapchainImage ) \
	X( xrWaitFrame ) X( xrBeginFrame ) X( xrEndFrame ) X( xrPollEvent ) \
	X( xrStringToPath ) X( xrCreateActionSet ) X( xrDestroyActionSet ) X( xrCreateAction ) \
	X( xrSuggestInteractionProfileBindings ) X( xrAttachSessionActionSets ) \
	X( xrSyncActions ) X( xrGetActionStateFloat ) X( xrGetActionStateBoolean ) \
	X( xrGetActionStateVector2f ) X( xrApplyHapticFeedback )

static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;
#define X( f ) static PFN_##f f;
XR_GLOBAL_FUNCS
XR_INSTANCE_FUNCS
#undef X

#define X( f ) \
	if( XR_FAILED( xrGetInstanceProcAddr( instance, #f, (PFN_xrVoidFunction *) &f ) ) || !f ) \
	{ \
		Msg( "openxr: the loader lacks %s\n", #f ); \
		return false; \
	}

static bool load_xr( XrInstance instance )
{
#if defined( WIN32 ) || defined( _WIN32 )
	#define XR_LOADER "openxr_loader.dll"
	static HMODULE lib = NULL;
	if( !lib && ( lib = LoadLibraryA( XR_LOADER ) ) )
		xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr) GetProcAddress( lib, "xrGetInstanceProcAddr" );
#else
	#define XR_LOADER "libopenxr_loader.so.1"
	static void * lib = NULL;
	if( !lib && ( lib = dlopen( XR_LOADER, RTLD_NOW ) ) )
		*(void **) &xrGetInstanceProcAddr = dlsym( lib, "xrGetInstanceProcAddr" );
#endif
	if( !xrGetInstanceProcAddr )
	{
		Msg( "openxr: could not load the OpenXR loader (%s); is OpenXR installed?\n", XR_LOADER );
		return false;
	}
	if( instance == XR_NULL_HANDLE )
	{
		XR_GLOBAL_FUNCS
	}
	else
	{
		XR_INSTANCE_FUNCS
	}
	return true;
}
#undef X

/*===================================================================
  OpenXR state
===================================================================*/

static XrInstance      xr_instance = XR_NULL_HANDLE;
static XrSystemId      xr_system   = XR_NULL_SYSTEM_ID;
static XrSession       xr_session  = XR_NULL_HANDLE;
static XrSpace         xr_local    = XR_NULL_HANDLE;	// the room, recentred
static XrSpace         xr_head     = XR_NULL_HANDLE;	// the head (VIEW space)
static XrSessionState  xr_state    = XR_SESSION_STATE_UNKNOWN;
static bool            xr_running  = false;		// between xrBeginSession and xrEndSession

typedef struct {
	XrSwapchain                 handle;
	XrSwapchainImageOpenGLKHR * images;
	uint32_t                    count;
	int                         w, h;
} swapchain_t;

static swapchain_t     eye_chain[ VR_EYES ];
static swapchain_t     panel_chain;
static int64_t         chain_format;

static XrViewConfigurationView xr_config_views[ VR_EYES ];

// per frame
static XrFrameState    frame_state;
static bool            frame_begun   = false;
static bool            views_valid   = false;
static XrView          xr_views[ VR_EYES ];
static vr_view_t       views[ VR_EYES ];
static int             eyes_done     = 0;

// the panel's place in the room while there is no 3D scene behind it
static bool            panel_placed  = false;
static XrPosef         panel_pose;

// a recentre was asked for
static bool            want_recenter = true;

static const char * xr_str( XrResult r )
{
	static char buf[ XR_MAX_RESULT_STRING_SIZE ];
	if( xr_instance != XR_NULL_HANDLE && xrResultToString && XR_SUCCEEDED( xrResultToString( xr_instance, r, buf ) ) )
		return buf;
	switch( r )
	{
	case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
	case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
	case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
	case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
	default: break;
	}
	snprintf( buf, sizeof( buf ), "XrResult %d", (int) r );
	return buf;
}

bool vr_active( void )
{
	return xr_session != XR_NULL_HANDLE;
}

bool vr_views_valid( void )
{
	return views_valid;
}

const vr_view_t * vr_view( int eye )
{
	return &views[ eye ];
}

void vr_recenter( void )
{
	want_recenter = true;
}

/*===================================================================
  Controllers
===================================================================*/

enum {
	ACT_MOVE, ACT_LOOK,
	ACT_TRIGGER_L, ACT_TRIGGER_R, ACT_GRIP_L, ACT_GRIP_R,
	ACT_A, ACT_B, ACT_X, ACT_Y, ACT_STICK_L, ACT_STICK_R, ACT_MENU,
	ACT_BUZZ_L, ACT_BUZZ_R,
	ACT_COUNT
};

static const struct { const char * name, * desc; XrActionType type; } action_info[ ACT_COUNT ] = {
	{ "move",          "Move",              XR_ACTION_TYPE_VECTOR2F_INPUT },
	{ "look",          "Turn",              XR_ACTION_TYPE_VECTOR2F_INPUT },
	{ "trigger_left",  "Left trigger",      XR_ACTION_TYPE_FLOAT_INPUT },
	{ "trigger_right", "Right trigger",     XR_ACTION_TYPE_FLOAT_INPUT },
	{ "grip_left",     "Left grip",         XR_ACTION_TYPE_FLOAT_INPUT },
	{ "grip_right",    "Right grip",        XR_ACTION_TYPE_FLOAT_INPUT },
	{ "button_a",      "A",                 XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "button_b",      "B",                 XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "button_x",      "X",                 XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "button_y",      "Y",                 XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "stick_left",    "Left stick click",  XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "stick_right",   "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "menu",          "Menu",              XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "buzz_left",     "Left vibration",    XR_ACTION_TYPE_VIBRATION_OUTPUT },
	{ "buzz_right",    "Right vibration",   XR_ACTION_TYPE_VIBRATION_OUTPUT },
};

typedef struct { int action; const char * path; } binding_t;

#define L "/user/hand/left/input/"
#define R "/user/hand/right/input/"
#define BUZZ { ACT_BUZZ_L, "/user/hand/left/output/haptic" }, { ACT_BUZZ_R, "/user/hand/right/output/haptic" }

static const binding_t bind_touch[] = {
	{ ACT_MOVE, L "thumbstick" }, { ACT_LOOK, R "thumbstick" },
	{ ACT_TRIGGER_L, L "trigger/value" }, { ACT_TRIGGER_R, R "trigger/value" },
	{ ACT_GRIP_L, L "squeeze/value" }, { ACT_GRIP_R, R "squeeze/value" },
	{ ACT_A, R "a/click" }, { ACT_B, R "b/click" },
	{ ACT_X, L "x/click" }, { ACT_Y, L "y/click" },
	{ ACT_STICK_L, L "thumbstick/click" }, { ACT_STICK_R, R "thumbstick/click" },
	{ ACT_MENU, L "menu/click" },
	BUZZ,
	{ -1, NULL }
};

static const binding_t bind_index[] = {
	{ ACT_MOVE, L "thumbstick" }, { ACT_LOOK, R "thumbstick" },
	{ ACT_TRIGGER_L, L "trigger/value" }, { ACT_TRIGGER_R, R "trigger/value" },
	{ ACT_GRIP_L, L "squeeze/value" }, { ACT_GRIP_R, R "squeeze/value" },
	{ ACT_A, R "a/click" }, { ACT_B, R "b/click" },
	{ ACT_X, L "a/click" }, { ACT_MENU, L "b/click" },
	{ ACT_STICK_L, L "thumbstick/click" }, { ACT_STICK_R, R "thumbstick/click" },
	BUZZ,
	{ -1, NULL }
};

static const binding_t bind_wmr[] = {
	{ ACT_MOVE, L "thumbstick" }, { ACT_LOOK, R "thumbstick" },
	{ ACT_TRIGGER_L, L "trigger/value" }, { ACT_TRIGGER_R, R "trigger/value" },
	{ ACT_GRIP_L, L "squeeze/click" }, { ACT_GRIP_R, R "squeeze/click" },
	{ ACT_A, R "trackpad/click" }, { ACT_B, R "menu/click" },
	{ ACT_X, L "trackpad/click" }, { ACT_MENU, L "menu/click" },
	{ ACT_STICK_L, L "thumbstick/click" }, { ACT_STICK_R, R "thumbstick/click" },
	BUZZ,
	{ -1, NULL }
};

static const binding_t bind_vive[] = {
	{ ACT_MOVE, L "trackpad" }, { ACT_LOOK, R "trackpad" },
	{ ACT_TRIGGER_L, L "trigger/value" }, { ACT_TRIGGER_R, R "trigger/value" },
	{ ACT_GRIP_L, L "squeeze/click" }, { ACT_GRIP_R, R "squeeze/click" },
	{ ACT_STICK_L, L "trackpad/click" }, { ACT_STICK_R, R "trackpad/click" },
	{ ACT_B, R "menu/click" }, { ACT_MENU, L "menu/click" },
	BUZZ,
	{ -1, NULL }
};

static const binding_t bind_simple[] = {
	{ ACT_TRIGGER_L, L "select/click" }, { ACT_TRIGGER_R, R "select/click" },
	{ ACT_B, R "menu/click" }, { ACT_MENU, L "menu/click" },
	BUZZ,
	{ -1, NULL }
};

#undef L
#undef R
#undef BUZZ

static const struct { const char * profile; const binding_t * binds; } profiles[] = {
	{ "/interaction_profiles/oculus/touch_controller",     bind_touch },
	{ "/interaction_profiles/valve/index_controller",      bind_index },
	{ "/interaction_profiles/microsoft/motion_controller", bind_wmr },
	{ "/interaction_profiles/htc/vive_controller",         bind_vive },
	{ "/interaction_profiles/khr/simple_controller",       bind_simple },
};

static XrActionSet xr_action_set = XR_NULL_HANDLE;
static XrAction    xr_actions[ ACT_COUNT ];
static bool        actions_ready = false;
static XrTime      menu_down_since = 0;
static bool        menu_recentered = false;

static XrPath xr_path( const char * s )
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath( xr_instance, s, &p );
	return p;
}

static bool create_actions( void )
{
	XrActionSetCreateInfo sci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	XrResult r;
	unsigned i;

	strcpy( sci.actionSetName, "gameplay" );
	strcpy( sci.localizedActionSetName, "Gameplay" );
	r = xrCreateActionSet( xr_instance, &sci, &xr_action_set );
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: xrCreateActionSet failed: %s\n", xr_str( r ) );
		return false;
	}

	for( i = 0; i < ACT_COUNT; i++ )
	{
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		aci.actionType = action_info[ i ].type;
		strcpy( aci.actionName, action_info[ i ].name );
		strcpy( aci.localizedActionName, action_info[ i ].desc );
		r = xrCreateAction( xr_action_set, &aci, &xr_actions[ i ] );
		if( XR_FAILED( r ) )
		{
			Msg( "openxr: xrCreateAction(%s) failed: %s\n", action_info[ i ].name, xr_str( r ) );
			return false;
		}
	}

	for( i = 0; i < sizeof( profiles ) / sizeof( profiles[ 0 ] ); i++ )
	{
		XrActionSuggestedBinding binds[ ACT_COUNT + 4 ];
		XrInteractionProfileSuggestedBinding sug = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		uint32_t n = 0;
		const binding_t * b;

		for( b = profiles[ i ].binds; b->path; b++ )
		{
			binds[ n ].action  = xr_actions[ b->action ];
			binds[ n ].binding = xr_path( b->path );
			n++;
		}
		sug.interactionProfile     = xr_path( profiles[ i ].profile );
		sug.countSuggestedBindings = n;
		sug.suggestedBindings      = binds;
		r = xrSuggestInteractionProfileBindings( xr_instance, &sug );
		if( XR_FAILED( r ) )
			DebugPrintf( "openxr: bindings for %s refused: %s\n", profiles[ i ].profile, xr_str( r ) );
	}

	attach.countActionSets = 1;
	attach.actionSets      = &xr_action_set;
	r = xrAttachSessionActionSets( xr_session, &attach );
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: xrAttachSessionActionSets failed: %s\n", xr_str( r ) );
		return false;
	}
	return true;
}

static float action_float( int a )
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
	gi.action = xr_actions[ a ];
	if( XR_FAILED( xrGetActionStateFloat( xr_session, &gi, &s ) ) || !s.isActive )
		return 0.0f;
	return s.currentState;
}

static bool action_bool( int a, XrTime * changed )
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
	gi.action = xr_actions[ a ];
	if( XR_FAILED( xrGetActionStateBoolean( xr_session, &gi, &s ) ) || !s.isActive )
		return false;
	if( changed )
		*changed = s.lastChangeTime;
	return s.currentState ? true : false;
}

static void action_vec2( int a, float * x, float * y )
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
	gi.action = xr_actions[ a ];
	*x = *y = 0.0f;
	if( XR_FAILED( xrGetActionStateVector2f( xr_session, &gi, &s ) ) || !s.isActive )
		return;
	*x = s.currentState.x;
	*y = s.currentState.y;
}

bool vr_read_controllers( vr_controllers_t * out )
{
	XrActiveActionSet active = { 0 };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	XrTime changed = 0;
	bool menu;
	float x, y;

	memset( out, 0, sizeof( *out ) );

	if( !actions_ready || !xr_running || xr_state != XR_SESSION_STATE_FOCUSED )
		return false;

	active.actionSet = xr_action_set;
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	if( XR_FAILED( xrSyncActions( xr_session, &sync ) ) )
		return false;

	// the game's joystick convention: x right, y down
	action_vec2( ACT_MOVE, &x, &y );
	out->axis[ VR_AXIS_LEFT_X ]  = x;
	out->axis[ VR_AXIS_LEFT_Y ]  = -y;
	action_vec2( ACT_LOOK, &x, &y );
	out->axis[ VR_AXIS_RIGHT_X ] = x;
	out->axis[ VR_AXIS_RIGHT_Y ] = -y;
	out->axis[ VR_AXIS_LEFT_TRIGGER ]  = action_float( ACT_TRIGGER_L );
	out->axis[ VR_AXIS_RIGHT_TRIGGER ] = action_float( ACT_TRIGGER_R );

	out->button[ VR_BUTTON_LEFT_TRIGGER ]  = out->axis[ VR_AXIS_LEFT_TRIGGER ]  > 0.5f;
	out->button[ VR_BUTTON_RIGHT_TRIGGER ] = out->axis[ VR_AXIS_RIGHT_TRIGGER ] > 0.5f;
	out->button[ VR_BUTTON_LEFT_GRIP ]     = action_float( ACT_GRIP_L ) > 0.5f;
	out->button[ VR_BUTTON_RIGHT_GRIP ]    = action_float( ACT_GRIP_R ) > 0.5f;
	out->button[ VR_BUTTON_A ]             = action_bool( ACT_A, NULL );
	out->button[ VR_BUTTON_B ]             = action_bool( ACT_B, NULL );
	out->button[ VR_BUTTON_X ]             = action_bool( ACT_X, NULL );
	out->button[ VR_BUTTON_Y ]             = action_bool( ACT_Y, NULL );
	out->button[ VR_BUTTON_LEFT_STICK ]    = action_bool( ACT_STICK_L, NULL );
	out->button[ VR_BUTTON_RIGHT_STICK ]   = action_bool( ACT_STICK_R, NULL );

	// a long press of the menu button recentres instead of opening the menu;
	// the press only reaches the game once released short
	menu = action_bool( ACT_MENU, &changed );
	if( menu )
	{
		if( !menu_down_since )
		{
			menu_down_since = changed ? changed : frame_state.predictedDisplayTime;
			menu_recentered = false;
		}
		if( !menu_recentered &&
		    frame_state.predictedDisplayTime - menu_down_since > RECENTER_HOLD )
		{
			vr_recenter();
			menu_recentered = true;
		}
	}
	else if( menu_down_since )
	{
		out->button[ VR_BUTTON_MENU ] = !menu_recentered;
		menu_down_since = 0;
	}
	return true;
}

void vr_haptic( int hand, float amplitude, int ms )
{
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };

	if( !vr_vibration || !actions_ready || !xr_running || xr_state != XR_SESSION_STATE_FOCUSED )
		return;
	info.action   = xr_actions[ hand == VR_HAND_LEFT ? ACT_BUZZ_L : ACT_BUZZ_R ];
	vib.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
	vib.duration  = (XrDuration) ms * 1000000;
	vib.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback( xr_session, &info, (const XrHapticBaseHeader *) &vib );
}

/*===================================================================
  Bring up
===================================================================*/

static void fail_reason( const char * why )
{
	Msg( "openxr: VR disabled, rendering to the desktop instead: %s\n", why );
}

static bool create_instance( void )
{
	XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
	XrExtensionProperties * ext = NULL;
	const char * enabled[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	uint32_t n = 0, i;
	bool has_gl = false;
	XrResult r;

	if( !load_xr( XR_NULL_HANDLE ) )
		return false;
	r = xrEnumerateInstanceExtensionProperties( NULL, 0, &n, NULL );
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: no OpenXR runtime available (%s); is one installed and active?\n", xr_str( r ) );
		return false;
	}
	ext = (XrExtensionProperties *) calloc( n ? n : 1, sizeof( *ext ) );
	if( !ext )
		return false;
	for( i = 0; i < n; i++ )
		ext[ i ].type = XR_TYPE_EXTENSION_PROPERTIES;
	r = xrEnumerateInstanceExtensionProperties( NULL, n, &n, ext );
	for( i = 0; XR_SUCCEEDED( r ) && i < n; i++ )
		if( !strcmp( ext[ i ].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ) )
			has_gl = true;
	free( ext );
	if( !has_gl )
	{
		Msg( "openxr: the runtime does not support %s\n", XR_KHR_OPENGL_ENABLE_EXTENSION_NAME );
		return false;
	}

	strcpy( ci.applicationInfo.applicationName, "ForsakenX" );
	strcpy( ci.applicationInfo.engineName, "ProjectX" );
	ci.applicationInfo.applicationVersion = 1;
	ci.applicationInfo.engineVersion      = 1;
	ci.applicationInfo.apiVersion         = XR_MAKE_VERSION( 1, 0, 0 );
	ci.enabledExtensionCount              = 1;
	ci.enabledExtensionNames              = enabled;

	r = xrCreateInstance( &ci, &xr_instance );
	if( XR_FAILED( r ) )
	{
		xr_instance = XR_NULL_HANDLE;
		Msg( "openxr: xrCreateInstance failed: %s\n", xr_str( r ) );
		return false;
	}
	if( !load_xr( xr_instance ) )
		return false;

	if( XR_SUCCEEDED( xrGetInstanceProperties( xr_instance, &ip ) ) )
		DebugPrintf( "openxr: runtime '%s' %u.%u.%u\n", ip.runtimeName,
			(unsigned) XR_VERSION_MAJOR( ip.runtimeVersion ),
			(unsigned) XR_VERSION_MINOR( ip.runtimeVersion ),
			(unsigned) XR_VERSION_PATCH( ip.runtimeVersion ) );
	return true;
}

static bool get_system( void )
{
	XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
	XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
	XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	PFN_xrGetOpenGLGraphicsRequirementsKHR get_req = NULL;
	uint32_t n = 0, i;
	XrResult r;

	gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	r = xrGetSystem( xr_instance, &gi, &xr_system );
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: no headset available: %s\n", xr_str( r ) );
		return false;
	}
	if( XR_SUCCEEDED( xrGetSystemProperties( xr_instance, xr_system, &sp ) ) )
		DebugPrintf( "openxr: system '%s', position tracking %d\n",
			sp.systemName, (int) sp.trackingProperties.positionTracking );

	// required before a session may be created
	r = xrGetInstanceProcAddr( xr_instance, "xrGetOpenGLGraphicsRequirementsKHR",
	                           (PFN_xrVoidFunction *) &get_req );
	if( XR_FAILED( r ) || !get_req || XR_FAILED( get_req( xr_instance, xr_system, &req ) ) )
	{
		Msg( "openxr: could not query the runtime's OpenGL requirements\n" );
		return false;
	}
	DebugPrintf( "openxr: runtime wants OpenGL %u.%u to %u.%u, we have '%s'\n",
		(unsigned) XR_VERSION_MAJOR( req.minApiVersionSupported ),
		(unsigned) XR_VERSION_MINOR( req.minApiVersionSupported ),
		(unsigned) XR_VERSION_MAJOR( req.maxApiVersionSupported ),
		(unsigned) XR_VERSION_MINOR( req.maxApiVersionSupported ),
		(const char *) glGetString( GL_VERSION ) );

	for( i = 0; i < VR_EYES; i++ )
	{
		memset( &xr_config_views[ i ], 0, sizeof( xr_config_views[ i ] ) );
		xr_config_views[ i ].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	r = xrEnumerateViewConfigurationViews( xr_instance, xr_system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, VR_EYES, &n, xr_config_views );
	if( XR_FAILED( r ) || n != VR_EYES )
	{
		Msg( "openxr: no stereo view configuration: %s\n", xr_str( r ) );
		return false;
	}
	DebugPrintf( "openxr: recommended eye size %ux%u (max %ux%u)\n",
		(unsigned) xr_config_views[ 0 ].recommendedImageRectWidth,
		(unsigned) xr_config_views[ 0 ].recommendedImageRectHeight,
		(unsigned) xr_config_views[ 0 ].maxImageRectWidth,
		(unsigned) xr_config_views[ 0 ].maxImageRectHeight );
	return true;
}

static bool create_session( void )
{
	XrSessionCreateInfo ci = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrResult r;
#if defined( XR_USE_PLATFORM_WIN32 )
	XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	gb.hDC   = wglGetCurrentDC();
	gb.hGLRC = wglGetCurrentContext();
	if( !gb.hDC || !gb.hGLRC )
	{
		Msg( "openxr: no current WGL context\n" );
		return false;
	}
#elif defined( XR_USE_PLATFORM_XLIB )
	XrGraphicsBindingOpenGLXlibKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
	int attr[] = { GLX_FBCONFIG_ID, 0, None };
	GLXFBConfig * configs;
	XVisualInfo * vi;
	int count = 0;

	gb.xDisplay    = glXGetCurrentDisplay();
	gb.glxContext  = glXGetCurrentContext();
	gb.glxDrawable = glXGetCurrentDrawable();
	if( !gb.xDisplay || !gb.glxContext || !gb.glxDrawable )
	{
		Msg( "openxr: the GL context is not a GLX one (only X11 is supported on this platform)\n" );
		return false;
	}
	glXQueryContext( gb.xDisplay, gb.glxContext, GLX_FBCONFIG_ID, &attr[ 1 ] );
	configs = glXChooseFBConfig( gb.xDisplay, DefaultScreen( gb.xDisplay ), attr, &count );
	if( configs && count > 0 )
	{
		gb.glxFBConfig = configs[ 0 ];
		vi = glXGetVisualFromFBConfig( gb.xDisplay, configs[ 0 ] );
		if( vi )
		{
			gb.visualid = (uint32_t) vi->visualid;
			XFree( vi );
		}
	}
	if( configs )
		XFree( configs );
#else
	Msg( "openxr: no OpenGL graphics binding for this platform\n" );
	return false;
#endif

#if defined( XR_USE_PLATFORM_WIN32 ) || defined( XR_USE_PLATFORM_XLIB )
	ci.next     = &gb;
	ci.systemId = xr_system;
	r = xrCreateSession( xr_instance, &ci, &xr_session );
	if( XR_FAILED( r ) )
	{
		xr_session = XR_NULL_HANDLE;
		Msg( "openxr: xrCreateSession failed: %s\n", xr_str( r ) );
		return false;
	}

	rs.poseInReferenceSpace.orientation.w = 1.0f;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	r = xrCreateReferenceSpace( xr_session, &rs, &xr_local );
	if( XR_SUCCEEDED( r ) )
	{
		rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		r = xrCreateReferenceSpace( xr_session, &rs, &xr_head );
	}
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: xrCreateReferenceSpace failed: %s\n", xr_str( r ) );
		return false;
	}
	return true;
#endif
}

static bool create_swapchain( swapchain_t * sc, int w, int h )
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	uint32_t i;
	XrResult r;

	ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format      = chain_format;
	ci.sampleCount = 1;
	ci.width       = w;
	ci.height      = h;
	ci.faceCount   = 1;
	ci.arraySize   = 1;
	ci.mipCount    = 1;
	r = xrCreateSwapchain( xr_session, &ci, &sc->handle );
	if( XR_FAILED( r ) )
	{
		sc->handle = XR_NULL_HANDLE;
		Msg( "openxr: xrCreateSwapchain %dx%d failed: %s\n", w, h, xr_str( r ) );
		return false;
	}
	sc->w = w;
	sc->h = h;
	if( XR_FAILED( xrEnumerateSwapchainImages( sc->handle, 0, &sc->count, NULL ) ) || !sc->count )
		return false;
	sc->images = (XrSwapchainImageOpenGLKHR *) calloc( sc->count, sizeof( *sc->images ) );
	if( !sc->images )
		return false;
	for( i = 0; i < sc->count; i++ )
		sc->images[ i ].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
	r = xrEnumerateSwapchainImages( sc->handle, sc->count, &sc->count,
		(XrSwapchainImageBaseHeader *) sc->images );
	if( XR_FAILED( r ) )
	{
		Msg( "openxr: xrEnumerateSwapchainImages failed: %s\n", xr_str( r ) );
		return false;
	}
	return true;
}

static void destroy_swapchain( swapchain_t * sc )
{
	if( sc->handle != XR_NULL_HANDLE )
		xrDestroySwapchain( sc->handle );
	if( sc->images )
		free( sc->images );
	memset( sc, 0, sizeof( *sc ) );
}

static bool create_swapchains( void )
{
	int64_t formats[ 64 ];
	uint32_t n = 0, i;
	int w, h, pw, ph, eye;

	if( XR_FAILED( xrEnumerateSwapchainFormats( xr_session, 64, &n, formats ) ) || !n )
	{
		Msg( "openxr: no swapchain formats\n" );
		return false;
	}
	// The game's pixels are already sRGB encoded, so an sRGB swapchain
	// (written without GL_FRAMEBUFFER_SRGB) tells the compositor the truth.
	chain_format = 0;
	for( i = 0; i < n && !chain_format; i++ )
		if( formats[ i ] == GL_SRGB8_ALPHA8 )
			chain_format = formats[ i ];
	for( i = 0; i < n && !chain_format; i++ )
		if( formats[ i ] == GL_RGBA8 )
			chain_format = formats[ i ];
	if( !chain_format )
		chain_format = formats[ 0 ];
	DebugPrintf( "openxr: swapchain format 0x%x\n", (unsigned) chain_format );

	w = ( (int) xr_config_views[ 0 ].recommendedImageRectWidth  * vr_res_scale ) / 100;
	h = ( (int) xr_config_views[ 0 ].recommendedImageRectHeight * vr_res_scale ) / 100;
	if( w > (int) xr_config_views[ 0 ].maxImageRectWidth )  w = xr_config_views[ 0 ].maxImageRectWidth;
	if( h > (int) xr_config_views[ 0 ].maxImageRectHeight ) h = xr_config_views[ 0 ].maxImageRectHeight;
	w &= ~1;
	h &= ~1;
	if( w < 64 || h < 64 )
	{
		Msg( "openxr: unusable eye size %dx%d\n", w, h );
		return false;
	}

	for( eye = 0; eye < VR_EYES; eye++ )
		if( !create_swapchain( &eye_chain[ eye ], w, h ) )
			return false;
	if( !target_create( &eye_target, w, h ) )
		return false;

	// the panel: twice the window's resolution for legibility, within reason
	pw = render_info.ThisMode.w * 2;
	ph = render_info.ThisMode.h * 2;
	if( pw > 2048 )
	{
		ph = ph * 2048 / pw;
		pw = 2048;
	}
	if( !create_swapchain( &panel_chain, pw, ph ) || !target_create( &panel_target, pw, ph ) )
		return false;

	vgl_GenFramebuffers( 1, &copy_fbo );
	DebugPrintf( "openxr: rendering %dx%d per eye (%d%% of recommended), panel %dx%d\n",
		w, h, vr_res_scale, pw, ph );
	return true;
}

bool vr_init( void )
{
	if( !vr_requested )
		return false;

	DebugPrintf( "openxr: starting VR\n" );

	if( !load_gl() )
	{
		fail_reason( "the OpenGL context lacks framebuffer objects" );
		goto fail;
	}
	if( !create_instance() )
	{
		fail_reason( "no usable OpenXR runtime" );
		goto fail;
	}
	if( !get_system() )
	{
		fail_reason( "no headset found" );
		goto fail;
	}
	if( !create_session() )
	{
		fail_reason( "could not create an OpenXR session" );
		goto fail;
	}
	if( !create_swapchains() )
	{
		fail_reason( "could not create the swapchains" );
		goto fail;
	}
	actions_ready = create_actions();
	if( !actions_ready )
		Msg( "openxr: controllers unavailable, use the keyboard, mouse or a gamepad\n" );

	Msg( "openxr: VR session created, waiting for the headset\n" );
	return true;

fail:
	vr_shutdown();
	return false;
}

void vr_shutdown( void )
{
	int eye;

	if( xr_running && xr_session != XR_NULL_HANDLE )
		xrEndSession( xr_session );
	xr_running = false;
	views_valid = false;
	frame_begun = false;

	if( surface_w && vgl_BindFramebuffer )
		bind_target( 0, 0, 0 );

	for( eye = 0; eye < VR_EYES; eye++ )
		destroy_swapchain( &eye_chain[ eye ] );
	destroy_swapchain( &panel_chain );
	if( vgl_DeleteFramebuffers )
	{
		target_free( &eye_target );
		target_free( &panel_target );
		if( copy_fbo )
			vgl_DeleteFramebuffers( 1, &copy_fbo );
	}
	copy_fbo = 0;

	if( xr_action_set != XR_NULL_HANDLE )
		xrDestroyActionSet( xr_action_set );	// destroys its actions too
	xr_action_set = XR_NULL_HANDLE;
	actions_ready = false;
	if( xr_head != XR_NULL_HANDLE )
		xrDestroySpace( xr_head );
	if( xr_local != XR_NULL_HANDLE )
		xrDestroySpace( xr_local );
	xr_head = xr_local = XR_NULL_HANDLE;
	if( xr_session != XR_NULL_HANDLE )
		xrDestroySession( xr_session );
	xr_session = XR_NULL_HANDLE;
	if( xr_instance != XR_NULL_HANDLE && xrDestroyInstance )
		xrDestroyInstance( xr_instance );
	xr_instance = XR_NULL_HANDLE;
	xr_system = XR_NULL_SYSTEM_ID;
	xr_state = XR_SESSION_STATE_UNKNOWN;
}

/*===================================================================
  Session life cycle
===================================================================*/

static void poll_events( void )
{
	XrEventDataBuffer ev;

	for( ;; )
	{
		memset( &ev, 0, sizeof( ev ) );
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		if( xrPollEvent( xr_instance, &ev ) != XR_SUCCESS )
			break;

		switch( ev.type )
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			XrEventDataSessionStateChanged * sc = (XrEventDataSessionStateChanged *) &ev;
			xr_state = sc->state;
			DebugPrintf( "openxr: session state %d\n", (int) xr_state );
			switch( xr_state )
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
				XrResult r;
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				r = xrBeginSession( xr_session, &bi );
				if( XR_SUCCEEDED( r ) )
				{
					xr_running = true;
					want_recenter = true;
					Msg( "openxr: headset view started\n" );
				}
				else
					Msg( "openxr: xrBeginSession failed: %s\n", xr_str( r ) );
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession( xr_session );
				xr_running = false;
				Msg( "openxr: headset view stopped\n" );
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				Msg( "openxr: the runtime ended the session, continuing on the desktop\n" );
				vr_shutdown();
				return;
			default:
				break;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			Msg( "openxr: the runtime is going away, continuing on the desktop\n" );
			vr_shutdown();
			return;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			want_recenter = true;
			break;
		default:
			break;
		}
	}
}

// Rebuild the room space so that its origin is under the head and its
// forward is where the head faces, ignoring pitch, roll and height.
static void recenter( XrTime t )
{
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrSpace base, fresh = XR_NULL_HANDLE;
	const XrQuaternionf * q;
	float yaw;

	// locate the head in an un-recentred room
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rs.poseInReferenceSpace.orientation.w = 1.0f;
	if( XR_FAILED( xrCreateReferenceSpace( xr_session, &rs, &base ) ) )
		return;
	if( XR_FAILED( xrLocateSpace( xr_head, base, t, &loc ) ) ||
	    !( loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT ) )
	{
		xrDestroySpace( base );
		return;	// try again next frame
	}
	xrDestroySpace( base );

	q = &loc.pose.orientation;
	yaw = (float) atan2( 2.0 * ( q->w * q->y + q->x * q->z ),
	                     1.0 - 2.0 * ( q->y * q->y + q->z * q->z ) );
	rs.poseInReferenceSpace.orientation.x = 0.0f;
	rs.poseInReferenceSpace.orientation.y = sinf( yaw * 0.5f );
	rs.poseInReferenceSpace.orientation.z = 0.0f;
	rs.poseInReferenceSpace.orientation.w = cosf( yaw * 0.5f );
	if( loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT )
	{
		rs.poseInReferenceSpace.position.x = loc.pose.position.x;
		rs.poseInReferenceSpace.position.z = loc.pose.position.z;
	}
	if( XR_FAILED( xrCreateReferenceSpace( xr_session, &rs, &fresh ) ) )
		return;

	xrDestroySpace( xr_local );
	xr_local = fresh;
	want_recenter = false;
	panel_placed = false;
	DebugPrintf( "openxr: recentred, yaw %.1f degrees\n", yaw * 57.29578f );
}

/*===================================================================
  Frames
===================================================================*/

void vr_begin_frame( void )
{
	XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	uint32_t n = 0;
	XrResult r;
	int eye;

	frame_begun = false;
	views_valid = false;
	eyes_done   = 0;

	if( xr_session == XR_NULL_HANDLE )
		return;

	poll_events();
	if( xr_session == XR_NULL_HANDLE || !xr_running )
		return;

	memset( &frame_state, 0, sizeof( frame_state ) );
	frame_state.type = XR_TYPE_FRAME_STATE;
	r = xrWaitFrame( xr_session, &wi, &frame_state );
	if( XR_FAILED( r ) )
	{
		DebugPrintf( "openxr: xrWaitFrame failed: %s\n", xr_str( r ) );
		return;
	}
	r = xrBeginFrame( xr_session, &bi );
	if( XR_FAILED( r ) )
	{
		DebugPrintf( "openxr: xrBeginFrame failed: %s\n", xr_str( r ) );
		return;
	}
	frame_begun = true;

	if( !frame_state.shouldRender )
		return;

	if( want_recenter )
		recenter( frame_state.predictedDisplayTime );

	for( eye = 0; eye < VR_EYES; eye++ )
	{
		memset( &xr_views[ eye ], 0, sizeof( xr_views[ eye ] ) );
		xr_views[ eye ].type = XR_TYPE_VIEW;
	}
	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.displayTime           = frame_state.predictedDisplayTime;
	li.space                 = xr_local;
	r = xrLocateViews( xr_session, &li, &vs, VR_EYES, &n, xr_views );
	if( XR_FAILED( r ) || n != VR_EYES ||
	    !( vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT ) )
		return;

	for( eye = 0; eye < VR_EYES; eye++ )
	{
		const XrView * v = &xr_views[ eye ];
		views[ eye ].pos[ 0 ]  = v->pose.position.x;
		views[ eye ].pos[ 1 ]  = v->pose.position.y;
		views[ eye ].pos[ 2 ]  = v->pose.position.z;
		views[ eye ].quat[ 0 ] = v->pose.orientation.x;
		views[ eye ].quat[ 1 ] = v->pose.orientation.y;
		views[ eye ].quat[ 2 ] = v->pose.orientation.z;
		views[ eye ].quat[ 3 ] = v->pose.orientation.w;
		views[ eye ].tan_left  = tanf( v->fov.angleLeft );
		views[ eye ].tan_right = tanf( v->fov.angleRight );
		views[ eye ].tan_up    = tanf( v->fov.angleUp );
		views[ eye ].tan_down  = tanf( v->fov.angleDown );
	}
	views_valid = true;

	// from here on the game draws into the panel unless an eye is bound
	bind_target( panel_target.fbo, panel_target.w, panel_target.h );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClearDepth( 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
}

void vr_eye_begin( int eye )
{
	(void) eye;
	if( !views_valid )
		return;
	bind_target( eye_target.fbo, eye_target.w, eye_target.h );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClearDepth( 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
}

// copy an offscreen target into the next image of a swapchain
static bool copy_to_swapchain( target_t * t, swapchain_t * sc )
{
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	uint32_t index = 0;

	if( XR_FAILED( xrAcquireSwapchainImage( sc->handle, &ai, &index ) ) )
		return false;
	wi.timeout = 100000000;	// 100ms
	if( XR_FAILED( xrWaitSwapchainImage( sc->handle, &wi ) ) )
	{
		xrReleaseSwapchainImage( sc->handle, &ri );
		return false;
	}

	vgl_BindFramebuffer( GL_DRAW_FRAMEBUFFER, copy_fbo );
	vgl_FramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		sc->images[ index ].image, 0 );
	vgl_BindFramebuffer( GL_READ_FRAMEBUFFER, t->fbo );
	vgl_BlitFramebuffer( 0, 0, t->w, t->h, 0, 0, sc->w, sc->h, GL_COLOR_BUFFER_BIT, GL_LINEAR );
	vgl_FramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );

	xrReleaseSwapchainImage( sc->handle, &ri );
	return true;
}

void vr_eye_end( int eye )
{
	if( !views_valid )
		return;
	if( copy_to_swapchain( &eye_target, &eye_chain[ eye ] ) )
		eyes_done |= 1 << eye;
	// back to the panel for whatever is drawn next
	bind_target( panel_target.fbo, panel_target.w, panel_target.h );
}

// Draw a texture over the whole of the current viewport, added to what is
// there: black adds nothing. Fixed function, which every context the game
// creates provides (the GL2/GL3 renderers also rely on a compatibility
// context), with all state saved and restored around it.
// tc: texture coordinates of the corners (bottom left, bottom right, top
// right, top left), NULL for the whole texture; additive: black is see-through
static void draw_quad( GLuint tex, const float * tc, bool additive )
{
	static const float whole[ 8 ] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

#if GL > 1
	vgl_UseProgram( 0 );
#endif
	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );  glPushMatrix(); glLoadIdentity();
	glMatrixMode( GL_TEXTURE );    glPushMatrix(); glLoadIdentity();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_ALPHA_TEST );
	glDisable( GL_LIGHTING );
	glDisable( GL_FOG );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	if( additive )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
	}
	else
		glDisable( GL_BLEND );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	if( !tc )
		tc = whole;
	glBegin( GL_QUADS );
	glTexCoord2f( tc[ 0 ], tc[ 1 ] ); glVertex2f( -1.0f, -1.0f );
	glTexCoord2f( tc[ 2 ], tc[ 3 ] ); glVertex2f(  1.0f, -1.0f );
	glTexCoord2f( tc[ 4 ], tc[ 5 ] ); glVertex2f(  1.0f,  1.0f );
	glTexCoord2f( tc[ 6 ], tc[ 7 ] ); glVertex2f( -1.0f,  1.0f );
	glEnd();

	glMatrixMode( GL_TEXTURE );    glPopMatrix();
	glMatrixMode( GL_MODELVIEW );  glPopMatrix();
	glMatrixMode( GL_PROJECTION ); glPopMatrix();
	glPopAttrib();
#if GL > 1
	vgl_UseProgram( current_program );
#endif
}

static float dot3( const float * a, const float * b )
{
	return a[ 0 ] * b[ 0 ] + a[ 1 ] * b[ 1 ] + a[ 2 ] * b[ 2 ];
}

static void norm3( float * v )
{
	float l = sqrtf( dot3( v, v ) );
	if( l > 0.0f )
	{
		v[ 0 ] /= l; v[ 1 ] /= l; v[ 2 ] /= l;
	}
}

// v turned by the quaternion q
static void quat_turn( const float * q, const float * v, float * out )
{
	float t[ 3 ];
	t[ 0 ] = 2.0f * ( q[ 1 ] * v[ 2 ] - q[ 2 ] * v[ 1 ] );
	t[ 1 ] = 2.0f * ( q[ 2 ] * v[ 0 ] - q[ 0 ] * v[ 2 ] );
	t[ 2 ] = 2.0f * ( q[ 0 ] * v[ 1 ] - q[ 1 ] * v[ 0 ] );
	out[ 0 ] = v[ 0 ] + q[ 3 ] * t[ 0 ] + q[ 1 ] * t[ 2 ] - q[ 2 ] * t[ 1 ];
	out[ 1 ] = v[ 1 ] + q[ 3 ] * t[ 1 ] + q[ 2 ] * t[ 0 ] - q[ 0 ] * t[ 2 ];
	out[ 2 ] = v[ 2 ] + q[ 3 ] * t[ 2 ] + q[ 0 ] * t[ 1 ] - q[ 1 ] * t[ 0 ];
}

// where the direction d is in the eye's picture, in pixels from bottom left
static bool eye_project( const vr_view_t * v, const float * axes, const float * d, float * x, float * y )
{
	float z = dot3( d, axes + 6 );
	if( z < 1e-4f )
		return false;
	*x = ( dot3( d, axes ) / z - v->tan_left ) / ( v->tan_right - v->tan_left ) * eye_target.w;
	*y = ( dot3( d, axes + 3 ) / z - v->tan_down ) / ( v->tan_up - v->tan_down ) * eye_target.h;
	return true;
}

// one euro filter weight for a cutoff frequency in Hz
static float smoothing( float dt, float cutoff )
{
	return 1.0f / ( 1.0f + 1.0f / ( 2.0f * 3.14159265f * cutoff * dt ) );
}

// The steady desktop view: a window shaped cut-out of the left eye that
// follows the head's turns but not its wobble, with the room's horizon
// level (the ship's own roll stays). Gives the texture coordinates of the
// window's corners, or false to show the plain middle of the eye.
// Based on the observer view in Forsaken VR's vr_target.c.
static bool steady_cutout( float aspect, float * tc )
{
	static const float unit[ 9 ] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f };
	static float fd[ 3 ], last[ 3 ], rate[ 3 ];
	static XrTime last_time = 0;
	const vr_view_t * v = &views[ 0 ];
	float axes[ 9 ], * f = axes + 6, side[ 3 ], p[ 3 ];
	float W = (float) eye_target.w, H = (float) eye_target.h;
	float dt, a, cx, cy, hx, hy, theta = 0.0f, c, s, cw, ch, ex = 0.0f, ey = 0.0f;
	int i, k;

	// the eye's right, up and forward
	for( i = 0; i < 3; i++ )
		quat_turn( v->quat, unit + i * 3, axes + i * 3 );

	dt = last_time ? ( frame_state.predictedDisplayTime - last_time ) * 1e-9f : 0.0f;
	last_time = frame_state.predictedDisplayTime;
	if( dt <= 0.0f || dt > 0.1f || dot3( f, fd ) < 0.7071f )
	{
		// a first frame, a pause or a recentre: start over
		for( i = 0; i < 3; i++ )
		{
			fd[ i ] = last[ i ] = f[ i ];
			rate[ i ] = 0.0f;
		}
	}
	else
	{
		// heavy at rest, light while turning
		a = smoothing( dt, 1.0f );
		for( i = 0; i < 3; i++ )
		{
			rate[ i ] += a * ( ( f[ i ] - last[ i ] ) / dt - rate[ i ] );
			last[ i ] = f[ i ];
		}
		a = smoothing( dt, 0.4f + 2.0f * sqrtf( dot3( rate, rate ) ) );
		for( i = 0; i < 3; i++ )
			fd[ i ] += a * ( f[ i ] - fd[ i ] );
		norm3( fd );
	}

	if( !eye_project( v, axes, fd, &cx, &cy ) )
		return false;

	// the room's horizontal there is the window's
	side[ 0 ] = -fd[ 2 ]; side[ 1 ] = 0.0f; side[ 2 ] = fd[ 0 ];
	norm3( side );
	for( i = 0; i < 3; i++ )
		p[ i ] = fd[ i ] + 0.02f * side[ i ];
	if( fabsf( fd[ 1 ] ) < 0.98f && eye_project( v, axes, p, &hx, &hy ) )
		theta = atan2f( hy - cy, hx - cx );
	c = cosf( theta );
	s = sinf( theta );

	// 0.78 of the eye's width, smaller until the turned corners fit
	for( cw = 0.78f * W; cw > 0.1f * W; cw *= 0.95f )
	{
		ch = cw / aspect;
		ex = fabsf( cw * 0.5f * c ) + fabsf( ch * 0.5f * s );
		ey = fabsf( cw * 0.5f * s ) + fabsf( ch * 0.5f * c );
		if( ex <= W * 0.5f && ey <= H * 0.5f )
			break;
	}
	ch = cw / aspect;
	cx = cx < ex ? ex : ( cx > W - ex ? W - ex : cx );
	cy = cy < ey ? ey : ( cy > H - ey ? H - ey : cy );

	for( k = 0; k < 4; k++ )
	{
		float sx = ( k == 1 || k == 2 ) ? 0.5f : -0.5f;
		float sy = ( k >= 2 ) ? 0.5f : -0.5f;
		tc[ k * 2 ]     = ( cx + sx * cw * c - sy * ch * s ) / W;
		tc[ k * 2 + 1 ] = ( cy + sx * cw * s + sy * ch * c ) / H;
	}
	return true;
}

// the desktop window: the left eye, cut to the window's shape, with the
// panel over it; or just the panel when there was no 3D scene
static void draw_mirror( bool eyes )
{
	int ww = render_info.ThisMode.w, wh = render_info.ThisMode.h;
	GLint vp[4];
	float tc[ 8 ];

	glGetIntegerv( GL_VIEWPORT, vp );
	bind_target( 0, 0, 0 );
	glViewport( 0, 0, ww, wh );

	if( eyes && vr_steady_mirror && steady_cutout( (float) ww / (float) wh, tc ) )
	{
		draw_quad( eye_target.color, tc, false );
		draw_quad( panel_target.color, NULL, true );
	}
	else if( eyes )
	{
		// largest window shaped rectangle in the middle of the eye
		int sw = eye_target.w, sh = eye_target.h;
		if( sw * wh > sh * ww )
			sw = sh * ww / wh;
		else
			sh = sw * wh / ww;
		vgl_BindFramebuffer( GL_READ_FRAMEBUFFER, eye_target.fbo );
		vgl_BlitFramebuffer(
			( eye_target.w - sw ) / 2, ( eye_target.h - sh ) / 2,
			( eye_target.w + sw ) / 2, ( eye_target.h + sh ) / 2,
			0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		vgl_BindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
		draw_quad( panel_target.color, NULL, true );
	}
	else
	{
		vgl_BindFramebuffer( GL_READ_FRAMEBUFFER, panel_target.fbo );
		vgl_BlitFramebuffer( 0, 0, panel_target.w, panel_target.h,
			0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		vgl_BindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	}
	glViewport( vp[0], vp[1], vp[2], vp[3] );
}

void vr_end_frame( void )
{
	XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
	XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView pv[ VR_EYES ];
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader * layers[ 2 ];
	bool eyes = ( eyes_done == ( 1 << VR_EYES ) - 1 );
	bool panel = false;
	XrResult r;
	int eye;

	if( !frame_begun )
		return;
	frame_begun = false;

	if( views_valid )
	{
		// the panel's pixels are opaque everywhere, so its alpha says nothing.
		// as a HUD over the world it is blended premultiplied with zero alpha,
		// which is additive: black is see-through.
		if( eyes )
		{
			glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		}
		panel = copy_to_swapchain( &panel_target, &panel_chain );
	}

	ei.displayTime          = frame_state.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount           = 0;
	ei.layers               = layers;

	if( views_valid && eyes )
	{
		for( eye = 0; eye < VR_EYES; eye++ )
		{
			memset( &pv[ eye ], 0, sizeof( pv[ eye ] ) );
			pv[ eye ].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			pv[ eye ].pose = xr_views[ eye ].pose;
			pv[ eye ].fov  = xr_views[ eye ].fov;
			pv[ eye ].subImage.swapchain = eye_chain[ eye ].handle;
			pv[ eye ].subImage.imageRect.extent.width  = eye_chain[ eye ].w;
			pv[ eye ].subImage.imageRect.extent.height = eye_chain[ eye ].h;
		}
		proj.space     = xr_local;
		proj.viewCount = VR_EYES;
		proj.views     = pv;
		layers[ ei.layerCount++ ] = (const XrCompositionLayerBaseHeader *) &proj;
	}

	if( views_valid && panel )
	{
		float h = PANEL_WIDTH * (float) panel_chain.h / (float) panel_chain.w;

		quad.subImage.swapchain = panel_chain.handle;
		quad.subImage.imageRect.extent.width  = panel_chain.w;
		quad.subImage.imageRect.extent.height = panel_chain.h;
		quad.size.width  = PANEL_WIDTH;
		quad.size.height = h;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		if( eyes )
		{
			// the HUD is fixed in the cockpit ahead of the ship's eye point,
			// or goes where the head goes
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.space = vr_hud_cockpit ? xr_local : xr_head;
			quad.pose.orientation.w = 1.0f;
			quad.pose.position.z = -PANEL_DIST;
			panel_placed = false;
		}
		else
		{
			// a menu screen stays where it first appeared, in front of the face
			if( !panel_placed )
			{
				float x = ( xr_views[ 0 ].pose.position.x + xr_views[ 1 ].pose.position.x ) * 0.5f;
				float y = ( xr_views[ 0 ].pose.position.y + xr_views[ 1 ].pose.position.y ) * 0.5f;
				float z = ( xr_views[ 0 ].pose.position.z + xr_views[ 1 ].pose.position.z ) * 0.5f;
				const XrQuaternionf * q = &xr_views[ 0 ].pose.orientation;
				float yaw = (float) atan2( 2.0 * ( q->w * q->y + q->x * q->z ),
				                           1.0 - 2.0 * ( q->y * q->y + q->z * q->z ) );
				panel_pose.orientation.x = 0.0f;
				panel_pose.orientation.y = sinf( yaw * 0.5f );
				panel_pose.orientation.z = 0.0f;
				panel_pose.orientation.w = cosf( yaw * 0.5f );
				panel_pose.position.x = x - sinf( yaw ) * PANEL_DIST;
				panel_pose.position.y = y;
				panel_pose.position.z = z - cosf( yaw ) * PANEL_DIST;
				panel_placed = true;
			}
			quad.layerFlags = 0;
			quad.space = xr_local;
			quad.pose  = panel_pose;
		}
		layers[ ei.layerCount++ ] = (const XrCompositionLayerBaseHeader *) &quad;
	}

	r = xrEndFrame( xr_session, &ei );
	if( XR_FAILED( r ) )
		DebugPrintf( "openxr: xrEndFrame failed: %s\n", xr_str( r ) );

	if( views_valid )
		draw_mirror( eyes );
	views_valid = false;
}

#endif // VR_OPENXR
