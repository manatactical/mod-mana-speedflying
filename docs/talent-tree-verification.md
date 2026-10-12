# Speedflying talent tree -- server-side verification

Verified against `docs/talent-tree-plan.md` on commit `2694817` ("feat(talents): server-authoritative
speedflying talent tree"). Source-level only: no AzerothCore build tree exists beside
`docs/azerothcore-wotlk` (sources only), so nothing here was compiled or run.

Files read: `src/mod_mana_speedflying.cpp` (1504 lines), `src/mod_mana_speedflying_loader.cpp`,
`conf/mod-mana-speedflying.conf.dist`, `data/sql/db-characters/20260101_speedflying_talents.sql`,
`README.md`, `docs/talent-tree-plan.md`, and the AzerothCore tree
(`ScriptDefines/PlayerScript.h`, `Handlers/CharacterHandler.cpp`, `Handlers/ChatHandler.cpp`,
`database/Updater/UpdateFetcher.cpp`, `database/Database/DatabaseWorkerPool.h`,
`database/Database/Field.h`, `Entities/Object/ObjectGuid.h`, `Entities/Unit/Unit.h`).

## Verdict

A faithful implementation of the plan. Two plan items deviate for good reasons; of the 14 findings
below, three are certain functional bugs (one talent that does nothing, one malformed refund that
wipes a tree, one short-message crash), four lose or misreport player data, and the rest are doc
drift, dead code and design choices the plan left open.

## Deviations from the plan

| Plan | Code | Why it's right |
|---|---|---|
| §4 "New `CharacterScript` (`ManaSpeedFlyingTalents`) handling only `OnCharacterDelete`" | `PlayerScript::OnPlayerDelete` (`:1402`, registered `:994`) | There is **no `CharacterScript` class** in this AC tree. `OnPlayerDelete(ObjectGuid, uint32)` is the `PlayerScript` hook; it is invoked from `HandleCharDeleteOpcode` *before* `Player::DeleteFromDB`, and the module's `Execute` lands on the single DB worker thread, so the row delete is ordered correctly. |
| §3 "award `yards * Talents.YardsPerPoint`" | `yards * weight * eff / g_talentYardsPerPoint` (`:1156`) | The plan's formula is a typo; division is what makes the key mean "yards per point". |

## Confirmed correct

- **Protocol.** Prefix `WXLTT\t`; op at index 6, args at `op + 2`; `W\t<id>`, `R\t<1|2|3|A>`, `Q`.
  Server→client `T:points,spent,unspent[,id:rank]` and `F:<reason>`; every refusal has a reason
  string, never a silent drop (except a `WXLTT` truncated below the op byte -- finding 14).
- **Talent table** ids 1–15, ids append-only with the `E_Count` guard; branches, tiers, max ranks and
  prerequisites match §5. Effect wiring verified for D1/D2/D3/D4/G6/G7/G8/G10/W11/W12/W13/W15;
  `E_ClimbDecel` correctly sums D2 + W15. Ids 5 and 9 are the exceptions -- findings 1 and 10.
- **Validation order** in `HandleTalentLearn` matches §4 exactly: enabled → mounted → progress row →
  id → max rank → prerequisite → tier → unspent. All under `g_lock`; every mutation happens after
  the last refusing check, so a forged learn costs exactly what a real one costs.
- **Guid keying.** `DbGuid(raw) = raw & 0xFFFFFFFF` equals `GetCounter()` for a player guid
  (`HasEntry()` false), which is `characters.guid`.
- **SQL.** `CREATE TABLE IF NOT EXISTS` (idempotent, re-runnable); directory `db-characters`
  satisfies `UpdateFetcher`'s `dirName.find("characters")`; column types map to the `Field`
  getters used (`Get<uint32>`, `Get<double>`, `Get<uint16>`, `Get<uint8>`).
- **Accrual guards.** Taxi (`IsInFlight`) and vehicle (`GetVehicle`) excluded at the top of
  `OnPlayerUpdate`; earning requires flying + `IsMounted()` + `GetMountID()` + link-eligible;
  >200 yd (or a fresh `havePos`) discards the sample; dive weight 1.5.
- **Effects are server-side.** Ranks come from the server's own table; `adfly`/`X` from the client
  are requests, not authority. `WXLTT` cannot widen the existing forged-message hole.
- **Per-player effective config** rebuilt on login (`:1370` ff.) and on config reload (`:1484`).
- `SpeedCapPct` (default 1600 = 16.0) clamps the three base state speeds in `ApplyTalents` -- see
  finding 11 for what that clamp does to a server whose own base speed is above it.
- **Respec** affordability (`HasEnoughMoney` / `ModifyMoney`) checked before any mutation.
- **The `WXLMS` link is untouched**: `WXLTT` rides its own prefix and its own handler, so an older client
  or server keeps working (`:1006`), which is what plan §6 asks for.

## Bugs

### 1. Glide T2 "Twin Vault" (id 9) is a complete no-op

```cpp
{ 9, ..., E_BoostStacks, 0.0f, "Twin Vault", ... }        // :131
c.boostStacks += static_cast<int>(t.eff[E_BoostStacks] - 1.0f + 0.5f);   // :277
```

`eff` starts at 1.0 and `0.0f * rank = 0`, so the delta is always `0`. The cap comment
`if (c.boostStacks > 4) c.boostStacks = 4; // 3 configured + 1 from G9` (`:301`) shows the intent.
Every other effect carries its unit in `pctPerRank` (`E_ShallowPull` uses 15.0 degrees,
`E_BoostCarry` 0.10); this one needs `1.0f`.

**Fix:** one character at `:131`: `0.0f` → `1.0f`.

### 2. A malformed refund argument refunds the whole tree

```cpp
const int branch = what == "A" ? -1 : std::atoi(what.c_str()) - 1;   // :902
```

`atoi("")` and `atoi("0")` both yield 0, so `branch == -1` -- the "refund everything" sentinel
(`:920` tests `branch >= 0` to choose the scoped path). `WXLTT\tR\t` (or `R\t0`, or any
non-numeric argument) is therefore a free, silent full respec. `R\t5` is correctly refused (`:903`);
only the low side collides. Lowercase `a` is also refused, since only `"A"` is compared.

**Fix:** parse strictly -- `what == "A"` -> all, else accept exactly `"1"`/`"2"`/`"3"`,
anything else -> `bad-branch`.

### 3. A stale, never-loaded `Progress` entry can wipe points and ranks on relog

- `OnPlayerUpdate` creates an entry with `Progress{}` when the guid is absent (`:1122-1124`); that
  entry has `loaded == false` and `eff` at struct defaults, not `g_cfg`.
- `OnPlayerLogin` then sets `it->second.loaded = true;` on an entry that already exists
  (`:1375-1379`) without loading anything.

The player now holds `points = 0`, no ranks, `loaded == true`. The first earned point sets `dirty`,
and `WriteProgress` (`:392`) does `INSERT ... ON DUPLICATE KEY UPDATE points = VALUES(points)`
followed by `DELETE` + re-insert of the (empty) rank set -- the DB row is replaced with zeros.

Trigger: if one update tick runs after `OnPlayerLogout` for a player still in the world, the erstwhile
entry is recreated as an empty placeholder and then lingers in `g_progress` for that guid (nothing
garbage-collects it). The next login for that guid loads the real row into a local `p` but finds the
placeholder already present, so it only flips `loaded` -- the real data is discarded. The first earned
point sets `dirty` and `WriteProgress` replaces the DB row with zeros. This is the logout/relog race the
comment at `:1373` is guarding, losing the data it was meant to protect.

**Fix:** on login, replace an existing entry that is not `loaded` (or erase the placeholder in
`OnPlayerLogout` and drop a placeholder that is not `loaded`); never set `loaded` on data that was not
read.

### 4. `FlushAllProgress(bool force)` is dead, so there is no shutdown flush

Only two call sites: `FlushAllProgress()` on the timer (`:1357`) and `WriteProgress` directly on
logout (`:1395`). `force` is never passed `true`, and the module's `WorldScript` has no `OnShutdown()`
override (the core's base class declares it). A graceful restart therefore loses up to `Talents.FlushMs`
bought since the last flush** (`dirty` is set on learn/refund). The plan accepted "a crash costs
the partial point"; a graceful shutdown should not.

**Fix:** one `WorldScript::OnShutdown()` override calling `FlushAllProgress(true)` under `g_lock` --
this is what the parameter was written for. (`OnShutdownInitiate` fires earlier, on the `/shutdown`
command, and is the other candidate.)

### 5. The rolling cap decays by halving, and a capped point burns its yards

`recentPoints /= 2` every 60 s (`:1177-1182`) instead of dropping the samples that left the window.
With the default 4, a continuously flying character earns 4, then only `4 - 2 = 2` in each following
minute -- a sustained rate of `MaxPointsPerMinute / 2`.

Concurrently, `:1158-1163` subtracts the whole point from `yardPartial` and then `break`s when the
cap refuses it -- the yard credit is gone, not banked.

**Fix:** trim only the meters that aged out of the window, and do not consume `yardPartial` when the
cap blocks the award.

## More findings

### 6. `yard_partial` is never marked dirty, so any earned fraction is lost on relog

`progress.dirty = true` is set only inside the point-award loop (`:1166`), but `yardPartial`
changes on every earning tick (`:1156`). On logout `if (pit->second.dirty)` (`:1395`) skips the
write, and `FlushAllProgress` requires `p.loaded && (p.dirty || force)`. A player who logs out
mid-fraction loses that fraction, contradicting the schema comment ("carries the fraction ... across
a relog").

**Fix:** mark dirty when `yardPartial` changes.

### 7. `ManaSpeedFlying.SpeedCapPct` and `ClimbSpeed` are invisible to the client

The `S:` announce (`:755`) sends only level/dive/boost, and `RateFor` (`:496`) has no climb/clamp
awareness. The panel's ceiling cannot reflect a clamped build, and D3 Terminal Velocity cannot raise
`divePct`. Deliberate protocol minimalism, but the panel silently lies on a capped server.

### 8. Six-place rule incomplete for the four new keys

`conf.dist` yes, reader yes, struct default yes, `README.md` yes -- but the client
`wxl-mana-speedflying.ini` has no talent line, and the panel mentions `MaxPointsPerMinute` once. You
can only earn points on a server where the client is pointed at it; the pair's player-facing INI is
where the tree should be explained. (No write-back place applies -- the module writes no INI.)

### 9. `g_talentCfg.enabled` is read before the lock

`:945` reads it outside `g_lock`; every other config read is inside. A config reload can race it --
benign in practice, but it is the one unlocked read.

### 10. Aftershock (id 5) spends a second stack, and its commit does not stick

`HandleAftershock` -> `ActivateBoost(st, eff, /*abort=*/true)` (`:793`). `ActivateBoost` does
`st.stacks--` (`:588`) and computes a *fresh* `BoostAdd`, so releasing SPACE into a dive needs an unspent
stack and adds the full boost again. "Releasing the boost key into a dive commits the burst at once"
(`README.md:136`) reads as folding the still-fading `boostOffset` into `current` without paying twice.

And the commit is transient: abort does `st.current += add - carry` (`:639`) but leaves
`st.start`/`st.target`/`st.duration` alone, and the transition block reassigns `st.current` every tick
(`:1240`, `:1248`, `:1258`), so the committed speed is overwritten on the next update. Net effect of a
successful call: the remaining fade is lost and a stack is spent.

Unreachable today anyway -- see the cross-repo note.

### 11. Prerequisite / tier prose in `conf.dist` contradicts the code

- `conf.dist:312` says "tier 3 once 3 points are" spent; code uses `spent[branch] < tier - 1` (tier 3
  opens at **2**) and `README.md:148` says 2. Conf is wrong.
- `conf.dist` says "Terminal Velocity and Shallow Pull need Held Breath" (id 2), but code and README
  both use talent id **1** (Steep Descent). Other prerequisites (5->3, 8->6, 9->6, 10->8, 13/14->11,
  15->13) match.
- `conf.dist` says "Held Breath, Aftershock and Rising Current all scale ClimbDecelMs"; Aftershock
  (id 5) uses `E_Count`, not `E_ClimbDecel`. The in-code `E_ClimbDecel` enum comment (`:81`) repeats
  the same wrong claim.
- Shallow Pull: code/README = 30 + 15 x rank degrees, but `conf.dist` says "+50% dive speed carried out
  of a shallow pull" and `kTalents[].desc` (`src:122`) says "Carry 50% more dive speed per rank".
- Two more `kTalents[].desc` strings describe other talents: id 4's and id 5's (which sits on `E_Count`,
  the unimplemented sentinel).
- `README.md:340` says talent 10 caps the elevated level-out at `SpeedCap`; code clamps to
  `SpeedCapPct`. `README.md:86` writes `WXLTT\tT:\t<points>,...` but the code emits `T:` with a colon
  and no tab (`src:812`), and the `F` line the same way.
- `SpeedCapPct` is documented as a "ceiling a talent may raise the *base* state speeds to"
  (`conf.dist:274`, `README.md`), but `ApplyTalents` clamps whatever it is handed. Set
  `ManaSpeedFlying.DiveSpeed` above `SpeedCapPct` and every player silently gets the ceiling instead of
  their server's configured speed. Either clamp only the talent delta, or document it as a hard ceiling
  on the base states.

### 12. Nits

- `Talents::RankById` (`:233`) is dead -- `HandleAftershock` inlines the same scan.
- The `OnPlayerLogout` comment block repeats its last line (`:1383-1384`).
- `kTalents[].name`/`desc` are never on the wire, so the client duplicates all 15 names; drift is only
  a matter of time.

### 13. `'Q'` (tally request) is refused when not mounted

`IsMounted()` is checked before the op switch, so the talent panel cannot refresh on foot.

### 14. Short `WXLTT` messages can throw out of the chat hook

`std::atoi(msg.c_str() + op + 2)` and `msg.substr(op + 2)` assume at least two bytes after the op.
For a 7-byte `WXLTT\tR` message, `substr(op + 2)` (pos 8 > size 7) throws `std::out_of_range`; a
7-byte `WXLTT\tW` reads one byte past the NUL. `WorldSession::Update` only catches
`InvalidHyperlink`/`IllegalHyperlink`/`PacketArrayMaxCapacity`/`ByteBufferException`, so the
exception escapes the hook. Plan §12 treats the addon channel as untrusted input, so a client -- or
anything else able to send on the prefix -- can trip this.

**Fix:** require `msg.size() > op + 1` before parsing the argument, and refuse otherwise.

## Not verified

- No compile and no in-game run: there is no AzerothCore build tree or `worldserver` binary beside
  `docs/azerothcore-wotlk`, and the client half of the pair is unfinished.
- Finding 3 (stale `Progress` entry) is reasoned from the code paths, not reproduced.
- `git diff --stat` and `git status --porcelain` in the client repo print nothing although `TalentUI.*`
  is present, and the client's own commit `a96646d feat(talents): client panel ...` tracks
  `src/TalentUI.cpp` -- so the shell never reached that repo, and the client's own history was not read
  directly. Client findings below come from reading the files themselves.

## Cross-repo note: the client half is unfinished

The client's `TalentUI.cpp` sends `WXLTT\tR\t<branch>` / `WXLTT\tW\t<id>` / `Q` (`:185`, `:206-207`,
`:432`), matching the server parser, and parses `T:`/`F:` bodies (`:345-370`), zeroing all ranks before
applying the pair list so omitted rank-0 entries are safe. What is missing:

- **Nothing sends the `WXLMS` state letter `'X'`.** `ServerState()` (`SpeedFlying.cpp:1485-1494`) returns
  only `C`/`D`/`H`/`L`, the boost letter is `'B'` (`:1419`, a `<advfly>B` body) and the periodic link is
  `:1509`. So the server's `HandleAftershock` (`:1076`) is unreachable and D5 Aftershock is dead on both
  sides until a sender exists, including the "boost key came up while a boost is still live" edge.
- `RegisterAddonMessagePrefix('WXLTT')` **is** present (`SpeedFlying.cpp:1204`), and the handler stores
  the body into `_G.WXL_TalentBody` (`:1207-1211`, prefix match). That part is wired.

## Risks to weigh before fixing

1. Findings 1, 2, 4 and 14 are static and certain; fix them first (one line each except 2 and 14).
2. Finding 3 depends on a logout/relog race that was reasoned about but not reproduced -- worth a log line
   before and after the fix.
3. Finding 5 is a design choice the plan left open; decide the intended economy (sustained rate = cap,
   or cap/2) before changing it.
4. `OnPlayerDelete` only fires from `HandleCharDeleteOpcode`. Deletes through other paths (console
   `.character delete`, offline delete at char-select) would leak rows. `OnPlayerDeleteFromDB` or
   `PEERHOOK_ON_DELETE_FROM_DB` with a transaction is the safer hook if that matters.
5. Nothing here was built. Confirm every finding with a build and an in-game session before acting on the
   design ones.
