# Wineland Architecture and Design

Wineland is an experimental Proton/Wine branch focused on native Wayland
presentation for Windows applications, especially launchers and games where
multiple processes, APIs, and window types contribute to one visible HWND tree.

This document describes the rendering, windowing, presentation, display, input,
and desktop-integration work in this branch. It intentionally does not document
the separate networking backports or generic Wine build/use instructions.

## How To Read This Document

Start with **Vocabulary**, **High-Level Data Flow**, **Frame Lifecycle**, and
**Producer Matrix**. Those sections give the mental model without requiring a
full source dive. Then read **Resource Lifetime Rules** and **Threading and
Locking** before changing code; those are the invariants shared by every
producer and consumer path.

After that, jump to the subsystem you are touching: Vulkan, wined3d OpenGL,
GDI, DWM cloaking, display modes, popups, tray, or input. **Entry-Point
Symbols** and **Main File Map** are the concept-to-code index when you need a
greppable symbol or file.

## Design Goals

Wineland keeps the Windows HWND model as the source of truth while letting
Wayland do the final composition. The important goals are:

- Present GPU frames from a process that does not own the visible toplevel.
- Preserve mixed rendering: Vulkan, DXVK/vkd3d, wined3d OpenGL, GDI, layered
  windows, popups, tray menus, and shaped windows.
- Avoid copying full frames through the parent process when a dma-buf can be
  passed directly.
- Preserve Windows visibility, clipping, minimize/restore, and foreground
  semantics even though Wayland has different surface roles and state.
- Keep resource ownership explicit: file descriptors, Wayland buffers, server
  channels, release tokens, and cached frame slots all have defined lifetimes.
- Fail back to ordinary Wine/Wayland behavior when a fast path cannot be used.

The work is not a compositor replacement. Gamescope, KWin, Mutter, Sway, and
other compositors still provide the Wayland environment. Wineland is the Wine
side that translates Windows window/rendering behavior into Wayland objects.

## Vocabulary

- **Host HWND**: The Windows window whose client area is being composed.
- **Producer**: The Wine-side path that hands the HWND compositor a concrete
  frame buffer for an HWND. It may be the renderer itself, such as a managed
  Vulkan swapchain; an export bridge, such as wined3d copying its back buffer
  into an exportable GL texture; or a GDI surface that already contains pixels
  drawn by the application. "Producer" does not mean "the original code that
  generated every pixel"; it means "the side that publishes the frame."
- **Consumer**: The process that owns the Wayland toplevel and turns published
  producer frames into Wayland attachments. It imports the dma-buf or SHM
  backing store, creates/reuses wl_buffers, positions surfaces, and returns
  release tokens when the compositor is done.
- **Client surface**: A Wayland child surface associated with a Vulkan or OpenGL
  client path.
- **Swapchain**: A rotating set of images used for presentation. The
  application renders into or resolves into one image, presents it, then later
  acquires another image for the next frame. In Wineland this may be a real host
  Vulkan/Wayland swapchain, a Wine-managed Vulkan image ring, or a wined3d
  OpenGL capture ring that behaves like a swapchain for publication.
- **Managed swapchain**: A Wine-created Vulkan swapchain replacement that owns
  exportable images and publishes them as HWND dma-buf frames.
- **Direct WSI**: The normal host Vulkan/Wayland swapchain path.
- **Carrier**: A transparent Wayland buffer attached to keep a surface mapped
  without showing Wine-initialized white/black pixels.
- **Punch-through hole**: A region removed from a parent GDI surface so a
  self-presenting child can show through.
- **GDI overlay**: A secondary SHM frame publisher for GDI pixels that must
  appear above a self-presenting child.
- **Stable slot**: A producer ring slot whose dma-buf identity is stable enough
  for the consumer to cache the wl_buffer.

For the code behind these terms, see **Entry-Point Symbols** and
**Main File Map** near the end of this document.

Naming convention: this document uses **dma-buf** for the Linux buffer-sharing
primitive and **dmabuf** where the code or symbols spell it that way, such as
`hwnd_dmabuf`.

## High-Level Data Flow

The central design is a per-HWND frame channel managed by wineserver:

```text
Windows app / rendering API
        |
        | renders, copies, or draws into an exportable buffer
        v
win32u producer side
        |
        | hwnd_dmabuf frame descriptor + fd over socketpair
        v
wineserver window object
        |
        | channel ownership, producer counts, frame discovery
        v
winewayland.drv consumer side
        |
        | import SHM/dma-buf, attach wl_buffers, arrange subsurfaces
        v
Wayland compositor
```

The server does not move pixels. It owns the window hierarchy, visibility state,
channel objects, and clipping semantics. Producers publish frame descriptors and
file descriptors. The Wayland owner imports those descriptors and builds the
surface tree.

