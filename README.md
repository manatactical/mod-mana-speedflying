# mod-mana-speedflying

An AzerothCore module that varies a flying mount's speed with how it is flying, as a server-side
companion to the client's **wxl-mount-anim** module:

| state      | speed |
|------------|-------|
| climbing   | 200%  |
| leveling   | 280%  |
| diving     | 800%  |

`wxl-mount-anim` already animates the mount as climbing / gliding / diving, so the two agree: wings
beat slowly on the way up, the glide rushes on the way down. The speed does not snap between states:

- **Diving** accelerates. The speed builds into `DiveSpeed` over `DiveMs`; it kicks off the line
  so the dive responds at once, then keeps accelerating as it goes.
- **Diving, then leveling** is a long glide over `GlideMs`, scaled by how much speed is actually
  being shed: pulling out of a shallow dive recovers sooner than a full `DiveSpeed` dive does.
  The mount keeps most of its dive speed at first and only bleeds it off hard near the end, before
  settling back to `LevelSpeed`.
- **Diving, then climbing** holds the dive speed for a moment over `ClimbDecelMs` (scaled the same
  way) so the built-up speed can be felt, then brakes hard into `ClimbSpeed`.
- **Diving, then climb, then level** (the pull-up is cut short) still carries the leftover dive
  speed: it glides out over `GlideMs` like a pull-out instead of blending like a fresh climb. A
  boost spent during that climb is folded into this glide too, so the momentum is not lost.
- Leaving a **hover** (a parked mount) uses a fixed, tiny `HoverMs` zone whatever it heads into --
  climb, level or dive -- so a take-off responds at once instead of ramping from a standstill.
- Every other change blends over `BlendMs`, spread over how far the speed has to move.

## How it binds to the client module

The client module does the thinking and tells the server directly, over the addon channel (the only
channel a native client module has):

```
WXLMS<TAB><advfly:0|1><state:L|C|D|H|B>
```

- `advfly` is `1` only for mounts whose model carries the retail **AdvFly** clips -- exactly the
  mounts `wxl-mount-anim` drives with AdvFly. This is what keeps the speed change off stock mounts
  and druid flight forms, with no server-side mount list. A `0` message tells the server to restore
  the mount's own speed and stand down.
- `state` is `C` climb, `D` dive, `H` hovering, `L` level / strafe / backward, and `B` the
  client's button boost. `B` spends a stack and adds `BoostAdd` to the mount's current speed: a
  burst that fades over `BoostFadeMs`, plus a slice (`BoostCarryPct`) folded into the running
  glide so the speed is still carried seconds later. A climb or hover keeps its own transition.

The client sends on every state change, announces the mount's capability when a mount is loaded, and
heartbeats about once a second. On the server the message is picked up by
`PlayerScript::OnPlayerBeforeSendChatMessage` (addon messages, `LANG_ADDON`), and a periodic
`OnPlayerUpdate` blends `MOVE_FLIGHT` toward the state's speed.

### The server answers with its speeds

The client scales its speed bar, its speed particles and its dive shake to the top dive speed, so
it has to know what that speed is. On the same `WXLMS` prefix the server replies with its configured
values:

```
WXLMS<TAB>S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,<stackDiveMs>,<stackAccumMs>,<stackIntervalMs>
                                       e.g. WXLMS\tS:280,800,1600,0,3,4000,2,9000,3000,1800,9000
```

The client reads the last addon message a few times a second and uses `levelPct`/`divePct` as the
empty and full ends of every speed effect, with `boostPct` as the boost ceiling and the two stack
intervals as the recharge cadence. The trailing `stackAccumMs`/`stackIntervalMs` are how far the
server has already accrued toward the next stack and the cadence it accrued them at: the client
anchors the orb's recharge bar to that exact position on every announce and only interpolates
locally in between, so a spent or newly granted stack does not restart the bar on the client. (An
older server that omits the two fields leaves the client running its own clock.) The reply goes out
on the first link message and again every few seconds (which also recovers a client UI reload), so
changing `ManaSpeedFlying.DiveSpeed` on the server is enough -- no matching edit in
`wxl-mount-anim.ini` is needed. If the module is not installed, the client falls back to its own
`GlideSpeedPct` / `MaxSpeedPct` defaults.

## Requirements

- Client: `wxl-mount-anim` with `ServerLink=1` in `wxl-mount-anim.ini` (default on).
- Server: `CONFIG_ADDON_CHANNEL` enabled (AzerothCore default) so the addon whisper is accepted.

## Install

