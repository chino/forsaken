# Playing in a VR headset (OpenXR)

Forsaken can render to a VR headset through [OpenXR](https://www.khronos.org/openxr/),
so it works with any OpenXR runtime: SteamVR, Monado, WiVRn/ALVR, Meta's runtime,
Virtual Desktop and so on. It is built in whenever the OpenXR headers are
available, and the OpenXR loader is only opened at run time when VR is asked
for, so the same binary runs fine on machines without OpenXR and behaves
exactly like a desktop build unless it is started with `-vr`.

The OpenXR code is based on the VR port in
[Forsaken VR](https://github.com/GameOrDie007/Forsaken-VR), a fork of ForsakenX
under the same license, which has many more VR features (aiming with the
controller, a cockpit fixed HUD, haptics, comfort options, standalone Quest builds).

## Building

Only the OpenXR headers are needed at build time, plus the usual dependencies.
On Debian/Ubuntu:

    apt-get install libopenxr-dev libx11-dev

When `pkg-config openxr` finds them, every build configuration includes VR
(`make check` shows `VR = 1`); the binary is not linked against the loader.
`make VR=0` leaves it out: `vr_openxr.c` is not compiled and every hook is
compiled out.

At run time `-vr` opens `libopenxr_loader.so.1` (`openxr_loader.dll` on
Windows), which comes with the OpenXR runtime or the `libopenxr1` package.

Platform notes:

- Linux: the game window must be an X11/GLX window (SDL's default; XWayland is
  fine). A native Wayland SDL window is detected and the game stays on the
  desktop.
- Windows: the WGL graphics binding is in place but has not been tested in
  this tree.
- macOS: there is no OpenXR OpenGL binding, so VR is not available.

## Running

    ./projectx -vr

Start your OpenXR runtime (SteamVR, `monado-service`, ...) and connect the
headset first. If there is no OpenXR loader, no runtime, no headset, or anything else goes
wrong, the log says why in a line starting `openxr: VR disabled, rendering to
the desktop instead:` and the game simply runs on the desktop as usual.

Options:

| Option         | Default | |
|----------------|---------|-|
| `-vr`          | off     | Render to the headset. `VR = true` in the config file does the same. |
| `-vrscale:N`   | 30      | World units per metre. Bigger makes the world feel smaller. |
| `-vrres:N`     | 100     | Eye resolution as a percentage of what the runtime recommends (10 to 400). |

Comfort settings, in the config file:

| Setting            | Default | |
|--------------------|---------|-|
| `VRTurnSpeed = N`  | 100     | How fast the controllers turn and pitch the ship, as a percentage of the game's rate. |
| `VRAutoLevel = true` | false | Ease the ship back level when you are not rolling, even if the pilot's own autoleveling is off. |
| `VRShipBob = false`  | true  | Stop the gentle drift of your ship at rest, which moves the whole view in a headset. |
| `VRHudCockpit = false` | true | Keep the HUD in front of your eyes wherever you look instead of fixed in the cockpit. |

## What you get

- The 3D world in stereo with the runtime's per-eye field of view. Your head
  turns and moves inside the cockpit, on top of the ship's own six degrees of
  freedom.
- The HUD and in-game menus on a panel about 2 m wide, 1.75 m in front of you,
  fixed in the cockpit: look around and it stays put, like the cockpit does
  (`VRHudCockpit = false` makes it follow your head). Black is see-through, so
  the HUD floats over the world.
- Title screens and loading screens on the same panel, fixed in the room where
  it appeared, with the 3D title room behind it in stereo.
- The desktop window mirrors the left eye with the HUD laid over it.
- **Recentre** by holding the menu button for about half a second.

## Controllers

The controllers show up as a joystick called **OpenXR Controllers**, so they
can be changed in the game's joystick menus like any other joystick and are
saved with your pilot. Bindings are suggested for Oculus Touch, Valve Index,
Windows Mixed Reality, HTC Vive and the generic Khronos simple controller.

Default layout (Touch names):

| Control            | Action |
|--------------------|--------|
| Left stick         | Thrust and strafe |
| Right stick        | Turn and pitch |
| Right trigger      | Fire primary |
| Left trigger       | Fire secondary |
| Right / left grip  | Slide up / down |
| A                  | Nitro |
| B                  | Drop mine |
| X / Y              | Roll left / right |
| Left / right stick click | Next primary / secondary weapon |
| Menu (short press) | Escape (in-game menu) |
| Menu (hold)        | Recentre |

In menus either stick moves, A or the right trigger chooses and B goes back.
Keyboard, mouse and gamepads keep working as usual.

## Trying it without a headset

Monado can run headless with a simulated headset, which is enough to check
that the session starts and frames are accepted:

    apt-get install monado-service libopenxr1-monado mesa-vulkan-drivers
    XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 monado-service &
    ./projectx -vr -vrres:400

(`monado-service` wants a usable stdin; `sleep infinity | monado-service`
works when it is started from a script.) The desktop window then shows the
simulated headset's left eye. The simulated device has no controllers.

## Known limitations

- Changing the video mode while in VR is not supported; restart instead.
- The rear view and missile camera pictures are drawn on the HUD panel, so
  they are see-through like the rest of the HUD.
- No haptics or controller aiming yet; see Forsaken VR for those.