## Frame Lifecycle

A cross-process frame normally goes through this lifecycle:

1. The producer asks whether the target HWND can import a format/modifier pair.
   Vulkan uses the Wayland Vulkan caps hook; wined3d uses the Wine-internal WGL
   HWND dmabuf helpers.
2. The producer opens a channel for the HWND. Normal producers can coexist with
   GDI overlays; exclusive channels are used when a producer must own the HWND
   content path.
3. The producer publishes `hwnd_dmabuf_frame_desc_t` plus a dma-buf fd, or an
   SHM descriptor for GDI paths.
4. The Wayland owner lists active frames below the host HWND, claims channels,
   imports new buffers, and arranges the Wayland surface tree.
5. When the compositor releases an attached wl_buffer, the consumer returns the
   matching `release_token` to the producer channel.
6. The producer drains releases and only then reuses busy slots.

Stable-slot producers export the same image identity for a ring slot across
frames, so the consumer can keep the wl_buffer and later reuse it without a new
fd. Non-stable producers send a fresh fd per frame, and the consumer releases
the wl_buffer after Wayland releases it.

## Worked Example: Cross-Process CEF Frame

Consider a launcher whose main Win32 toplevel is owned by process A, while a CEF
GPU/render process B presents the browser content for a child HWND.

1. Process A owns the Wayland `xdg_toplevel`. It is the consumer because only it
   can attach buffers to the visible Wayland surface tree.
2. Process B creates a rendering surface for the child HWND. If it only probes
   formats, no hole should be punched yet. Once it creates a real swapchain or
   otherwise opens a producer channel, the server can treat the child as
   self-presenting.
3. The server computes the parent clip. The GDI surface for the toplevel gets a
   punch-through hole where the browser frame should appear.
4. Process B publishes a frame descriptor and a dma-buf fd over the HWND dmabuf
   channel. For Vulkan managed swapchains this is usually a stable ring slot;
   for wined3d OpenGL it is usually a freshly exported GL dma-buf.
5. Process A lists frames below the toplevel, claims the channel, imports the
   frame, and decides how to display it: direct-to-toplevel, one child
   subsurface, or multiple slices for a shaped/occluded region.
6. If native GDI controls are painted above the browser area, the GDI overlay
   path publishes those pixels as a separate SHM plane stacked above the browser
   producer.
7. When the compositor releases the wl_buffer, process A sends the release token
   back to process B. Only then may process B reuse a busy slot.

This example is the same architecture used by the other producers. What changes
is how the frame buffer is obtained; the server channel, consumer import,
surface arrangement, and release-token rules stay the same.

## Producer Matrix

The producer role is about the handoff point into Wineland, not about where the
pixels were first generated:

| Producer path | What it actually provides | Transport | Typical frame identity | Main fallback |
| --- | --- | --- | --- | --- |
| Vulkan direct WSI | The app/driver presents directly to the host Wayland swapchain. | Host `VkSwapchainKHR` | Host driver owned | Normal Wayland WSI |
| Vulkan managed swapchain | Wine allocates exportable Vulkan images and publishes the presented image. | HWND dma-buf channel | Stable Vulkan image slots | Direct host swapchain |
| wined3d OpenGL | Wine copies the wined3d back buffer into an exportable GL texture and publishes it. | HWND dma-buf channel via WGL helpers | Usually fresh exported GL dma-buf | Existing wined3d window update path |
| Foreign GDI | A process-local GDI surface hands off already-drawn SHM pixels for a foreign HWND. | HWND dma-buf SHM channel | SHM frame content | Ordinary process-local GDI surface |
| GDI overlay | GDI pixels that must appear above a producer hole are published as an overlay plane. | HWND dmabuf overlay channel | SHM overlay frame | No overlay; parent surface clipping decides visibility |

## Resource Lifetime Rules

Several invariants are intentionally repeated across the implementation:

- A producer slot is busy until the matching release token returns.
- A stable slot may be cached only when the producer promises stable image
  identity.
- A wl_buffer listener owns one compositor release for one attach.
- A channel close invalidates the cached caps and ring state.
- Producer ids and ring generations distinguish recreated swapchains from old
  releases.
- GDI overlay pixels are retained only for regions still above a producer.
- A transparent carrier is content-neutral and may be reused while size matches.
- Role destruction must detach Wayland objects before dropping Wine-side state.
- Server visibility and clipping are authoritative; driver-side caches are
  performance hints, not policy.

## Threading and Locking

The cross-process paths use a few different synchronization layers:

- wineserver owns HWND hierarchy, clipping policy, producer-channel objects, and
  channel discovery;
- producer and consumer processes communicate frame descriptors and file
  descriptors through socketpair-backed channels;
- win32u producer paths use per-object locks for swapchain/ring state and a
  process-wide producer-device lock where host driver queue operations need
  external synchronization;
