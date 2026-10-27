# mod-mana-speedflying

An AzerothCore module that varies a flying mount's speed with how it is flying, as a server-side
companion to the client's **wxl-mana-speedflying** module:

| state      | speed |
|------------|-------|
| climbing   | 220%  |
| leveling   | 320%  |
| diving     | 600%  |

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
on the first link message, whenever a stack is spent or granted, **whenever `maxStacks` changes** (Twin
Vault raises it to 4, and the held count does not move when it does, so a count-only trigger never told
the client and the orb row stayed at three), and again every few seconds (which also recovers a client
UI reload), so
changing `ManaSpeedFlying.DiveSpeed` on the server is enough -- no matching edit in
`wxl-mana-speedflying.ini` is needed. If the module is not installed, the client falls back to its own
`GlideSpeedPct` / `MaxSpeedPct` defaults.

## The talent protocol -- `WXLTT`

Client to server (an addon whisper, prefix `WXLTT`, a tab after the prefix):

    WXLTT<TAB>W<TAB><talentId>                     learn one rank
    WXLTT<TAB>B<TAB><id>:<target>[,<id>:<target>]  learn up to an absolute rank each (a draft)
    WXLTT<TAB>R<TAB><1|2|3|A>                      refund a tree (1 dive, 2 glide, 3 wings) or everything
    WXLTT<TAB>U<TAB><talentId>                     take one rank back; whatever ahead of it needed it goes too
    WXLTT<TAB>Q                                    request the tree

`R` also accepts `b1`..`b3` for an older build. Every `B` is absolute, so a resent batch is the same request
and a dropped reply cannot double-spend. The client sends at most 12 pairs per `B`.

Server to client, one whisper per body (the addon cap is 255 bytes):

    T:<points>,<spent>,<unspent>,<n>,<ranks>,P<permille>,N<yards>   the whole tree, REPLACES the client's table
    P:<permille>,<yards>                                           the partial point alone, for the progress bar
    F:<reason>                                                     a refusal

`<ranks>` is `<n>` characters, one per talent in table order: `0`-`9`, then `a`-`z` (a linked talent reaches
35). The whole tree fits one message (about 90 bytes), and it must be one message: the client's listener keeps
only the last `T`/`F` body it hears, so a split tally would lose every part but the last. A client whose
table has a different `<n>` ignores the ranks rather than mis-assigning them. A tally is sent after every
`W`, `B`, `R` and `Q`. The bar moves on its own, so it rides a `P:` body about twice a second while flight
accrues yards.

Refusal reasons (`F:`): `locked` (no parent is at full rank), `cap-taken` (another big talent holds a rank), `ring-full` (a plain talent of the same tree and ring is at full rank),
`max-rank`, `no-points`, `unknown` (an id this build does not know), `no-progress`, `not-mounted`,
`disabled`, `bad-arg`, `bad-branch`, `no-money`. A refusal is always answered, so the panel cannot drift.

Rank changes are validated on the server in this order: max rank, one big talent, a parent at full rank,
then unspent points. A batch applies in dependency passes, so a draft may rank a bridge and the talent it
opens in any order; a refusal stops only what cannot yet be bought.

## Requirements

- Client: `wxl-mana-speedflying` with `ServerLink=1` in `wxl-mana-speedflying.ini` (default on).
- Server: `CONFIG_ADDON_CHANNEL` enabled (AzerothCore default) so the addon whisper is accepted.

## Install

Copy the `mod-mana-speedflying` folder into your AzerothCore `modules/` directory and rebuild the
worldserver. The build copies `mod-mana-speedflying.conf.dist` into `configs/`; rename it to
`mod-mana-speedflying.conf` and edit the values there. The module is picked up automatically (no
`CMakeLists.txt` is needed on current AzerothCore).

## The circular tree (Wind-Sworn)

The talent tree is a circle. Windborn (id 100) is the centre and the only way in. Three trees hang off it,
each five rings deep: **Dive** (top), **Glide** (lower right) and **Wings** (lower left). Ring 6 holds three
big talents, one of which a character may ever take. Beyond ring 6 each big talent has a linked talent that
gives up to 35 ranks of +1% flight speed. The client draws the same layout from the same table.

