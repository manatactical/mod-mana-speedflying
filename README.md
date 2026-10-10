# mod-mana-speedflying

An AzerothCore module that varies a flying mount's speed with how it is flying, as a server-side
companion to the client's **wxl-mount-anim** module:

| state      | speed |
|------------|-------|
| climbing   | 200%  |
| leveling   | 280%  |
| diving     | 380%  |

`wxl-mount-anim` already animates the mount as climbing / gliding / diving, so the two agree: wings
beat slowly on the way up, the glide rushes on the way down. The speed does not snap between states:

- **Diving** accelerates. The speed builds into `DiveSpeed` over `DiveMs`; it kicks off the line
  so the dive responds at once, then keeps accelerating as it goes.
- **Diving, then leveling** is a long glide over `GlideMs`, scaled by how much speed is actually
  being shed: pulling out of a shallow dive recovers sooner than a full `DiveSpeed` dive does.
  The mount keeps most of its dive speed at first and only bleeds it off hard near the end, before
  settling back to `LevelSpeed`.
- **Diving, then climbing** brakes over the shorter `ClimbDecelMs` (scaled the same way), so pulling
  up sheds speed sooner than leveling would.
- Leaving a **hover** (a parked mount) uses a fixed, tiny `HoverMs` zone whatever it heads into --
  climb, level or dive -- so a take-off responds at once instead of ramping from a standstill.
- Every other change blends over `BlendMs`, spread over how far the speed has to move.

## How it binds to the client module

The client module does the thinking and tells the server directly, over the addon channel (the only
channel a native client module has):

```
WXLMS<TAB><advfly:0|1><state:L|C|D|H>
```

- `advfly` is `1` only for mounts whose model carries the retail **AdvFly** clips -- exactly the
  mounts `wxl-mount-anim` drives with AdvFly. This is what keeps the speed change off stock mounts
  and druid flight forms, with no server-side mount list. A `0` message tells the server to restore
  the mount's own speed and stand down.
- `state` is `C` climb, `D` dive, `H` hovering, `L` level / strafe / backward.

The client sends on every state change, announces the mount's capability when a mount is loaded, and
heartbeats about once a second. On the server the message is picked up by
`PlayerScript::OnPlayerBeforeSendChatMessage` (addon messages, `LANG_ADDON`), and a periodic
`OnPlayerUpdate` blends `MOVE_FLIGHT` toward the state's speed.

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
ManaSpeedFlying.DiveSpeed    = 3.8    ; 380% (old .DescendSpeed key still read)
ManaSpeedFlying.BlendMs      = 5500   ; default blend, proportional to the span it covers
ManaSpeedFlying.DiveMs       = 4000   ; accelerate into a dive
ManaSpeedFlying.GlideMs      = 9000   ; long glide from a dive back to level
ManaSpeedFlying.ClimbDecelMs = 2500   ; dive -> climb brake, shorter than the glide
ManaSpeedFlying.HoverMs      = 500    ; fixed accel/decel zone leaving a hover
ManaSpeedFlying.SendMs       = 150
ManaSpeedFlying.TimeoutMs    = 2500   ; restore the mount's own speed if the link goes quiet
```

- **BlendMs** is the time for the *whole* climb-to-dive span. The rate is linear, so a smaller
  hop (for example level to climb) takes proportionally less. Raise it for a longer blend.
  **GlideMs** and **ClimbDecelMs** are likewise scaled by the speed actually being shed, so a
  shallow pull-out recovers faster than a full dive's.
- The speeds are multipliers of base run speed, the same scale `SetSpeed(MOVE_FLIGHT, ...)` uses: a
  stock 280% mount is `2.8`.
- Only flying players are touched, and only the forward flight speed; ground speed, taxis and
  vehicles are left alone.

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