- the Wayland consumer keeps compositor release ownership local to wl_buffer
  listeners and returns release tokens to producers only after Wayland release.

The important design rule is that ownership flags are latched where the owner
can make the decision. The server decides visibility and producer presence, the
producer owns slot reuse, and the Wayland owner owns wl_buffer lifetime.

## Adding a New Producer Path

A new producer should follow the existing shape:

1. Decide whether the target HWND can import the frames by querying caps before
   allocating long-lived export resources.
2. Provide a fallback for missing caps, unsupported formats/modifiers, failed
   imports, and broken channels.
3. Open the appropriate HWND dmabuf channel only when the path is actually going
   to publish frames.
4. Fill `hwnd_dmabuf_frame_desc_t` with stable producer identity, ring/image
   identity, size, format, modifier, dirty region, alpha/HDR metadata, and a
   nonzero release token.
5. Mark slots busy until the matching release returns. Never reuse a busy
   buffer merely because a newer frame exists.
6. Close the channel and clear cached caps when the HWND, format, producer ring,
   or export resources are recreated.
7. Keep the server-visible HWND state authoritative; driver-side shortcuts must
   be cache or fallback decisions, not policy decisions.

## HWND dmabuf Protocol

This section formalizes the wire contract behind the publish, claim, import,
and release steps described above.

The shared protocol lives around:

- `include/wine/hwnd_dmabuf.h`
- `server/protocol.def`
- `server/window.c`
- `dlls/win32u/hwnd_dmabuf.c`

`hwnd_dmabuf_frame_desc_t` is the frame contract. It carries:

- dimensions, stride, offset, fourcc, modifier, and per-plane layout;
- `producer_unique_id`, `ring_generation`, `image_id`, and `frame_seq`;
- dirty rectangles for partial updates;
- `release_token`, returned by the consumer when the compositor releases the
  frame;
- optional DXGI color/HDR metadata;
- flags such as `HWND_DMABUF_FLAG_STABLE_SLOT`, `HWND_DMABUF_FLAG_SHM`, and
  `HWND_DMABUF_FLAG_GDI_OVERLAY`.

The server exposes requests to:

- mark an HWND as having a pending dmabuf producer;
- open normal or exclusive producer channels;
- open a GDI-overlay producer channel;
- claim the consumer end of a channel in the Wayland owner process;
- list active producer frames below a host HWND;
- release channels when producers go away.

Release tokens are the synchronization boundary between producer and consumer.
A producer must not overwrite or reuse a busy slot until its release comes back.
The consumer must return a release with enough identity fields to distinguish
old producers, recreated rings, and stale cached buffers.

Channel state is counted separately from frame state:

- **pending producer**: a swapchain/channel is being created; the server already
  treats the window as self-presenting so parent clipping can be prepared before
  the first frame arrives;
- **active producer**: a producer channel is open and can publish frames;
- **GDI overlay producer**: a separate overlay channel is open for GDI pixels
  that belong above a producer hole;
- **exclusive producer**: a producer owns the content path and excludes other
  normal producers for that HWND.

This separation lets the server distinguish "about to present", "currently
presenting", "overlay only", and "tearing down" without inspecting pixel data.

## Server-Side Window Semantics

`server/window.c` extends the window object with:

- dmabuf producer and consumer channel objects;
- GDI overlay channel objects;
- pending/producer/exclusive counts;
- paint flags for pixel-format and client-clip state.

The server decides when a window has **self-presenting content**. That includes
traditional pixel-format windows, windows whose parent client area is explicitly
clipped for a client surface, active HWND dmabuf producers, and HWNDs with a
pending dmabuf producer allocation. Self-presenting windows are not simply
painted into the parent GDI surface; instead, the parent surface must contain a
hole where the producer will appear.

The region walk produces:

- the normal parent surface region;
- punch-through holes for self-presenting children;
- a GDI-over-producer region for transparent or chrome-like GDI that must remain
  visible above a GPU producer.

This keeps Windows sibling/child clipping decisions centralized in wineserver,
where the full HWND tree is already known.

## Wayland Surface Roles

The Wayland driver maps HWND state to one of several roles:

- `WAYLAND_SURFACE_ROLE_TOPLEVEL`
- `WAYLAND_SURFACE_ROLE_SUBSURFACE`
- `WAYLAND_SURFACE_ROLE_POPUP`
- `WAYLAND_SURFACE_ROLE_LAYER`
- `WAYLAND_SURFACE_ROLE_NONE`

The role logic is mainly in:

- `dlls/winewayland.drv/window.c`
- `dlls/winewayland.drv/wayland_surface.c`

Toplevels use `xdg_toplevel`. Child/client content uses subsurfaces. Menus and
dropdowns become `xdg_popup` where possible. Tray and selected menu surfaces can
use `zwlr_layer_shell_v1` when the compositor supports it.