**Unlock rule.** A node can take a rank once **any** of its parents is at **full rank**. A big talent has the three ring-5
talents of its tree as parents. Windborn has no parent. There are no tiers, no picks of two and no branch gates.

**One big talent.** Only one ring-6 node may hold a rank. The server refuses a second one with `cap-taken`.
The linked talent opens only on its own big talent (`locked` otherwise).

**Bridges.** Still Air (140), Tucked Wings (141) and Rising Current (142) sit on ring 3. Skimmer (143),
Windfall (144) and Thunderhead (145) sit on ring 5. Each one links the two neighbouring trees. Still Air and
Tucked Wings are the two angle buffs:
Still Air raises the climb threshold from 15 to 25 degrees, and Tucked Wings lowers the dive threshold from
75 to 65 degrees, so a dive starts sooner. Both are read by the client from the tally.

**Cost and cap.** Each rank costs one point. `Talents.MaxPoints` is 140 by default: the core plus at most 139
ranks. Points cost `Talents.BaseYards` (500) yards for the first, and each one after it needs
`Talents.YardStep` (0.04) more yards than the one before, compounding.

**Respec.** `R1`, `R2`, `R3` clear the ordinary and linked talents of the dive, glide or wings tree. Bridges,
the core and the big talents stay. `RA` clears everything. A refund never re-opens a node by itself: points
already spent below a refunded node keep their rank.

**Migration.** The old 17-id tree (ids 1..17) is retired. The new ids are 100..151 and the two sets do not
overlap. A stored row with an old id matches nothing, so the module skips it on load and its points come
back as unspent on the first tally. Nothing is deleted from `character_speedflying_talents`, so the change is
reversible. Every character starts the new tree with its full earned points unspent.

**Config.** `Talents.MaxPointsPerMinute` stays at 4, but raise it for a 140-point tree or the burst rate
dominates the earning. See `conf/mod-mana-speedflying.conf.dist`.

**The 52 nodes** (from `talents.json`; the same table is `kTalents` on the server and in the client):

