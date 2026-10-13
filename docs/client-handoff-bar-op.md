# Client work owed by `wxl-mana-speedflying` — the `WXLTT\tP` bar op

**Status:** server side is done and committed. The client has not been touched. Until the client change
lands, one bug is live (see §1).

Repo to change: `C:\Users\Strix\Documents\Github Projects\WXL\wxl-mana-speedflying`
Files: `src/TalentUI.cpp` (parser), `src/SpeedFlying.cpp` (the listener, no change expected).

---

## 1. The bug the client currently has — **fix this first**

`TalentUI.cpp::PollTally()` reads `_G.WXL_TalentBody` and clears it **only** on the `T:` and `F:`
paths. The unknown-op branch returns *before* the clear:

```cpp
else
{
    return; // an op a newer server added; nothing on this side to do with it
}

// Clear it so the same body is not re-applied every frame, and not at all after a reload.
if (script::Context())
    script::Execute("_G.WXL_TalentBody=nil", "wxl-mana-speedflying");
```

The server now sends `P:<permille>,<yards>` bodies on its own ~500 ms cadence. An unpatched client hits
that `return`, leaves the body in the global, and re-reads the same string on **every frame** — while
also never draining the global, so the next real `T:` or `F:` body is masked as long as `P` bodies keep
arriving. The panel would freeze its bar and stop answering learns.

Minimum fix, on its own, before any of §2: clear the global on **every** read that got a string,
regardless of which branch handled it. Move the clear to just after the `isString`/`body[0]` checks.

---

## 2. Handle `P` — the actual feature

In `PollTally()`, alongside the existing `T:` and `F:` branches, add a `P:` branch:

```
server -> client:  WXLTT\tP\t<permille>,<yards>
```

- `<permille>` — 0..1000, the partial point as thousandths of *the point being paid for*. Same value
  the `T:` tail's `,P<permille>` carries.
- `<yards>` — yards the next point still needs, rounded to whole yards. Same value the `T:` tail's
  `,N<yards>` carries. It is **0** when the tree is at `Talents.MaxPoints` (there is no next point).

Behaviour:

- Parse with `std::sscanf(body + 2, "%d,%d%n", &permille, &need, &consumed)`. A return `< 2` means a
  malformed body — ignore it, do not raise, do not touch the bar.
- Clamp `permille` to `0..1000` and `need` to `>= 0`, exactly as the `T:` tail does today.
- Assign `g_partial` and `g_needYards`, then `Redraw()`.
- **Do not** touch `g_points`, `g_spent`, `g_unspent`, `g_rankOf[]` or `g_haveTally`. `P` is the bar
  fields alone; a tally is the only thing allowed to move a rank. Touching them here would clear the
  tree on every bar tick, because the loop that scans rank pairs is what resets them.
- `g_status[0]` is left alone — `P` is not a refusal.

Suggested shape, matching the file's existing style:

```cpp
else if (body[0] == 'P' && body[1] == ':')
{
    int permille = 0, need = 0;
    if (std::sscanf(body + 2, "%d,%d", &permille, &need) < 2)
        return; // malformed: keep the bar we last drew
    g_partial   = permille < 0 ? 0 : (permille > 1000 ? 1000 : permille);
    g_needYards = need < 0 ? 0 : need;
    // Only the bar moved; a rank or a point total is a tally's job.
}
```

then fall through to the shared clear + `Redraw()`.

### Backward compatibility — the tail is still there

The `T:` body keeps its `,P<permille>,N<yards>` tail. The server sends a full tally on every learn,
refund, `Q`, and on the tick that pays a point, so a client that only ever reads the tail still gets a
correct bar at those moments — it just does not see the bar creep between them. That is a degraded but
correct display, not a broken one. **Do not remove the tail parsing**; `P` is an optimisation, the tail
is the floor.

---

## 3. Verification

There is no test suite on either side. The check is:

1. Build the client module, deploy, start the client on an AdvFly mount with the server module loaded.
2. `F:\World of Warcraft - Manastorm\Logs\wxl-core.log` must show `extensions: loaded 'wxl-mana-speedflying'`.
3. Fly level on a mount long enough to bank a partial point (default: the first point is 500 yards).
4. Open the talent panel (SHIFT+T). The progress bar must fill smoothly, not in jumps of half a second
   or more, and the "yards remaining" text must count down.
5. Learn a rank the moment it becomes affordable. It must take on the **first** click — a client that
   masks the `T:` body behind an undrained `P` shows the old total and refuses.
6. Respec (`R`), confirm the tree clears and the bar returns to the cost of point 1.

Bisect tool: comment out the server's `SendTalentBar` call in `OnPlayerUpdate` and rebuild the server
module. The bar should then only move on point/rank changes — that is the pre-change behaviour, so it
isolates a client-side parser fault from a server-side one.

---

## 4. Not in scope, but paired

Two other server-side items were reviewed and **not** implemented; they are listed here so the client
side can decide on them together later.

- **Boost announce amplification.** The server answers every boost (`B`) and aftershock (`X`) message
  with an inline ~100-byte speed announce, at an unbounded rate, because a 2-byte message in is answered
  by a 100-byte message out. The server-side fix (defer the announce to the update tick via a dirty
  flag) needs no client change. The client-side half would be to stop sending `B`/`X` faster than the
  tick can answer.
- **Link latency.** `SpeedFlying.cpp::SendLink` coalesces at 200 ms, so a motion change reaches the
  server up to 200 ms late. Forcing a send on an actual motion change (it already passes `force` for
  capability changes and dismounts) plus dropping the coalescing window to ~100 ms would cut the felt
  delay on a dive onset. Entirely client-side; no protocol change.