Role changes are conservative. A live toplevel should not be destroyed merely
because Windows briefly reports transition state during minimize, restore,
fullscreen, or alt-tab. Explicit `SWP_HIDEWINDOW` still wins; transient
visibility loss during state changes is treated as noise for already-mapped
toplevels.

## Wayland Consumer for HWND Frames

The consumer side in `wayland_surface.c` imports frames found under a host HWND.
It supports:

- dma-buf import via `zwp_linux_dmabuf_v1`;
- SHM import for GDI/foreign-GDI paths;
- stable-slot wl_buffer caching;
- per-commit refs so wl_buffer release is matched to producer release tokens;
- fallback to transparent carriers when the app has not produced real pixels.

The consumer distinguishes several cases:

- **Full rectangular frame**: attach one imported buffer to the child surface.
- **Direct-to-toplevel frame**: attach a full-client producer directly to the
  toplevel's own surface when no separate child surface is needed.
- **Shaped or occluded frame**: split visible regions into subsurfaces.
- **No producer frame yet**: keep the surface mapped with a transparent carrier.
- **Failed import or missing caps**: fall back without corrupting producer state.

The carrier avoids committing Wine's freshly allocated white/black SHM buffer as
visible application content before the application has actually painted or
presented.

The consumer keeps recently seen child frame entries alive for a short grace
window. A transiently missing child therefore does not immediately tear down the
cached surface or slot state; it is removed only if it stays absent.

## Shaped and Occluded Windows

Wayland does not offer arbitrary shaped child presentation directly equivalent
to Win32 regions. Wineland handles this by slicing the imported producer frame
into multiple child subsurfaces:

- server/window regions identify the visible producer pixels;
- `wayland_surface.c` converts visible rects into derived slice geometry;
- each slice gets its own wl_surface, wl_subsurface, and viewport;
- per-slice wl_buffers wrap the same producer image with adjusted source
  coordinates;
- input regions on slices are empty so input remains governed by Win32.

The slice path includes two performance protections:

- a layout cache using exact derived geometry comparisons, so unchanged slices
  do not resend Wayland position/viewport state every frame;
- a rectangle covering pass that can replace many region rectangles with a
  smaller exact disjoint cover before slicing.

The layout cache stores derived per-slice geometry, the sibling anchor, and the
bottom of the slice stack. Rectangles are converted through the current Wayland
surface scale before comparison, so scale changes naturally invalidate the cache
by changing the derived geometry. On a cache hit with no new producer frame, the
consumer can skip both layout traffic and buffer attaches.

The covering pass builds an edge grid from the region rectangle boundaries,
marks covered cells, emits exact disjoint rectangles, and adopts the result only
when it reduces the rectangle count. It is bounded by
`WAYLAND_DMABUF_COVER_MAX_CELLS` so pathological regions do not allocate an
unbounded grid on the presentation path.

The slice cap is `WAYLAND_DMABUF_MAX_SLICES`. If a shape is too complex or a
slice attach fails, the attempt is treated atomically and the path falls back
instead of leaving stale partial slices on screen.

## Vulkan Producer Path

The Vulkan work is primarily in:

- `dlls/win32u/vulkan.c`
- `dlls/winewayland.drv/vulkan.c`
- `include/wine/vulkan_driver.h`

There are two Vulkan presentation modes.

### Direct Host Swapchain

If the toplevel can be presented locally and does not need masking that direct
Wayland WSI cannot express, Wineland leaves the app on the normal host
swapchain path.

Pixel-format/self-presenting state is established at swapchain creation rather
than mere surface creation. This avoids punching a visible hole for applications
that create a `VkSurfaceKHR` only for probing and never create a swapchain.

### Managed Swapchain

If the HWND needs cross-process presentation or unmaskable composition, Wine
creates a managed swapchain:

- query the Wayland driver's HWND dmabuf caps;
- intersect compositor modifiers with host-exportable Vulkan image modifiers;
- allocate exportable host images;
- export each image as a dma-buf;
- provide acquire/present/wait behavior for the managed images;
- publish presented images over the HWND dmabuf channel.

Managed swapchains have no host `VkSwapchainKHR`. Functions that normally
forward a swapchain handle must therefore either handle the managed object or
return `VK_ERROR_OUT_OF_DATE_KHR` for invalid/stale handles. Timing and latency
queries are mostly stubbed to satisfy the API contract, while release-image and
HDR metadata calls are intercepted so they do not pass null host swapchains to
the driver. Swapchain status is an internal helper used by those paths, not a
separate exposed driver entry point.