| id | ring | tree | kind | name | ranks | parents | effect |
|---|---|---|---|---|---|---|---|
| 100 | core | all | core | Windborn | 1 | - | All flight speeds +20%. |
| 101 | 1 | dive | node | Steep Descent | 3 | 100 | Accelerate into a dive 8% faster per rank. |
| 102 | 1 | dive | node | Held Breath | 3 | 100 | Hold dive speed 15% longer per rank when pulling up into a climb. |
| 103 | 2 | dive | node | Terminal Velocity | 3 | 101 | Reach 6% more dive speed per rank. |
| 104 | 2 | dive | node | Sharp Banking | 3 | 101, 102 | Speed changes blend 8% faster per rank. |
| 105 | 2 | dive | node | Dive Yield | 3 | 102 | Earn 10% more talent progress per rank while diving. |
| 106 | 3 | dive | node | Shallow Pull | 3 | 103, 104 | A pull-out 15 degrees shallower per rank keeps its whole glide. |
| 107 | 3 | dive | node | Aftershock | 1 | 104, 105 | Releasing the boost key into a dive commits the burst at once. |
| 108 | 4 | dive | node | Gravity Well | 3 | 106, 140 | Reach 5% more dive speed per rank. |
| 109 | 4 | dive | node | Rebound | 3 | 106, 107 | Fold 8 more points of a boost into the glide per rank. |
| 110 | 4 | dive | node | Dive Bomb | 2 | 107, 142 | A boost during a dive adds 20% more speed per rank. |
| 111 | 5 | dive | node | Meteor | 1 | 108, 109 | Accelerate into a dive 35% faster. |
| 112 | 5 | dive | node | Hammerfall | 2 | 109, 108 | Reach 10% more dive speed per rank. |
| 113 | 5 | dive | node | Redline | 2 | 110, 109 | Raise the speed cap by 200 points per rank. |
| 114 | 1 | glide | node | Long Glide | 3 | 100 | Glide 12% longer per rank out of a dive. |
| 115 | 1 | glide | node | Updraft | 3 | 100 | Climb 6% faster per rank. |
| 116 | 2 | glide | node | Skyward Path | 3 | 114 | Fly level 5% faster per rank. |
| 117 | 2 | glide | node | Slipstream | 3 | 114, 115 | Shorten the step to the next point by 12% per rank. |
| 118 | 2 | glide | node | Hover Poise | 2 | 115 | Leave a hover 30% faster per rank. |
| 119 | 3 | glide | node | Long Haul | 3 | 116, 117 | Earn 8% more talent progress per rank while level or climbing. |
| 120 | 3 | glide | node | Glide Anchor | 1 | 117, 118 | A boost keeps 15 more points of the ordinary glide. |
| 121 | 4 | glide | node | Jetstream | 3 | 119, 141 | Fly level 6% faster per rank. |
| 122 | 4 | glide | node | Draft | 3 | 119, 120 | Speed changes blend 6% faster per rank. |
| 123 | 4 | glide | node | Updraft Master | 3 | 120, 140 | Climb 8% faster per rank. |
| 124 | 5 | glide | node | Eternal Current | 1 | 121, 122 | Glide 50% longer out of a dive. |
| 125 | 5 | glide | node | Highway | 2 | 122, 121 | Fly level 10% faster per rank. |
| 126 | 5 | glide | node | Skybound | 2 | 123, 122 | A boost keeps 20 more points of the ordinary glide per rank. |
| 127 | 1 | wings | node | Iron Wings | 3 | 100 | Each boost adds 8% more speed per rank. |
| 128 | 1 | wings | node | Quick Recovery | 3 | 100 | Boost stacks recharge 8% faster per rank. |
| 129 | 2 | wings | node | Diving Wings | 3 | 127 | Boost stacks recharge 10% faster per rank while diving. |
| 130 | 2 | wings | node | Afterburn | 3 | 127, 128 | A boost's speed lasts 10% longer per rank. |
| 131 | 2 | wings | node | Wing Reserve | 1 | 128 | Hold one more boost stack. |
| 132 | 3 | wings | node | Banked Momentum | 3 | 129, 130 | Fold 10 more points of a boost into the glide per rank. |
| 133 | 3 | wings | node | Overcharge | 1 | 130, 131 | Raise the boost speed ceiling 10%. |
| 134 | 4 | wings | node | Surge Cells | 3 | 132, 142 | Each boost adds 8% more speed per rank. |
| 135 | 4 | wings | node | Fast Cells | 3 | 132, 133 | Boost stacks recharge 8% faster per rank. |
| 136 | 4 | wings | node | Reserve Charge | 1 | 133, 141 | Start every flight with one more boost stack. |
| 137 | 5 | wings | node | Twin Vault | 1 | 134, 135 | Hold one more boost stack. |
| 138 | 5 | wings | node | Overdrive | 2 | 135, 134 | Raise the boost speed ceiling 12% per rank. |
| 139 | 5 | wings | node | Last Gasp | 2 | 136, 135 | A boost's speed lasts 25% longer per rank. |
| 140 | 3 | dive/glide | bridge | Still Air | 1 | 103, 118 | A glide may be 25 degrees steep before it reads as a climb. |
| 141 | 3 | glide/wings | bridge | Tucked Wings | 1 | 116, 131 | A dive starts at 65 degrees instead of 75. |
| 142 | 3 | wings/dive | bridge | Rising Current | 1 | 105, 129 | A climb keeps 40% more of its entered speed. |
| 143 | 5 | dive/glide | bridge | Skimmer | 2 | 108, 123 | Level and dive speed 4% higher per rank. |
| 144 | 5 | glide/wings | bridge | Windfall | 2 | 121, 136 | Boosts add 10% more speed and level flight 3% per rank. |
| 145 | 5 | wings/dive | bridge | Thunderhead | 2 | 110, 134 | Diving stacks recharge 15% faster and dive speed rises 3% per rank. |
| 146 | 6 (big) | dive | cap | Falcon's Stoop | 1 | 112, 111 | Dive speed +40% and the dive ramp is halved. |
| 147 | 6 (big) | glide | cap | Endless Horizon | 1 | 125, 124 | Level and climb speed +30%. |
| 148 | 6 (big) | wings | cap | Tempest Heart | 1 | 138, 137 | Two more boost stacks, stacks recharge 25% faster, boosts add 40% more. |
| 149 | 7 (linked) | dive | linked | Stoop Mastery | 35 | 146 | All flight speeds +1% per rank. |
| 150 | 7 (linked) | glide | linked | Horizon Mastery | 35 | 147 | All flight speeds +1% per rank. |
| 151 | 7 (linked) | wings | linked | Tempest Mastery | 35 | 148 | All flight speeds +1% per rank. |

