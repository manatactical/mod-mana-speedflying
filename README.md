# mod-mana-speedflying

An AzerothCore module that varies a flying mount's speed with how it is flying, as a server-side
companion to the client's **wxl-mana-speedflying** module:

| state      | speed |
|------------|-------|
| climbing   | 200%  |
| leveling   | 280%  |
| diving     | 800%  |

`wxl-mana-speedflying` already animates the mount as climbing / gliding / diving, so the two agree: wings
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
  boost spent during that climb is banked and released onto that glide too, so the momentum is not
  lost.
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
  mounts `wxl-mana-speedflying` drives with AdvFly. This is what keeps the speed change off stock mounts
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
`wxl-mana-speedflying.ini` is needed. If the module is not installed, the client falls back to its own
`GlideSpeedPct` / `MaxSpeedPct` defaults.

## The talent protocol -- `WXLTT`

The tree rides a **second prefix**, so the `WXLMS` link parser is untouched and an older client keeps
working against this server. Append only; never reorder or renumber a field.

```
client -> server:  WXLTT<TAB>W<TAB><talentId>   learn one rank
                   WXLTT<TAB>R<TAB><1|2|3|A>    refund a branch, or A for everything
                   WXLTT<TAB>Q                  request the current tree
	server -> client:  WXLTT<TAB>T<TAB><points>,<spent>,<unspent>[,<id>:<rank>,...]
                   WXLTT<TAB>F<TAB><reason>     a refusal, echoed for the panel
```

The tally's trailing `id:rank` pairs are a list, so a new talent needs no protocol change, and the
`F` reply means a refusal is always answered rather than silently dropped -- that is what stops the
panel drifting out of step with the server. A tally is sent after every `W`, `R` and `Q`.

Every write validates in this order, inside one lock: config enabled, the player is on a mount, the
character has a progress row, the talent id is known, `rank + 1 <= maxRank`, the prerequisite is at
full rank, the tier is unlocked, and only then is the derived unspent count checked. A forged learn
message therefore costs exactly what a real one would.

D5 **Aftershock** is the one client→server message that is not the tree itself: it reuses the
`WXLMS` link's state byte with a new letter `X`, sent when the boost key comes up while a boost is
still live. The server checks the rank itself, so an `X` from a character without the talent does
nothing, and matches on count exactly like a real one. An older server that does not know `X` falls
back to its `default:` branch and treats it as a plain level state -- degraded, not broken.

Only client-side talents need a wire field. W14 (Smoothed Plumage) is feel-only, so its ranks ride
the tally and the client scales its own dive shake and mote density; every other talent is applied
entirely server-side and needs nothing new, which is what keeps the protocol append-only instead of
adding a field per talent.

## Requirements

- Client: `wxl-mana-speedflying` with `ServerLink=1` in `wxl-mana-speedflying.ini` (default on).
- Server: `CONFIG_ADDON_CHANNEL` enabled (AzerothCore default) so the addon whisper is accepted.

## Install

Copy the `mod-mana-speedflying` folder into your AzerothCore `modules/` directory and rebuild the
worldserver. The build copies `mod-mana-speedflying.conf.dist` into `configs/`; rename it to
`mod-mana-speedflying.conf` and edit the values there. The module is picked up automatically (no
`CMakeLists.txt` is needed on current AzerothCore).

## The talent tree

Each character earns **points** by flying on an AdvFly mount, and spends them on 15 talents in three
branches of five, one point per rank. The server owns the tree: the client only renders it. Points
are `yards / Talents.YardsPerPoint`, dive-weighted (a dive counts 1.5x its distance), capped per
rolling minute by `Talents.MaxPointsPerMinute` (`0` removes the cap). A tick covering more than 200
yards is treated as a teleport and earns nothing.

| id | branch | tier | name | ranks | effect (percentage of the key above) |
|----|--------|------|------|-------|---------------------------------------|
| 1 | Dive | T1 | Steep Descent | 3 | `DiveMs` -8% per rank |
| 2 | Dive | T1 | Held Breath | 2 | `ClimbDecelMs` +15% per rank |
| 3 | Dive | T2 | Terminal Velocity | 3 | `DiveSpeed` +6% per rank, capped by `SpeedCapPct` |
| 4 | Dive | T2 | Shallow Pull | 2 | a pull-out 15 degrees shallower per rank keeps its whole glide |
| 5 | Dive | T3 | Aftershock | 1 | releasing the boost key into a dive commits the burst at once |
| 6 | Glide | T1 | Long Glide | 3 | `GlideMs` +12% per rank |
| 7 | Glide | T1 | Slipstream | 3 | `YardsPerPoint` +12% per rank |
| 8 | Glide | T2 | Banked Momentum | 2 | boost carry +10 points of `BoostCarryPct` per rank |
| 9 | Glide | T2 | Twin Vault | 1 | `BoostMaxStacks` +1 |
| 10 | Glide | T3 | Skyward Path | 2 | a glide ends 10% above the level bar per rank, settling over 1.5 s |
| 11 | Wings | T1 | Diving Wings | 3 | `StackDiveIntervalMs` -10% per rank |
| 12 | Wings | T1 | Steady Wings | 3 | `StackIntervalMs` -8% per rank |
| 13 | Wings | T2 | Iron Wings | 2 | `BoostAdd` +8% per rank |
| 14 | Wings | T2 | Smoothed Plumage | 2 | client-side only: dive shake, speed-mote density |
| 15 | Wings | T3 | Rising Current | 3 | `ClimbDecelMs` +40% per rank |