The managed path is entered only when the Wayland owner reports HWND dmabuf caps
for the target HWND and the host Vulkan device can export compatible images. If
caps are missing or negotiation fails, the code silently falls back to the direct
host swapchain path. Managed channels also have a loss path: a stalled or broken
channel marks the swapchain lost, and later API calls report an out-of-date state
so the application can recreate.

The managed swapchain state machine is deliberately small:

- images start free;
- `vkAcquireNextImage*` marks one valid, non-busy image as acquired and signals
  the app's acquire semaphore/fence with an empty host submit;
- `vkQueuePresentKHR` consumes the acquired image, waits for rendering to be
  complete, assigns a fresh release token, and publishes the frame;
- published images are busy until the consumer returns a matching
  producer-id/ring-generation/image-id/release-token tuple;
- if all slots stay busy past the stall window, or the channel breaks, the
  managed swapchain is marked lost and the app is pushed toward recreation.

The ring uses at least three images and caps at `WINE_VK_MANAGED_MAX_IMAGES`.
Stable Vulkan slots can be cached by the consumer, so after the first import the
producer can publish later frames without re-sending the fd unless the consumer
reports that its cache was lost.

Host and managed swapchains can appear in the same `vkQueuePresentKHR` call.
The host subset is repacked for the real host present, while managed swapchains
publish frames through the HWND dmabuf channel.

## NVIDIA Wayland WSI Workaround

The branch contains a NVIDIA-specific workaround for a Wayland WSI bug where
destroying one Vulkan instance can tear down process-global Wayland dispatch
state while another instance still presents.

For NVIDIA Wayland instances:

- host instance destroys are delayed while any NVIDIA Wayland instance remains;
- delayed host instances are destroyed when the last one is gone;
- the workaround is gated to NVIDIA + `VK_KHR_wayland_surface`.

This is intentionally narrow and documented as a workaround. It avoids
presenting through a live instance after another instance has invalidated the
driver's Wayland dispatch table.

## wined3d OpenGL Producer Path

The OpenGL/wined3d path lets the OpenGL backend participate in the same HWND
dmabuf architecture:

- `dlls/winewayland.drv/opengl.c` exports GL textures as dma-bufs through
  Wine-internal WGL entry points;
- `dlls/wined3d/swapchain.c` captures the swapchain back buffer into an
  exportable GL texture/FBO ring;
- the ring publishes frames through the HWND dmabuf channel;
- the consumer imports them exactly like Vulkan-produced frames.

This is a fast path, not a guarantee. It currently targets exportable,
single-plane RGB content and falls back when the Wayland driver, GL driver, or
HWND caps cannot provide a compatible dma-buf path. When it succeeds,
CEF-style launchers and D3D-through-wined3d content can use the same
cross-process architecture as Vulkan producers.

The wined3d GL producer does not expose stable slots. Each present captures the
current back buffer into a GL texture/FBO, exports that texture as a dma-buf,
publishes the fd, and waits for the corresponding release before reusing that
ring image. The ring also keeps the last valid image out of the immediate reuse
set where possible, which avoids tearing down the most recent visible content
just because the next frame is being prepared.

Format negotiation is conservative:

- the GL export reports a fourcc and modifier;
- the HWND caps list is checked for that exact pair;
- linear exports may fall back to an implicit-modifier caps entry;
- alpha fourccs may fall back to an opaque equivalent when the consumer can
  import only the opaque variant;
- if none of those matches, the frame can still be published and the Wayland
  consumer performs the final validation.

The branch also carries OpenGL robustness fixes around temporary context DCs
and backup DCs. These avoid reusing a destroyed window DC when wined3d or
opengl32 needs to copy/reset context state after fake/probe windows are gone.

## WGL / DX Interop

`win32u/opengl.c` also implements a Wine-side `WGL_NV_DX_interop` model. It maps
shared D3D/KMT resources to GL memory objects/textures where the driver support
exists. This is an application-facing extension path, used by consumers such as
ANGLE/CEF. It is independent of the wined3d OpenGL producer path above, which
exports its swapchain texture through the Wine-internal dma-buf export helpers
instead of using `wglDX*` interop calls.

The implementation tracks:

- opened WGL/DX devices;
- registered objects;
- share handles;
- access mode and lock state;
- GL object deletion and error cleanup.

## GDI and Window Surfaces

GDI remains important even when GPU content is self-presenting. The branch adds
several pieces to make GDI coexist with producer holes.

### App-Painted Tracking

`window_surface` tracks which pixels originated from the application:

- `app_painted_region`
- `app_painted_full`

The Wayland SHM flush path only publishes app-originated pixels. Unpainted Wine
initialization memory is not treated as application content. This prevents
never-painted windows from showing large white planes during GPU-process startup.

The tracking is also updated for paths that dirty a surface without normal
window-DC drawing, such as scaled-surface copies and `UpdateLayeredWindow`.
Once the tracked region covers the whole surface, it collapses to
`app_painted_full` so later GDI primitives do not keep allocating and combining
regions on the hot path.