**Persistence.** `character_speedflying` holds lifetime points and the sub-point yard fraction;
`character_speedflying_talents` holds one row per ranked talent. Unspent points are derived
(`points - SUM(rank)`) and never stored, so the two cannot drift. Dirty rows are flushed on a
`Talents.FlushMs` (20 s) timer, on logout and on world shutdown. Respec is free by default and takes
`Talents.RespecCost` if set. It refuses rather than half-applies when the money is missing. Deleting a
character deletes both of its rows, so a recycled guid cannot inherit a tree. Points are keyed on the
character's low guid (`characters.guid`, i.e. `GetGUID().GetCounter()`), and the queries interpolate
integers only, never a string.

## Configuration

All keys live under `[worldserver]`; see `conf/mod-mana-speedflying.conf.dist` for the full
comments. Defaults:

```ini
ManaSpeedFlying.Enable       = 1
ManaSpeedFlying.ClimbSpeed   = 2.0    ; 200%
ManaSpeedFlying.LevelSpeed   = 3.2    ; 320%
ManaSpeedFlying.DiveSpeed    = 6.0    ; 600%
ManaSpeedFlying.BlendMs      = 5500   ; default blend, proportional to the span it covers
ManaSpeedFlying.DiveMs       = 4000   ; accelerate into a dive
ManaSpeedFlying.GlideMs      = 18000  ; long glide from a dive back to level
ManaSpeedFlying.ClimbDecelMs = 3500   ; dive -> climb: hold the speed, then brake hard
ManaSpeedFlying.HoverMs      = 500    ; fixed accel/decel zone leaving a hover
ManaSpeedFlying.SendMs       = 100    ; blend step; each change is broadcast, so the rate is quantized to 0.01
ManaSpeedFlying.TimeoutMs    = 2500   ; restore the mount's own speed if the link goes quiet
ManaSpeedFlying.BoostAdd     = 4.0    ; 400%: speed one boost adds to the glide
ManaSpeedFlying.BoostSpeed   = 12.0   ; 1200%: hard boost ceiling (raised to the fastest state
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
Talents.MaxPoints          = 13  ; lifetime cap on earned points
Talents.BaseYards          = 500.0 ; yards for the first point
Talents.YardStep           = 1.0   ; each point after the first costs this much more (1.0 = doubles)
Talents.MaxPointsPerMinute = 4   ; rolling-window cap; 0 = uncapped
Talents.RespecCost         = 0   ; copper per respec; 0 = free
Talents.FlushMs            = 20000 ; how often dirty points/ranks are written back
Talents.TeleportYards      = 200.0 ; a tick longer than this is a teleport, not flight
Talents.DiveAccrual        = 1.5 ; distance multiplier while diving
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
`StackDiveIntervalMs` (3 s) while diving, up to `BoostMaxStacks`. Twin Vault raises that ceiling to 4, and
the new ceiling is announced the moment the rank lands (see the `S:` body above), so the client's orb row
grows to four then rather than after the next stack happens to be spent. The timer only runs while the mount
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
