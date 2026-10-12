# Speedflying talent tree -- plan

Design only; nothing implemented. Both repos of the pair are touched: `mod-mana-speedflying`
(authority + protocol) and `wxl-mana-speedflying` (feel + UI).

## 1. Shape

- Points are earned per character, **by flying**: yards travelled on an AdvFly mount while the
  link is up. Persisted in two new tables inside `acore_characters` -- no new database server and
  no new connection block; `CharacterDatabase` is already available to the module.
- Three branches mirror the existing state machine: **Dive**, **Glide**, **Wings**. Five talents
  each, three tiers, a tier unlocking on points already spent in that branch.
- Trust split: speed, boost, stack and cadence effects are applied **and validated server-side**;
  fov, shake and particle feel is applied **client-side** from ranks the server announces.

## 2. Database

`mod-mana-speedflying/data/sql/db-characters/2026xxxx_speedflying_talents.sql` -- idempotent, so the
module SQL updater may apply it twice without harm.

    CREATE TABLE IF NOT EXISTS `character_speedflying` (
      `guid`         INT UNSIGNED NOT NULL,
      `points`       INT UNSIGNED NOT NULL DEFAULT 0,  -- lifetime earned
      `yard_partial` DOUBLE NOT NULL DEFAULT 0,        -- yards not yet worth a point
      PRIMARY KEY (`guid`)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

    CREATE TABLE IF NOT EXISTS `character_speedflying_talents` (
      `guid`      INT UNSIGNED NOT NULL,
      `talent_id` SMALLINT UNSIGNED NOT NULL,
      `rank`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
      PRIMARY KEY (`guid`,`talent_id`)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

- Unspent points are derived: `points - SUM(rank cost)`. Nothing is stored twice.
- Talent rows exist only while `rank >= 1`; a respec deletes them.
- `yard_partial` carries the fraction under a whole point across a flush.
- A module **cannot** add a `CharacterDatabasePreparedStatement` entry -- that enum lives in the
  core. Use raw `CharacterDatabase.Query` / `Execute`, and only ever interpolate GUIDs, talent ids
  and ranks: integers, never a string.

## 3. Server: earning points

- Add `PLAYERHOOK_ON_LOGIN` to `ManaSpeedFlyingPlayer`'s hook list (today:
  `ON_BEFORE_SEND_CHAT_MESSAGE`, `ON_UPDATE`, `ON_LOGOUT`). Login reads the two tables into an
  in-memory `g_progress` map beside `g_states`; logout flushes it.
- Accrual rides the existing `OnPlayerUpdate` path, which already walks players on a timer.
- Per tick award `yards * Talents.YardsPerPoint`, where `yards` is the position delta scaled by the
  flight state (dive 1.5, climb 1.0, level 1.0), so diving pays best.
- Guards, all server-side: pay only while `st.eligible` **and** `player->IsFlying()` **and** on a
  mount -- never a taxi, never a vehicle; discard a tick delta past a teleport threshold
  (200 yards) instead of paying it; cap a rolling one-minute window at `MaxPointsPerMinute`.
  Without that cap an AFK orbit is the whole economy.
- Flush on logout and on a periodic timer (`Talents.FlushMs`, the same kind of throttled write the
  rest of the module already does), not on every point earned.

## 4. Server: spending points

New `CharacterScript` (`ManaSpeedFlyingTalents`) handling only `OnCharacterDelete` -- delete both
rows for the GUID. Spending itself does not go through a script: it is a branch inside
`OnPlayerBeforeSendChatMessage`, beside the existing `WXLMS` branch.

    client -> server:  WXLT<TAB><op><args...>
    W -> WXLTT\tW\t<talentId>       learn one rank
    R -> WXLTT\tR\t<tree>           refund <tree>  (or A for all)
    Q -> WXLTT\tQ                   request the full tree

Validation on every write, in this order: config enabled, player on a mount, GUID has a progress
row, talent id in range, `rank + 1 <= maxRank`, prerequisites met, tier unlocked, and finally the
derived unspent-point check inside the same lock that mutates rank. A refusal is answered, not
dropped, so the client panel cannot drift out of step with the server.

- Respec is free and instant; the cost knob (`Talents.RespecCost`) exists but defaults to 0.
- The tally (`WXLTT\tT\t...`, section 6) is authoritative: the client renders ranks, never decides
  them. A forged learn message is answered on the same validation as a real one.

## 5. The talents

Cost is 1 point per rank. `T1` means "1 point spent in this branch", which is what opens tier 2.

| id | branch | tier | name | ranks | effect |
|----|--------|------|------|-------|--------|
| 1 | Dive | T1 | **Steep Descent** | 3 | `DiveMs` -8% per rank (accelerate into a dive faster) |
| 2 | Dive | T1 | **Held Breath** | 2 | `ClimbDecelMs` +15% per rank (hold dive speed longer on a pull-up) |
| 3 | Dive | T2 | **Terminal Velocity** | 3 | `DiveSpeed` +6% per rank, capped by `BoostSpeed` |
| 4 | Dive | T2 | **Shallow Pull** | 2 | full carry out of a dive below 30 degrees |
| 5 | Dive | T3 | **Aftershock** | 1 | releasing SPACE aborts a live boost into a dive |
| 6 | Glide | T1 | **Long Glide** | 3 | `GlideMs` +12% per rank |
| 7 | Glide | T1 | **Slipstream** | 3 | `YardsPerPoint` +12% per rank (faster point earning) |
| 8 | Glide | T2 | **Banked Momentum** | 2 | boost carry folded into the glide +10pp per rank |
| 9 | Glide | T2 | **Twin Vault** | 1 | `BoostMaxStacks` +1 |
| 10 | Glide | T3 | **Skyward Path** | 2 | glide ends above `LevelSpeed`, settling over 1.5 s |
| 11 | Wings | T1 | **Diving Wings** | 3 | `StackDiveIntervalMs` -10% per rank |
| 12 | Wings | T1 | **Steady Wings** | 3 | `StackIntervalMs` -8% per rank |
| 13 | Wings | T2 | **Iron Wings** | 2 | `BoostAdd` +8% per rank |
| 14 | Wings | T2 | **Smoothed Plumage** | 2 | dive shake -25% per rank, motes +15% |
| 15 | Wings | T3 | **Rising Current** | 3 | entry hold `climbDecelMs` +40% per rank: a climb keeps the speed it entered with |

**One shared cap.** D3 is the only talent that raises a state speed, and `BoostSpeed` is derived from
the base states (`cfg.boostSpeed = max(boostSpeed, max(climb, level, dive))`), so D3 lifts the boost
ceiling with it. Talent-driven raising is bounded by one new knob,
`ManaSpeedFlying.SpeedCapPct` (default 1600), applied to the three base states **before** that
derivation -- clamping `boostSpeed` alone is silently undone by the `max()`.

**The climb entry is one knob.** D2 Held Breath and W15 Rising Current both scale `climbDecelMs`:
ranks from the two branches sum into a single multiplier, they do not compound one after the other.
Nothing scales the ordinary `BlendMs`-proportional climb entry (~733 ms from level): that duration
is also the level->dive entry, so halving it would halve dive entry time too.

## 6. Protocol -- `WXLTT`, appended beside `WXLMS`

A second prefix, so the existing link's parser is untouched. **Append only, never reorder or
renumber**: an older client must keep working against a newer server.

    client -> server:  WXLTT<TAB>W<TAB><talentId>      learn one rank
                       WXLTT<TAB>R<TAB><tree|A>        refund a branch or everything
                       WXLTT<TAB>Q                     request the tree

    server -> client:  WXLTT<TAB>T<TAB><points>,<spent>,<unspent>[,<id>:<rank>,...]
                       WXLTT<TAB>F<TAB><reason>        a refusal, echoed for the panel

Trailing pairs are a list, so new talents need no protocol change.

Only **client-side** talents need a wire field at all. A server-side talent (section 7) is validated
and applied entirely on the server, so it needs nothing new: the client keeps working against an
older server without knowing the talent exists, and an older client keeps working against a newer
server. That asymmetry is deliberate -- it is what keeps the protocol append-only instead of adding
a field per talent.

## 7. Effects, where each one is applied

    server  (authoritative; a forged WXLTT is validated identically)
      D3  DiveSpeed        base value, then clamped by SpeedCapPct
      D4  glide carry      carried speed retained below a shallow dive angle
      G6  GlideMs, G10 SkywardPath (all four entry durations ride one multiplier)
      G7  YardsPerPoint (server-side only -- never trusted from a client)
      G8  BoostCarryPct        G9 BoostMaxStacks
      W11 StackDiveIntervalMs  W12 StackIntervalMs

    client  (feel; driven by the ranks in the tally)
      W14 dive shake amplitude, speed-mote pool size
      any future fov / particle / clip talent

`g_cfg` is the loaded config and is also read by `OnPlayerUpdate`, so a per-player effective copy
must be built once per player on login and on every talent write -- never mutated in place, and
never read from `g_cfg` inside the award loop.

## 8. Client UI

Module-built Lua frames, exactly the pattern the speed bar and the boost orb row already use: the
C++ side writes values, Lua owns the animation and the layout.

- New `TalentUI.cpp` (and a `TalentUI.hpp` for the one entry point) in `wxl-mana-speedflying/src`.
  `screen(WXL_MountAnimTalentFrame)` is the frame, `root(parent,name)` the child helper, matching
  the existing `g_hudButtonCtx` / `WXL_MountAnimFrame` conventions.
- Texture-backed frames, not `sysfont`: system fonts are missing glyphs here, and the existing HUD
  already draws with `Interface\speedflying\*`. Add `talents\` art under
  `Data\patch-Z.mpq\Interface\speedflying\`.
- Open with `SHIFT+T` (keyboard, non-protected) and a small button beside the orb row.
- The panel needs **no device resources**: pure Lua frames driven from C++ writes, so there is
  nothing to release on `OnDeviceLost` or rebuild on `OnDeviceReset`. That is the whole reason to
  build it in Lua rather than as a D3D9 pass.
- C++ writes only the immutable delta: `WXL_MountAnimTalents` (the tally), talent metadata
  (id, name, tier, branch, maxRank, description) and the points available. Lua eases the bars,
  greys a talent it cannot yet buy and colour-tints the branch, on the `OnUpdate` the frame
  already needs.
- Click to learn, right-click to refund one rank, and a branch-button "Reset" -- each one emitting a
  `WXLTT` line through the existing send path. Nothing is optimistically committed client-side: the
  server's tally is what redraws the panel.

## 9. Settings surface

Every key lives in six places; this feature adds four, so each of the four is a six-place change:

    Talents.Enable
    Talents.YardsPerPoint
    Talents.MaxPointsPerMinute
    ManaSpeedFlying.SpeedCapPct

Every **talent effect strength** (`climbDecelMs`, `GlideMs`, `DiveSpeed`, `BoostAdd`, the two stack
cadences, `BoostCarryPct`, `BoostMaxStacks`, yards-per-point) follows the other half of the same
rule: the base value is the existing `ManaSpeedFlying.*` key, and the talent is a percentage of it,
hardcoded. Those keys are already documented in six places, so a talent adds no key at all -- the
six-place obligation is discharged by describing the talent in the `.ini` comment block beside the
key it scales, not by minting a new one.

The other knobs (`RespecCost`, `FlushMs`, teleport threshold, dive-accrual multiplier) are optional
and are named here so they are not added by accident: default them in code only, or give them the
full six places -- not three.

## 10. Order of work

1. SQL + `g_progress` load/flush on login/logout, and a self-check that a rank survives a relog.
2. Accrual in `OnPlayerUpdate` with the guards, and a log line per awarded point.
3. `WXLTT` server side: parse, validate, answer the tally. Verify with a hand-typed addon message
   before any UI exists.
4. Per-player effective config; wire the four server-applied effects.
5. `TalentUI.cpp` plus the metadata table; open it, see the tally, click a rank.
6. Client-side feel effects from the ranks.
7. The four config keys, six places each, and the README protocol section.

## 11. Verification

- Nothing here has a test suite: build, deploy, fly, read
  `F:\World of Warcraft - Manastorm\Logs\wxl-core.log` (module lines are tagged
  `wxl-mana-speedflying`; the server module logs in the worldserver log).
- Point accrual: fly a known distance, confirm the logged award matches the configured
  yards-per-point, then confirm it does not move while walking, on a taxi or on a ground mount.
- Persistence: learn a rank, log out mid-flight, relog, confirm the rank and the unspent count.
- Respec: refund with points already spent in a tier, confirm the tier relocks and the derived
  unspent count returns exactly what was refunded.
- Protocol: a hand-typed `WXLTT` learn past `maxRank` must be refused and answered, not ignored.

## 12. Traps

- **The addon channel is untrusted.** The existing review already flags the forged-boost hole; the
  tree must not widen it. Every effect above is applied server-side from a server-side config
  delta, and the client only ever *renders*. Anything the client sends is a request, not a fact.
- **Client-side speed effects do not exist.** Speed changes have to be re-applied by the server
  every tick or the engine's mount handling undoes them, which is why §7 puts all of them on the
  server.
- `Data\patch-Z.mpq` is loose files read as a patch, so talent art dropped there is live on the next
  start -- and the module may not assume it is present.
- Talent art must be redrawn at the sizes actually used; `.blp` at 34x34 beside the existing orb
  textures is the reference, and a 512 texture scaled down reads as mud.
- The client locks the loaded DLL, so the client must be closed for every deploy of
  `wxl-mana-speedflying.dll`.