### Foreign GDI Bridge

When a different process obtains a DC for a window it does not own, win32u can
create a foreign GDI window surface. That surface publishes SHM frames over the
same HWND dmabuf channel model. This lets cross-process GDI drawing reach the
Wayland owner instead of silently painting into the wrong process-local surface.

### GDI Overlay Above Producers

Some applications paint native GDI controls over GPU/CEF content. Because the
parent surface is clipped out where a producer hole exists, those GDI pixels
need a separate overlay plane.

The server computes a GDI-over-producer region. `dce.c` accumulates actual GDI
paint into that region. `window_surface.c` publishes an SHM overlay frame with
`HWND_DMABUF_FLAG_GDI_OVERLAY`, and the Wayland consumer stacks it above the
producer frame.

Background erase and copy-bits operations are filtered separately so ordinary
erase/copy churn does not become permanent overlay content.

The overlay master buffer is retained across frames because many applications
paint native controls only when they are damaged. When the overlay region
shrinks, pixels that are no longer above a producer are cleared and marked dirty
so stale overlay content is removed from the compositor. When the geometry is
unchanged, unchanged overlay pixels remain valid without forcing the app to
repaint them every producer frame.

## DWM Occlusion and Cloaking

`dlls/dwmapi/dwmapi_main.c` contains two pieces that matter for launcher-style
UIs:

- `DwmExtendFrameIntoClientArea()` custom-frame handling;
- a foreign-window cloaking policy controlled by `PROTON_CLOAK_OCCLUDERS`.

Wine does not have a real DWM compositor, but Wayland can show windows from
other processes above the application that is doing occlusion tracking. CEF and
similar toolkits may see those foreign toplevels as opaque occluders and suspend
rendering. The cloaking path reports selected foreign opaque toplevels as
DWM-cloaked so those trackers do not treat them as app content blockers.

`PROTON_CLOAK_OCCLUDERS` accepts:

- `off`: disabled, the default;
- `cover`: cloak a foreign opaque toplevel overlapping one of this process's
  visible toplevels;
- `fs`: cloak only a foreign borderless toplevel covering a whole monitor;
- `always`: cloak any foreign visible toplevel.

## Client Surfaces

Vulkan and OpenGL client surfaces are tracked separately from ordinary SHM
window surfaces. A client surface records whether it has presented at least
once. This matters because creating a surface for probing must not immediately
make the parent transparent, while an actually presenting client must keep the
parent clipped.

The attach path handles:

- toplevel role changes;
- first presentation;
- reparenting;
- minimized windows;
- pending configure state;
- avoiding eviction of a live presented client by a later non-presenting probe.

## Minimize, Restore, and Alt-Tab

Windows minimize/restore state and Wayland `xdg_toplevel` state do not map
one-to-one. In particular, Wayland does not send a clear "unminimized" state.

When the compositor advertises minimize support, Wineland keeps already-mapped
minimized toplevels alive so the compositor can still restore them. It avoids
pushing the iconic Win32 `-32000` / tiny minimized geometry as the real Wayland
window size. Restore is driven through the foreground path when keyboard focus
re-enters a minimized surface, so stale focus bounces do not force phantom
restores. The window-state handler also has a restore path for compositor-driven
state changes.

While minimized, client producer subsurfaces are detached so a full-size stale
GPU frame does not remain visible behind other windows.

## Popups, Menus, and Layer Shell

Wayland has strict popup/grab rules, while Win32 applications often create
ownerless or ambiguous popup windows. Wineland classifies menu-like windows and
uses:

- `xdg_popup` for ordinary owned menus/dropdowns;
- explicit grabs only for native `#32768` popup-menu class windows;
- layer-shell for tray/context menus when needed and supported;
- foreground restoration for menus that temporarily live outside the normal
  owner chain.

This is especially important for WPF/CEF combo boxes, tray context menus, and
launcher UI popups.

## System Tray and SNI

The branch adds an in-process StatusNotifierItem backend:

- `dlls/win32u/sni.c`
- `dlls/shell32/systray.c`

When a StatusNotifierWatcher exists on the session bus, `Shell_NotifyIcon`
requests can be exported as SNI items directly from the application process.
The classic explorer systray remains the fallback.

SNI context menus are integrated with the layer-shell menu path when available,
but the tray backend is not Wayland-only. It also supports the classic fallback
path, deliberately avoids dbusmenu by letting the application render its own
Win32 menu, and uses a lazy D-Bus pump thread that can reap itself when idle.

## Display, Modes, EDID, and HDR

Wayland output state is collected in:

- `dlls/winewayland.drv/display.c`
- `dlls/winewayland.drv/wayland_output.c`
- `dlls/winewayland.drv/generic_output_device.c`
- `dlls/win32u/sysparams.c`

