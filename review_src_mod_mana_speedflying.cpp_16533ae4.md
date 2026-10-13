## Review

**0 critical, 5 warnings, 5 suggestions**

🟡 **5 Warnings**

- `src/mod_mana_speedflying.cpp:446` - Thread-safety: LoadProgress() runs on the login thread before g_lock is taken, and p.Rebuild() (lines 424-435) reads the shared globals g_cfg and g_speedCapPct while OnAfterConfigLoad may be assigning them under g_lock on the world thread. This is an unsynchronized data race on non-atomic state; the login path should copy/re-read config under the lock or use an atomic snapshot.
- `src/mod_mana_speedflying.cpp:1612` - OnPlayerDelete() erases the guid from g_progress and the DB rows, but never erases it from g_states. The comment on this hook explicitly guards against a recycled guid, yet a reused guid would inherit the previous character's live PlayerState (eligible, stacks, stackAccum, pendingCarry), unlike OnPlayerLogout which clears both maps.
- `src/mod_mana_speedflying.cpp:1011` - Bridges are exempt from both the branch-open check (t.tier >= 3 && !BranchOpen) and the tier-above check (the loop at 1017). Bridge 16 sits in the always-open Dive branch, so that is harmless, but bridge 17 (Tucked Wings) sits in Glide and can therefore be learned from the start, opening Wings without ever opening the Glide branch it belongs to. The unlock graph is bypassable.
- `src/mod_mana_speedflying.cpp:1141` - std::atoi() is called on untrusted addon-channel input for the talent id. A numeric value outside int range is undefined behaviour, and the code already relies on the id being validated against the table. Use strtol with range checking instead.
- `src/mod_mana_speedflying.cpp:1547` - FlushAllProgress() performs blocking CharacterDatabase writes while g_lock is held (OnPlayerUpdate holds the lock from line 1297). A slow/flushed character DB will stall every other player's link tick and chat-hook processing on this path.

🔵 **5 Suggestions**

- `src/mod_mana_speedflying.cpp:1141` - The 'W' parser assumes the separator at msg[op+1] is a tab and passes c_str()+op+2 straight to atoi. A malformed body such as "WXLTT\tW123" yields a valid size check but parses the wrong id. Validate msg[op+1] == '\t' before parsing.
- `src/mod_mana_speedflying.cpp:879` - SendSpeedAnnounce uses %.3g, which switches to scientific notation at 3 significant digits: 1600 becomes "1.6e+03" (and at the 1600%% cap the boost field always does). Verify the client's parser accepts exponent form; %.4g or an integer formatter would be unambiguous.
- `src/mod_mana_speedflying.cpp:1301` - A placeholder Progress emplaced here default-constructs eff, so EffectiveConfig() returns the Config member initializers rather than the INI-loaded g_cfg. If any update tick runs before OnPlayerLogin replaces the placeholder, the player silently gets default speeds. Initialise eff from g_cfg when minting the placeholder.
- `src/mod_mana_speedflying.cpp:945` - Inside the tally builder, n += std::snprintf(...) can go negative on an encoding error, after which sizeof(body) - static_cast<size_t>(n) underflows and passes an oversized length to the next snprintf. The initial n<0 check does not cover this loop; guard each append.
- `src/mod_mana_speedflying.cpp:1098` - player->ModifyMoney(-static_cast<int32>(g_talentCfg.respecCost)) can overflow or sign-flip if Talents.RespecCost is configured above INT32_MAX; clamp the option to a sane ceiling at config load.

**Summary:** The module is well-structured and heavily documented, but it has a real unsynchronized read of g_cfg/g_speedCapPct on the login thread, a stale-state gap in OnPlayerDelete, and a talent-gate bypass that lets the Wings branch be opened without Glide. Input parsing (atoi, missing separator check) and a blocking DB flush under the global mutex are the other notable robustness concerns.