Copy the `mod-mana-speedflying` folder into your AzerothCore `modules/` directory and rebuild the
worldserver. The build copies `mod-mana-speedflying.conf.dist` into `configs/`; rename it to
`mod-mana-speedflying.conf` and edit the values there. The module is picked up automatically (no
`CMakeLists.txt` is needed on current AzerothCore).

## Configuration

All keys live under `[worldserver]`; see `conf/mod-mana-speedflying.conf.dist` for the full
comments. Defaults:

```ini
ManaSpeedFlying.Enable       = 1
ManaSpeedFlying.ClimbSpeed   = 2.0    ; 200%
ManaSpeedFlying.LevelSpeed   = 2.8    ; 280%
ManaSpeedFlying.DiveSpeed    = 8.0    ; 800%
ManaSpeedFlying.BlendMs      = 5500   ; default blend, proportional to the span it covers
ManaSpeedFlying.DiveMs       = 4000   ; accelerate into a dive
ManaSpeedFlying.GlideMs      = 18000  ; long glide from a dive back to level
ManaSpeedFlying.ClimbDecelMs = 3500   ; dive -> climb: hold the speed, then brake hard
ManaSpeedFlying.HoverMs      = 500    ; fixed accel/decel zone leaving a hover
ManaSpeedFlying.SendMs       = 150
ManaSpeedFlying.TimeoutMs    = 2500   ; restore the mount's own speed if the link goes quiet
ManaSpeedFlying.BoostAdd     = 4.0    ; 400%: speed one boost adds to the glide
ManaSpeedFlying.BoostSpeed   = 16.0   ; 1600%: hard boost ceiling (>= DiveSpeed)
ManaSpeedFlying.BoostFadeMs  = 4000   ; how fast the burst bleeds back to the state speed
ManaSpeedFlying.BoostCarryPct = 0.20  ; fraction of the boost folded into the glide
ManaSpeedFlying.BoostMaxStacks  = 3   ; 0..3, recharged on a timer
ManaSpeedFlying.StackIntervalMs     = 9000 ; normal-flight stack recharge
ManaSpeedFlying.StackDiveIntervalMs = 3000 ; diving stack recharge
```

- **BlendMs** is the time for the *whole* climb-to-dive span. The rate is linear, so a smaller
  hop (for example level to climb) takes proportionally less. Raise it for a longer blend.
  **GlideMs** and **ClimbDecelMs** are likewise scaled by the speed actually being shed, so a
  shallow pull-out recovers faster than a full dive's.
- The speeds are multipliers of base run speed, the same scale `SetSpeed(MOVE_FLIGHT, ...)` uses: a
  stock 280% mount is `2.8`.
- Only flying players are touched, and only the forward flight speed; ground speed, taxis and
  vehicles are left alone.

### The button boost

Boost stacks recharge on a timer: one every `StackIntervalMs` (9 s) in normal flight, or every
`StackDiveIntervalMs` (3 s) while diving, up to `BoostMaxStacks`. The timer only runs while the mount
is link-eligible (flying, AdvFly mount), so a grounded mount earns nothing. Each press of the client's
boost button (can be used while climbing, diving or gliding level) spends one stack and adds `BoostAdd` (400% by
default) to the mount's current speed, up to the `BoostSpeed` ceiling. There is no cooldown, so held
stacks can be chained into one bigger burst. `BoostFadeMs` is how long the burst takes to bleed back
to the glide the mount already had, and `BoostCarryPct` folds a slice of every boost into the
ordinary glide (it decays on the long `GlideMs` curve), so a boost is still felt seconds after the
burst is gone. A boost spent while climbing is not folded *during* the climb: a climb is not a glide,
so the whole boost is the short burst and the speed settles back to `ClimbSpeed` on the climb's own
transition. The moment you level out, however, the still-fading burst is folded into the level glide,
so the speed you built climbing is carried onto the long `GlideMs` curve instead of emptying in a few
seconds.

## Notes

- The client→server channel is an addon whisper to self. It is throttled on the client (about one
  message per 200 ms, plus a 1 s heartbeat) and carries no gameplay authority: the server only ever
  sets a flight speed. A forged message can at worst ask for one of the three configured speeds on
  a flying player.
- If the client module is unloaded, disabled or crashes mid-flight, `TimeoutMs` restores the mount's
  own speed and the module stands down until the next link message.
- Some servers apply mount-speed auras that recompute the flight rate when mounting or on buffs. The
  module re-applies its blended value on the next update, so the two may briefly fight; disable one
  or the other if that shows.

## Build

Standard AzerothCore module: drop into `modules/` and build the worldserver. No extra dependencies.