The Wayland driver reports outputs, modes, EDID, and HDR capability to win32u.
The work area is currently reported as the monitor rectangle. Physical size is
encoded in the generated EDID rather than being an independently modeled Win32
work-area concept. EDID can come from an override, sysfs, or a generated generic
EDID. HDR support combines EDID/panel capability with compositor color
management support.

Display modes are controlled by two win32u flags:

- `emulate_modeset` defaults to true. In that mode Wine keeps a single real
  current mode and treats mode changes as successful without asking the display
  server to modeset.
- `emulate_modelist` controls generation of a virtual mode list when modeset
  emulation is not active.

The physical/virtual split is therefore not always active. It is mainly used
when `emulate_modeset` is false, such as under steamcompmgr/gamescope or when
the `EmulateModeset` registry option disables modeset emulation. In that path
Wine can keep the compositor's physical mode while presenting games with a
virtual current mode. The steamcompmgr path also has a specific "fake current
mode" behavior so gamescope-style scaling can preserve the requested game mode.

Normal Wayland output handling must avoid letting a game's fake mode replace
the physical monitor mode used for raw coordinates, virtual-screen metrics, and
window placement.

The practical behavior is:

| Mode path | Mode list exposed to apps | Current mode stored by Wine | Why |
| --- | --- | --- | --- |
| Default `emulate_modeset` | Single real current mode | Real current mode | Report success for modesets without changing the compositor mode. |
| `emulate_modelist` path | Generated virtual modes | May use a virtual current mode | Let games choose common fullscreen sizes while retaining physical coordinates. |
| steamcompmgr/gamescope | Generated virtual modes plus fake current mode handling | Requested game mode can be reflected for gamescope scaling | Preserve gamescope's mode-scaling contract. |
| Normal Wayland physical state | Real compositor mode remains the physical source mode | Real physical mode must remain available | Keep raw coordinates and virtual-screen metrics sane across processes. |

## Environment and Registry Controls

| Control | Default | Area | Effect |
| --- | --- | --- | --- |
| `PROTON_USE_TABTIP` | off | Explorer/tabtip | Starts Wine's `tabtip.exe` from explorer when enabled. |
| `PROTON_CLOAK_OCCLUDERS` | `off` | DWM/CEF occlusion | Chooses the foreign-window cloaking policy: `off`, `cover`, `fs`, or `always`. |
| `DXVK_HDR` / `DXVK_NO_HDR` | runtime dependent | HDR | Participates in HDR exposure decisions together with Wayland color/HDR capability. |
| `EmulateModeset` | inverted registry option | Display | Controls `emulate_modeset`; true in the registry disables the Wine emulation flag. |
| `EmulateModelist` | inverted registry option | Display | Controls virtual modelist generation when modeset emulation is inactive. |

## Input and Hot-Path Optimizations

The branch includes smaller optimizations that matter for launcher performance:

- `GetKeyState` avoids server synchronization when mouse movement did not
  actually change any key-state bytes.
- `get_window_from_point` can return the first hit directly instead of building
  a heap-allocated full handle list for tiny hit-test queries.
- tabtip startup is off by default and gated in explorer behind
  `PROTON_USE_TABTIP`; `windows.ui` `InputPane` caches the tabtip window lookup.
- no-op OpenGL context flushes are recognized before expensive drawable sync
  work when they cannot present a frame.

These are not the core presentation architecture, but they reduce desktop-wide
slowdowns in launcher UIs that poll input or redraw frequently.

## Entry-Point Symbols

The main ABI and internal entry points are:

- syscalls: `NtUserHwndDmaBufOpenProducer`,
  `NtUserHwndDmaBufCloseProducer`, `NtUserHwndDmaBufPublish`,
  `NtUserHwndDmaBufDrainRelease`, and `NtUserHwndDmaBufGetCaps`;
- server requests: `hwnd_list_dmabuf_frames`, `hwnd_dmabuf_set_pending`,
  `hwnd_dmabuf_get_channel`, `hwnd_dmabuf_get_channel_exclusive`,
  `hwnd_dmabuf_claim_channel`, and `hwnd_dmabuf_release_channel`;
- WGL helpers: `wglWineDmaBufExportSupportedWINE`,
  `wglWineExportDmaBufWINE`, `wglWineCloseDmaBufWINE`,
  `wglWineHwndDmaBufOpenProducerWINE`, `wglWineHwndDmaBufGetCapsWINE`,
  `wglWineHwndDmaBufPublishWINE`, and
  `wglWineHwndDmaBufDrainReleaseWINE`;
- driver hooks: the Wayland Vulkan dmabuf caps hook and the OpenGL dma-buf
  export hooks in `winewayland.drv`.

The generated wow64/opengl thunk files mirror the WGL helper ABI for 32-bit
clients.

