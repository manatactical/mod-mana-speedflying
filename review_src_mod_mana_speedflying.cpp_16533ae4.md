## Review

**3 critical, 3 warnings, 3 suggestions**

🔴 **3 Critical**

- `src/mod_mana_speedflying.cpp:996` - The learn gate compares lifetime earned points (`p.points >= g_talentCfg.maxPoints`) instead of unspent points. `HandleTalentRefund` never lowers `p.points`, so a character that has earned the cap and then respecced keeps `points == maxPoints` while `Unspent() > 0`; every subsequent learn is refused with "all-spent" and the tree is permanently unspendable. The following `Unspent() <= 0` check already covers the real case, so this check should be removed or changed to test Unspent().
- `src/mod_mana_speedflying.cpp:889` - D5 Aftershock calls `ActivateBoost(st, eff, /*abort=*/true)`, which decrements a stack (line 684) and computes a fresh `BoostAdd`, so releasing the boost key into a dive charges a second stack and re-adds the boost; with no stacks left it silently does nothing instead of committing the live burst. Aftershock should fold the existing `boostOffset` into `current`, not spend another stack.
- `src/mod_mana_speedflying.cpp:1557` - OnPlayerLogin sets `it->second.loaded = true` on an entry that already exists without loading its row. If OnPlayerUpdate ran first it created the placeholder `g_progress.emplace(guid, Progress{})` at line 1274, so the player now holds points=0, no ranks and default `eff` with loaded=true; the first award sets `dirty` and WriteProgress (line 395) REPLACEs the DB row with zeros (and deletes all talent rows). Replace an unloaded placeholder rather than flipping its loaded flag.

🟡 **3 Warnings**

- `src/mod_mana_speedflying.cpp:730` - The abort commit (`st.current += add - carry`) is not durable: `st.start`/`st.target`/`st.duration` are left untouched, so the update transition block (1428/1447/1455) overwrites `st.current` on the next tick. It also zeroes `boostStart`/`boostOffset` (line 730-731), discarding the pre-existing fading burst the commit is supposed to keep. Net effect of a successful Aftershock is a lost fade plus a spent stack.
- `src/mod_mana_speedflying.cpp:816` - `st.settling` is armed on a G10 level-out but SetTarget's real state-change path never clears it (only the grounded reset at 1394 and settle completion at 1434 do). If the player climbs or dives after a level-out has begun, the new transition runs to completion and then the settle block (1428) overrides `st.current` back toward `levelSpeed`, ignoring the new state until the next state change.
- `src/mod_mana_speedflying.cpp:1099` - The `IsMounted()` guard sits before the op switch, so the read-only 'Q' tally request is refused with "not-mounted" while on foot and the panel cannot refresh. Only the mutating ops (W/R) need the mounted check; 'Q' should be answered regardless.

🔵 **3 Suggestions**

- `src/mod_mana_speedflying.cpp:1084` - `g_talentCfg.enabled` is read before `g_lock` is acquired (lock taken at line 1096), unlike every other config read in this file; an OnAfterConfigLoad reload can race it. Move the read inside the lock.
- `src/mod_mana_speedflying.cpp:1665` - `tc.teleportYards` is clamped here but never populated from ConfigManager, and `tc.flushMs` (line 246) is likewise never read, so the clamp is dead and `Talents.TeleportYards`/`Talents.FlushMs` are not tunable despite being treated as knobs. Either read them or drop the clamp.
- `src/mod_mana_speedflying.cpp:1344` - `progress.recentPoints /= 2` every rolling minute halves the intended sustained award rate (default MaxPointsPerMinute=4 yields ~2/min) rather than expiring only the samples that aged out of the window.

**Summary:** The module is well-structured and the untrusted-message parsing has mostly been hardened, but several concrete defects remain: the respec/lifetime-cap interaction permanently bricks the tree after a respec, Aftershock double-spends a stack and its commit does not survive the next tick, and a placeholder Progress entry can cause OnPlayerLogin to mark unloaded data loaded and overwrite the character's stored points/ranks with zeros. A pending G10 settle is also never cancelled by a new flight state, and the read-only 'Q' request is incorrectly refused while unmounted.