- **Tier gate.** Tier 2 in a branch opens at 1 point spent in it, tier 3 at 2. A talent's prerequisite
  must also be at full rank first (3 needs 1, 4 needs 1, 5 needs 3, 8 needs 6, 9 needs 6, 10 needs 8,
  13 and 14 need 11, 15 needs 13).
- **No effect is ever applied twice.** D2, W15 and D5 all move `ClimbDecelMs`; their ranks are summed
  into one multiplier, not compounded. D3 is the only talent that raises a state speed, and the boost
  ceiling is derived from the fastest state -- so `SpeedCapPct` is applied to the base states *before*
  that derivation, or the clamp would be silently undone.
- **Per-player, not global.** The effects live in a per-character copy of the config (`Effective cfg`),
  rebuilt on login, on every rank bought and on every config reload; the base keys are never mutated.
  Idle players are not walking around with the server's global speeds.
- **Persistence.** `character_speedflying` holds lifetime points and the sub-point yard fraction;
  `character_speedflying_talents` holds one row per unrefunded rank. Unspent points are derived
  (`points - SUM(rank)`) and never stored, so the two cannot drift. Dirty rows are flushed on a 20 s
  timer and on logout. Respec (`WXLTT\tR\t<1|2|3|A>`) is free by default and takes `Talents.RespecCost`
  if set; it refuses rather than half-applies when the money is missing. Deleting a character deletes
  both of its rows, so a recycled guid cannot inherit a tree.

Points are keyed on the character's low guid (`characters.guid`, i.e. `GetGUID().GetCounter()`), and the
module cannot add a prepared statement -- the `CharacterDatabaseStatements` enum lives in the core -- so
the queries interpolate integers only, never a string.

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
ManaSpeedFlying.BoostSpeed   = 16.0   ; 1600%: hard boost ceiling (raised to the fastest state
                                     ; speed if set lower)
ManaSpeedFlying.BoostFadeMs  = 4000   ; how fast the burst bleeds back to the state speed
ManaSpeedFlying.BoostCarryPct = 0.50  ; fraction of the boost folded into the glide
ManaSpeedFlying.BoostGlidePct = 0.70  ; fraction of GlideMs a boost's carry may spend
ManaSpeedFlying.BoostMaxStacks  = 3   ; 0..3, recharged on a timer
ManaSpeedFlying.StackIntervalMs     = 9000 ; normal-flight stack recharge
ManaSpeedFlying.StackDiveIntervalMs = 3000 ; diving stack recharge
ManaSpeedFlying.SpeedCapPct  = 1600.0 ; ceiling a talent may raise a base state speed to

; Talent tree (see "The talent tree" below; every effect is a percentage of the key above it)
Talents.Enable             = 1   ; master switch for earning and spending
Talents.YardsPerPoint      = 250.0 ; yards of flight that earn one point
Talents.MaxPointsPerMinute = 4   ; rolling-window cap; 0 = uncapped
Talents.RespecCost         = 0   ; copper per respec; 0 = free
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
ordinary glide (it decays on the glide curve), so a boost is still felt seconds after the
burst is gone. `BoostGlidePct` caps how much glide that slice may buy: a full `BoostAdd` of carry
buys `BoostGlidePct` of the ordinary `GlideMs` (0.7 by default, so a boost sheds before the 9 s
stack recharge instead of keeping the mount boosted all the time), a boost that lands on a
running glide extends that glide by only its own carry rather than restarting the long pull-out
curve, and chained or banked stacks cannot buy more than one boost's worth. The carried speed also
never lifts the base glide above `DiveSpeed`: anything past it is speed no plain state reaches, so
it stays in the short burst and fades on `BoostFadeMs` -- a boost spent at the dive ceiling empties
on the fade instead of stretching the pull-out that follows. A boost spent while climbing has
nowhere to fold *during* the climb -- a climb is not
a glide -- so its carried half is banked, the other half is the short burst, and the speed settles
back to `ClimbSpeed` on the climb's own transition. The moment you level out, the banked half is
released onto an ease-in glide sized by the carry: a full `BoostAdd` of carry buys `BoostGlidePct`
of a `GlideMs`, a smaller carry proportionally less. So the speed you built climbing does not empty
in a few seconds.

## Notes

- The client→server channel is an addon whisper to self. It is throttled on the client (about one
  message per 200 ms, plus a 1 s heartbeat) and the server treats it as untrusted input: a `1` link
  is only honoured for a flying player, a boost off the ground is answered but not applied, and a
  message flood is dropped. The server only ever sets a flight speed, so a forged link can at worst
  ask for one of the three configured state speeds -- or, while actually flying, spend the same boost
  stacks a legitimate client would have had to build.
- If the client module is unloaded, disabled or crashes mid-flight, `TimeoutMs` restores the mount's
  own speed and the module stands down until the next link message.
- Some servers apply mount-speed auras that recompute the flight rate when mounting or on buffs. The
  module re-applies its blended value on the next update, so the two may briefly fight; disable one
  or the other if that shows.

## Build

Standard AzerothCore module: drop into `modules/` and build the worldserver. No extra dependencies.