## Fallback Taxonomy

| Failure or unsupported case | Fallback behavior |
| --- | --- |
| No HWND dmabuf caps | Use the normal host swapchain or normal SHM/GDI path. |
| Incompatible Vulkan modifier/export support | Do not create a managed swapchain; use direct WSI. |
| Failed Wayland dma-buf import | Fall back for that frame without reusing stale slice content. |
| Too many or failed shape slices | Clear attempted slices and use the simpler path. |
| Never-painted SHM surface | Attach a transparent carrier instead of Wine-initialized pixels. |
| Missing layer-shell support | Use the normal popup/menu fallback. |
| Broken/stalled managed channel | Mark the swapchain lost and report out-of-date to force recreation. |

## Verifying Changes

There is no single automated test suite for this architecture. Practical
verification is app-driven:

- use a Vulkan/DXVK or vkd3d title for direct vs managed swapchain behavior;
- use a CEF-based launcher for cross-process browser content, GDI overlay,
  slicing, DWM cloaking, and tabtip/input behavior;
- use a wined3d OpenGL title for the OpenGL producer path;
- use tray-heavy launchers for SNI and menu positioning;
- use fullscreen games and gamescope/steamcompmgr for mode and alt-tab changes.

Run with the relevant channels from the debugging map below and check both the
visible behavior and the lifetime signals: channel open/close, frame publish,
wl_buffer release, release-token drain, role changes, and fallback paths.

## Debugging Map

Useful channels depend on the subsystem:

- `+waylanddrv`: Wayland roles, dmabuf import, slicing, carriers, popups,
  display, output, and SNI/layer interaction.
- `+vulkan`: managed swapchains, host-vs-managed presents, timing/latency
  interposition, NVIDIA workaround behavior.
- `+d3d,+d3d_perf`: wined3d GL swapchain capture, backup-DC fallback, GL
  present behavior.
- `+opengl`: WGL context/drawable behavior and dmabuf export helpers.
- `+dwmapi`: foreign-window cloaking and custom-frame behavior.
- `+systray`: SNI registration, D-Bus watcher state, tray context menus.
- `+win,+event,+input`: window state, hit testing, input polling, and focus.

For Wayland protocol-level questions, compositor `WAYLAND_DEBUG=1` traces are
useful, but most Wineland decisions are visible in the Wine channels above.

## Main File Map

| Area | Files |
| --- | --- |
| HWND dmabuf protocol | `include/wine/hwnd_dmabuf.h`, `dlls/win32u/hwnd_dmabuf.c`, `server/window.c`, `server/protocol.def` |
| Wayland consumer | `dlls/winewayland.drv/wayland_surface.c`, `dlls/winewayland.drv/window.c`, `dlls/winewayland.drv/window_surface.c` |
| Vulkan producer | `dlls/win32u/vulkan.c`, `dlls/winewayland.drv/vulkan.c`, `include/wine/vulkan_driver.h` |
| wined3d/OpenGL producer | `dlls/wined3d/swapchain.c`, `dlls/wined3d/context_gl.c`, `dlls/winewayland.drv/opengl.c`, `dlls/win32u/opengl.c`, `dlls/opengl32/make_opengl`, `dlls/opengl32/unix_thunks.c` |
| GDI surfaces and overlays | `dlls/win32u/dce.c`, `dlls/win32u/dibdrv/dc.c`, `dlls/winewayland.drv/window_surface.c`, `include/wine/gdi_driver.h` |
| Display and HDR | `dlls/winewayland.drv/display.c`, `dlls/winewayland.drv/wayland_output.c`, `dlls/winewayland.drv/generic_output_device.c`, `dlls/win32u/sysparams.c` |
| DWM occlusion | `dlls/dwmapi/dwmapi_main.c` |
| Popups/layer/tray | `dlls/winewayland.drv/window.c`, `dlls/winewayland.drv/wayland_surface.c`, `dlls/win32u/sni.c`, `dlls/shell32/systray.c` |
| Input/perf polish | `server/queue.c`, `server/window.c`, `dlls/win32u/input.c`, `dlls/win32u/window.c`, `dlls/windows.ui/inputpane.c`, `programs/explorer/desktop.c` |
| 32-bit thunk/ABI support | `dlls/opengl32/make_opengl`, `dlls/opengl32/unix_thunks.c`, `dlls/wow64win/user.c` |

## Out of Scope for This Document

The following are intentionally not covered here:

- winhttp/wininet/secur32/schannel/nsiproxy networking backports;
- unrelated media fixes;
- packaging and distribution scripts;
- generic Wine user documentation;
- unrelated Proton runtime behavior;
- compositor implementation details outside Wine.

Those areas should be documented separately so this file remains focused on the
Wayland rendering and windowing architecture.
