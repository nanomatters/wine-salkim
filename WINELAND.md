# Wineland Proton

Wineland is an experimental, Wayland-focused Proton build currently based on
CachyOS Proton. It adds better support for games and launchers that use modern
GPU rendering, embedded web views, custom window chrome, and several Windows
processes at once.

## What Wineland Gives You

- **Better CEF and Chromium launchers.** Browser-based launchers, embedded web
  views, dropdowns, and popups can render correctly when their GPU process is
  separate from the visible launcher window. This is the project's main feature.

- **Direct GPU-frame sharing between Wine processes.** Wineland can pass a
  rendered dma-buf directly to the Wine process that owns the visible Wayland
  window. This avoids copying full frames through the CPU when the graphics
  driver and compositor support it.

- **Support for GPU-rendered child content.** Games and launchers that render
  into a child window or from another process have a Wayland-native path instead
  of relying only on ordinary window updates.

- **Mixed GPU and GDI UI that stays visible.** Native buttons, overlays, custom
  chrome, and GDI controls can appear correctly above a GPU-rendered game or
  browser surface without exposing Wine's unpainted backing buffer.

- **Better custom frames, menus, and tray integration.** Wineland handles many
  Chromium/CEF title bars, frameless windows, context menus, popups, and
  Windows tray icons more naturally in a Wayland desktop.

- **More robust fullscreen and window state changes.** It improves common
  trouble spots around fullscreen, borderless mode, alt-tab, maximize, restore,
  fractional scaling, virtual modes, and changing resolution in a game.

- **Improved HDR and colour plumbing.** On a compatible Wayland compositor and
  display, it reports HDR and SDR-white information to Windows games and carries
  HDR and scRGB colour descriptions through Wine's presentation paths.

- **Fewer presentation hangs and surface-recreation problems.** Infinite Vulkan
  presentation waits can return for swapchain recreation instead of remaining
  stuck indefinitely, and compatible Wayland resources are reused across
  aggressive recreation.

- **Optional direct-toplevel fullscreen presentation.** An experimental switch
  can simplify the Wayland surface tree for eligible fullscreen Vulkan games,
  which may improve the compositor's opportunity to use direct scanout.

- **A real wined3d/OpenGL dma-buf path.** When a title uses
  `PROTON_USE_WINED3D=1`, Wineland can copy its GL backbuffer into an exportable
  texture and share it with the Wayland window owner. This matters for older
  games, wined3d fallback testing, and browser-style content using OpenGL.

- **Targeted D3D/OpenGL texture sharing.** CEF and ANGLE-style renderers can
  use the common GPU-texture sharing path between Direct3D and OpenGL. This is
  compatibility support for those applications, not every possible interop use.

Most of these features work automatically. The optional direct-toplevel path is
disabled by default and can be tested per game:

```
WAYLANDDRV_DIRECT_TOPLEVEL=1 %command%
```

Direct scanout and zero-copy presentation are always compositor decisions. They
depend on the game, GPU driver, monitor, overlays, cursor, recording state, and
colour pipeline; Wineland improves the path to those features but cannot force
them.

## Tested Launchers

The current branch has been exercised with the following launcher and web-view
workloads:

- **Native Windows Steam client.** Launch it with
  `PROTON_USE_WINED3D=1 %command%`. This is specific to the tested Windows Steam
  setup; other games should normally use their default renderer.
- **Battle.net**
- **Ubisoft Connect**
- **Rockstar Games Launcher**
- **Warframe's launcher**

These are practical compatibility tests, not guarantees for every version or
compositor. They cover the CEF/Chromium, custom-frame, popup, GDI-overlay, and
cross-process rendering cases that motivated Wineland.

## How It Works

The sections below explain the implementation and fallbacks in more detail.
They are useful for troubleshooting, testing, and understanding why a given
game may use a fast path or a normal Wayland fallback.

## The Main Feature: Cross-Process dma-buf Presentation

Wineland adds an internal **HWND dma-buf bridge**. It transfers GPU buffers,
not copied pixels, from the process that rendered them to the process that owns
the Wayland window:

```text
Renderer or CEF GPU process
        |
        | dma-buf fd, format, modifier, damage, colour description
        v
Wine server HWND channel
        |
        | per-window ownership and release tracking
        v
Wayland window owner
        |
        | imports the buffer and attaches it to the surface tree
        v
Wayland compositor
```

The Wayland process advertises its supported formats and modifiers. Managed
Vulkan intersects these with the GPU's exportable combinations before creating
shared images, while the OpenGL path checks exported images against the same
capabilities. The compositor releases each buffer when it no longer needs it,
allowing Wine to reuse the corresponding ring slot. Bridge frames can carry an
acquire fence when the producer and compositor support explicit synchronization;
otherwise Wine waits for rendering to finish before publishing them.

