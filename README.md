# DishonoredBorderless

**Downloads are on [Nexus Mods](https://www.nexusmods.com/dishonored/mods/379?tab=description)** — this repository is the source. To build it
yourself, see [Build](#build); to just use the mod, get the release archive and
follow its `README.txt`.

A Dishonored 1-specific borderless windowed mod: a 32-bit `d3d9.dll` proxy that
forces the game's Direct3D 9 device into windowed mode and then holds its window
borderless and desktop-sized against the engine's own window management.

Implements [`plan.md`](plan.md) — the Option C + Option B hybrid from its
"Recommended Strategy" section. [`FINDINGS.md`](FINDINGS.md) records what was
measured about the game, why off-the-shelf tools fail against it, and the
silent failure modes worth knowing about.

## Why generic borderless tools fail here

Dishonored disables window resizing and re-asserts its own window style, so the
usual external recipe (find window → strip borders → resize to desktop) is
undone from inside the process, which is where the widely reported flicker and
forced minimising come from. Fixing it from outside is not really possible; this
runs *inside* the game process instead, on both of the layers that matter:

| Layer | What it does |
| --- | --- |
| D3D9 | `IDirect3D9::CreateDevice` and `IDirect3DDevice9::Reset` are intercepted; `D3DPRESENT_PARAMETERS.Windowed` is forced to `TRUE` and the backbuffer sized to the monitor, so the game never takes exclusive fullscreen. |
| Win32 | The game window is subclassed. Style changes, position changes and non-client frame calculation are overridden as they happen, so the game's attempts to restore its own frame never take visible effect. |
| Keyboard | A `WH_KEYBOARD_LL` hook restores the plain Win key (opens Start menu) and Alt+F4 (closes the game), both of which Windows suppresses when a `WS_POPUP` window covers the full monitor. Alt+Tab works without intervention. Win+key combinations are not supported. |

### Verified against this install

- `Dishonored.exe` is PE machine `0x14C` — **32-bit**. The proxy must be 32-bit.
- It statically imports exactly four symbols from `d3d9.dll`:
  `Direct3DCreate9`, `D3DPERF_BeginEvent`, `D3DPERF_EndEvent`,
  `D3DPERF_SetOptions`. So the renderer is confirmed D3D9 (plan.md 4.2's open
  question), and a proxy DLL in `Binaries\Win32` is loaded ahead of the system
  one, because `d3d9.dll` is not a KnownDLL and the application directory is
  searched first.

## Build

### Toolchain

You need a **32-bit** C++ toolchain. A 64-bit-only mingw-w64 will not do — it
has no 32-bit runtime libraries, and `-m32` fails at link time.

```powershell
scoop install mingw-mstorsjo-llvm-ucrt     # llvm-mingw; hosts x86_64, targets i686 too
```

Alternatives: `scoop install mingw-winlibs -a 32bit` (gcc, i686 only — note it
provides its own `g++` shim and will shadow an existing one), or Visual Studio
Build Tools with the C++ workload, running `build.ps1` from an *x86 Native Tools
Command Prompt*.

### Build, test, install

```powershell
.\build.ps1                     # compiles, then verifies the PE it produced
.\tests\Verify-Borderless.ps1   # runs the proxy against a D3D9 test app
.\install.ps1                   # copies it into Dishonored\Binaries\Win32
.\install.ps1 -Uninstall        # removes it again
```

`build.ps1` ends by re-reading `build\d3d9.dll` and asserting three things that
otherwise fail *silently* — the game simply refuses to start, with no message:

1. machine type is `x86`;
2. the exports are named `Direct3DCreate9` etc., **not** `Direct3DCreate9@4`
   (`__stdcall` decoration; handled by `src/d3d9.def` plus `-Wl,--kill-at`);
3. nothing outside the system DLL set is imported — no `libc++.dll`,
   `libunwind.dll` or `libwinpthread-1.dll` that would have to ship alongside.

`tests\Verify-Borderless.ps1` (plan.md 4.1, 4.5.1) builds a small D3D9 app that
mimics the game — 32-bit, static `Direct3DCreate9` import, decorated window,
device requested as exclusive fullscreen at 1280×720 — drops the proxy beside
it, and then asserts **from a separate process** that the window has no caption
and no resizing frame, sits exactly where it should, has a client area equal to
its whole window rect, and a backbuffer matching that client area.

Mid-run the app calls `Reset` asking to go back to exclusive fullscreen, the way
the game's own video options do, and the geometry is re-checked at exit — so a
proxy that wins the first exchange and loses a later one still fails. Two
scenarios run: the default fill-the-monitor case, and `TargetMonitorIndex=2` at
an explicit 1600×900, which is the only run that exercises monitor selection and
the centring arithmetic against a non-zero monitor origin (skipped on a
single-display setup, or with `-SkipMultiMonitor`).

The app briefly covers a display with a blue window; that is expected.

## Configuration

`DishonoredBorderless.ini` sits next to the DLL. Every key is documented inline
in that file; the ones you are most likely to touch:

| Key | Default | |
| --- | --- | --- |
| `ResolutionWidth` / `ResolutionHeight` | `0` | `0` = fill the monitor. A smaller size is centred, and the backbuffer follows it — so this doubles as a resolution override. |
| `TargetMonitorIndex` | `0` | `0` = the monitor the game opens on; `1..N` = a specific one. |
| `BlockMinimizeOnFocusLoss` | `1` | Swallows `SC_MINIMIZE`. |
| `LogLevel` | `info` | `debug` logs every style and geometry correction. |

The log goes to `DishonoredBorderless.log` beside the DLL, or to
`%LOCALAPPDATA%\DishonoredBorderless\` if the game directory is not writable.

## Troubleshooting

**The game does not start at all.** The DLL failed to load. Run `.\build.ps1`
and read its PE check output — a 64-bit build, decorated export names, or a
missing runtime import all produce exactly this symptom with no error dialog.

**Blurry image, or the window is smaller than the desktop.** The process did not
become DPI-aware. Check the log for `IsProcessDPIAware=yes`. If it says `no`, the
exe's manifest already declared an awareness and `SetProcessDpiAwarenessContext`
is a no-op — set it externally instead: right-click `Dishonored.exe` →
Properties → Compatibility → Change high DPI settings → *Override high DPI
scaling behaviour* → *Application*.

**Nothing happens and there is no log.** Another `d3d9.dll` may already be
installed (ReShade, dgVoodoo, ENB) — `install.ps1` refuses to overwrite one
without `-Force`. Chaining two d3d9 wrappers is not supported.

**`Verify-Borderless.ps1` fails with `D3DERR_DEVICELOST` (`0x88760868`).** Some
other process owns the display in exclusive fullscreen — very likely Dishonored
itself — and D3D9 will not create a second device until it lets go. Close it and
re-run. This is not a proxy fault; a plain unproxied D3D9 app fails the same way.

**Win key and Alt+F4 don't work in-game.** When a `WS_POPUP` window covers
the full monitor, Windows suppresses shell hotkeys. The mod installs a
low-level keyboard hook that restores the plain Win key (opens Start menu)
and Alt+F4 (closes the game). Win+key combinations are swallowed to avoid
side effects. Alt+Tab works without any intervention. If these still don't
work, check `DishonoredBorderless.log` for `installed low-level keyboard hook`.

**It goes borderless, then reverts.** Set `LogLevel=debug` and look for repeated
`style ... -> ...` / `enforced rect` lines. That is the watchdog fighting
something; report what is on the other side.

**Flicker or minimising on alt-tab.** Confirm `ForceWindowed` took effect —
the log should show `CreateDevice applied: Windowed=TRUE`. If the game still
minimises, `BlockMinimizeOnFocusLoss=1` should stop it.

## Design notes

**Vtable patching instead of COM wrapper objects.** plan.md 4.2.1 describes
returning "your wrapped `IDirect3D9` object". This implementation patches the
two vtable slots it needs (`IDirect3D9::CreateDevice`, `IDirect3DDevice9::Reset`)
and hands the game the genuine interface pointers. Two reasons: `IDirect3DDevice9`
has ~119 methods, and every hand-written forwarder is a chance to get a signature
subtly wrong for zero benefit at 117 of them; and anything that compares
interface pointers or hooks the same vtable — the Steam overlay does both — sees
an ordinary D3D9 object. Behaviour at the two interception points is exactly what
plan.md 4.2.2 specifies.

**No structured exception handling.** plan.md 4.3.3 suggests wrapping the window
procedure in `__try`/`__except`. mingw-w64/gcc does not support SEH on i686 and
clang's support there is incomplete, so the window procedure is instead written
to be total: every pointer from a message is null-checked, every window handle is
`IsWindow`-checked, and unhandled messages go straight to the original procedure.

**Re-entrancy.** Enforcing the style calls `SetWindowLong`/`SetWindowPos`, which
synchronously re-enter our own window procedure via `WM_STYLECHANGING` and
`WM_WINDOWPOSCHANGING`. `Win_Enforce` holds an interlocked guard so those
re-entries adjust the message in flight rather than recursing.

**`WM_NCCALCSIZE` returns 0.** This makes the client area equal the whole window
rect, which is what actually removes the frame — and it keeps working even if the
game re-adds `WS_CAPTION` faster than we can strip it.

**The vtable slot numbers are load-bearing.** `kSlot_CreateDevice` and
`kSlot_Reset` in `src/hooks_d3d9.cpp` are both 16, fixed by COM's ABI and the
declaration order in `d3d9.h`. `PatchSlot` checks that the memory is committed,
which is not the same as checking that the slot is the right one — a wrong index
would swap some other function pointer and crash rather than report anything.
Their correctness is asserted only by `Verify-Borderless.ps1` passing.

## Scope

Implemented: plan.md 4.2 (D3D9 proxy), 4.3 (WndProc hook), 4.6 (all three
contingencies — watchdog fallback, re-attach on window recreation, and
`EnableWndProcHook=0` / `Enabled=0` to disable either layer), plus a
`WH_KEYBOARD_LL` hook that restores the plain Win key (Start menu) and Alt+F4,
which Windows suppresses when a `WS_POPUP` window covers the full monitor.

Not implemented: plan.md 4.4's optional GUI or CLI settings editor — the `.ini`
is edited directly. Reading resolution back out of the game's own configs is not
needed either, since the backbuffer is derived from the monitor.

Verified by `Verify-Borderless.ps1` against the test app: the 32-bit build and
its export/import surface, DPI awareness, `CreateDevice` and `Reset` both
rewritten from exclusive fullscreen to windowed, the subclass and its
style/geometry enforcement, monitor selection, an explicit non-fullscreen
resolution, and the geometry surviving a mid-run `Reset`.

Verified in the real game, from `DishonoredBorderless.log` over several play
sessions:

- The proxy loads, forwards to `C:\WINDOWS\system32\d3d9.dll`, and becomes
  DPI-aware (`IsProcessDPIAware=yes`, virtual screen 5760x2160).
- `CreateDevice` arrives as `Windowed=FALSE 3840x2160` and is applied as
  `Windowed=TRUE`. The window it attaches to is class
  `LaunchUnrealUWindowsClient`.
- A mid-session `Reset` also arrived as `Windowed=FALSE` and was rewritten the
  same way, so the interception holds after startup. What triggered it was not
  recorded.
- **Resolution changes from the game's own video options work (1.0.1).** Three
  toggles between 1080p and 4K in one session, every `Reset requested` /
  `Reset applied` pair identical but for `Windowed`. The same session also
  *started* at 1920x1080 — a non-native startup resolution, which the test app
  does not cover — and `CreateDevice` preserved it.

  Under 1.0.0 this was broken: the same action logged
  `Reset requested: … 1920x1080` against `Reset applied: … 3840x2160`, and the
  game rendered at quarter size in a corner over stale frames, leaving the pause
  menu unusable. See the note in `RewritePresentParams` for why rewriting the
  backbuffer behind the game's back cannot work.
- Window teardown and detach are clean.
- Alt-tab is instant, with none of the display re-mode it caused before. The
  game auto-opens its pause menu on focus loss, which is the game's own
  behaviour.

Still unmeasured: input latency and raw mouse capture (plan.md 4.5.3). The
keyboard hook adds a negligible per-keystroke cost (one `FindWindowW` +
`PostThreadMessage`) that should not be perceptible.

## Release

```powershell
.\package.ps1              # -> dist\DishonoredBorderless-1.0.0.zip
```

`release\` holds the user-facing files: a plain-text `README.txt` and
`LICENSE.txt`. That is deliberately all.

**The shipped package contains no installer scripts (removed in 1.0.2).** It
used to carry `Install.ps1` plus `.bat` wrappers — the wrappers being necessary
because a downloaded zip's files carry a Mark-of-the-Web, and an unsigned `.ps1`
is then refused under any execution policy stricter than `Unrestricted`, so
`powershell -ExecutionPolicy Bypass -File` was the only thing that made a
double-click work.

That line is also, verbatim, one of the most recognisable malware-dropper
signatures there is, and Nexus Mods quarantined the 1.0.1 upload. The trigger
was almost certainly the `.bat`, not the DLL: the archive was a plain ZIP, not
nested, not password-protected, and 0/70 on VirusTotal. A convenience that
copies two files into one directory is not worth arguing with an automated
scanner about, so it is gone. Manual installation was always the documented
primary method.

The root `install.ps1` remains as a *development* convenience for pushing a
build into the game; it is not shipped. If you ever reinstate a shipped
installer, test it from a fresh extraction with MOTW applied, never from the
source tree — the source tree has no MOTW, so testing in place proves nothing
about the case that actually breaks.

`package.ps1` copies `build\d3d9.dll`; it never rebuilds it. Run `build.ps1`
deliberately, re-test, and only then package — so that whatever ships is bytes
somebody actually ran, not a build produced as a side effect of zipping. Note
that a rebuild invalidates two external things that only a human can redo: any
published VirusTotal permalink (keyed to the old hash) and any "verified in the
real game" claim. It refuses to package if `tools\Check-Pe.ps1` fails, or
if the SHA-256 quoted in `release\README.txt` does not match the DLL — that hash
is what a stranger checks before trusting an unsigned `d3d9.dll`, and a stale
one is worse than none.