Bridge selection is automatic. Managed Vulkan falls through to a host swapchain
when bridge setup fails, while OpenGL capture is attempted only after Wine has
confirmed the required extensions and consumer capabilities. Whether an
application has a useful fallback for unsupported cross-process child content
remains application-specific.

### Producers and overlays

Managed Vulkan swapchains carry child content through the bridge only when
cross-process presentation is needed. In the ordinary single-process case, Wine
keeps using the direct host swapchain.

The wined3d/OpenGL path captures the GL backbuffer into an exportable texture
ring and keeps each image busy until Wayland releases it. Separately, the
targeted `WGL_NV_DX_interop` support covers the common single-image 2D RGBA
texture path used by CEF and ANGLE-style clients; it is not a complete
implementation of every interop resource type.

Windows applications can also mix GPU content with GDI controls or custom
chrome. Wineland tracks the region that must remain above GPU content and makes
only that part of the GDI surface visible there. This avoids exposing unpainted
backing storage during launcher startup while keeping genuine controls visible.

## Other Wineland Improvements

### More direct fullscreen Vulkan presentation

Eligible fullscreen Vulkan games can optionally borrow the Wayland toplevel
surface directly. This removes an unnecessary child-surface layer and gives the
compositor a simpler fullscreen surface tree.

It is disabled by default while it is being tested. A role change, visible
overlay, geometry change, or other failed eligibility check invalidates the
promoted presentation, allowing a recreated swapchain to use the regular
surface layout.

Direct-toplevel presentation can improve the conditions for direct scanout,
but it cannot force it. The compositor decides whether a frame receives a
hardware plane.

### Fullscreen scaling and display transitions

Wineland improves the Wayland details that show up as game-facing problems:

- fullscreen, maximize, minimize, restore, and alt-tab behavior;
- configure acknowledgement and mode-change handling;
- borderless, frameless, and collapsed-caption windows;
- fractional-scale coordinate mapping and virtual display modes; and
- colour-aware FSHack scaling for supported formats and colour spaces.

At a lower game resolution, FSHack may require a GPU composition pass. It does
not enable that pass solely for native-resolution presentation.

### HDR and colour descriptions

With compositor support, Wineland reports HDR capability and the Windows
SDR-white level, and describes HDR10 and scRGB content to Wayland. Cross-process
dma-buf presentation (for example, GPU-rendered child-window content) follows
the Windows BT.2100 model, while ordinary SDR remains on the compositor's
default colour handling. Game-provided HDR metadata is not used for those
Wine-owned Wayland images; it is forwarded when a game uses a normal Vulkan
swapchain on its own visible window.

HDR still requires compatible display, driver, and compositor support. Colour
conversion or an ICC profile may require composition and prevent direct scanout.

### Presentation reliability

Wineland keeps infinite Vulkan presentation waits interruptible, returning
out-of-date when the target changes or a wait makes no progress for three
seconds. This favors swapchain recreation over an indefinite stall. Compatible
Wayland client surfaces are also reused across recreation to reduce role and
subsurface churn.

### Desktop integration

Wineland adds native StatusNotifierItem support for applications that use the
classic Windows tray API. It also provides a Wayland layer-shell path for tray
menus and selected unanchored application menus, while retaining the regular
window-role fallback when layer-shell is unavailable.

## Important Limits

- Wineland is not a compositor. KWin, Gamescope, COSMIC, Mutter, Sway, or
  another compositor still decides plane use, direct scanout, colour transforms,
  and timing.
- dma-buf paths need compatible GPU drivers, formats, modifiers, and compositor
  protocol support. Wine rejects incompatible bridge configurations, but the
  visible fallback for cross-process child content can be application-specific.
- Direct scanout is not guaranteed. Cursors, overlays, recording, effects,
  colour processing, or a non-fullscreen surface can require composition.
- The direct-toplevel switch is experimental and intentionally opt-in.
- HDR requires a compatible display and compositor pipeline. It may disable
  direct scanout when colour conversion is necessary.

## Recommended Use and Diagnostics

Use Wineland normally in a native Wayland session. The cross-process dma-buf
bridge is automatic; no launch option is required for it.

For a short diagnostic capture, use the Wine channels relevant to the problem:

```
WINEDEBUG=+waylanddrv,+vulkan,+d3d,+opengl %command%
```

For browser-launcher problems, include whether the issue is in the main window,
a CEF popup, a tray menu, or an embedded web view. For display issues, include
the compositor and version, GPU and driver, monitor layout and scale, HDR state,
resolution, and whether direct-toplevel mode was enabled.
