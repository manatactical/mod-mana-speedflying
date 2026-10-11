/*
 * mod-mana-speedflying: vary a flying mount's speed with its flight state.
 *
 * Server-side companion to the client's wxl-mount-anim module. That module already works out the
 * state (climbing / leveling / diving) and whether the mount carries the AdvFly clips, so it
 * says so directly: it whispers an addon message with the "WXLMS" prefix, and this module blends the
 * mount's flight speed toward the speed the state calls for. No server-side guesswork and no mount
 * list -- a mount the client does not drive with AdvFly reports '0' and its speed is left alone.
 *
 * Protocol, an addon whisper:  "WXLMS\t<advfly:0|1><state:L|C|D|H>"
 *   advfly  '1' = the mount is one wxl-mount-anim drives (AdvFly clips), '0' = leave the speed alone
 *   state   C climb, D dive, H hovering, L level / strafe / backward
 *
 * The server answers with its own configured speeds so the client scales its bar, particles and shake
 * to them:  "WXLMS\tS:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,
 * <stackMs>,<stackDiveMs>"  (the same WXLMS prefix; the client reads the last addon message once a
 * second). <cooldownMs> is sent as 0 (the boost has no cooldown) and the two trailing intervals are the
 * normal and diving stack-recharge cadences. <stacks> is the server's live boost-stack count, so the
 * client button mirrors the authoritative value instead of drifting; it is re-sent whenever the count
 * changes.
 *
 * The state is applied over a transition whose length and curve depend on where the speed is going:
 * a dive accelerates into DiveSpeed (faster and faster), pulling out to level is a long glide that
 * bleeds speed slowly at first and then hard, and pulling up into a climb holds the built-up speed
 * before braking hard. A climb cut short to level keeps that glide. Other hops keep the
 * span-proportional blend. If the link goes quiet for TimeoutMs, the
 * mount's own speed is restored.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    enum class Motion
    {
        Level = 0,
        Climb,
        Dive,
        Hover
    };

    // Shape of a transition's speed curve.
    enum class Curve
    {
        Linear, // constant rate
        EaseIn, // holds the start speed, then moves hard near the end
        Dive    // starts with a small kick, then keeps accelerating
    };

    struct Config
    {
        bool   enabled      = true;
        float  climbSpeed   = 2.0f;  // 200% of base run speed
        float  levelSpeed   = 2.8f;  // 280%
        float  diveSpeed    = 8.0f;  // 800%
        uint32 blendMs      = 5500;  // default blend, proportional to the span it must cover
        uint32 diveMs       = 4000;  // accelerate into a dive
        uint32 glideMs      = 18000; // long ease-in glide from a dive back to level
        uint32 climbDecelMs = 3500;  // dive -> climb: hold the dive speed, then brake hard
        uint32 hoverMs      = 500;   // short accel/decel zone leaving a hover
        uint32 sendMs       = 150;   // how often the blended speed may reach the client
        uint32 timeoutMs    = 2500;  // drop the link if no message arrives for this long
        float  boostAdd     = 4.0f;  // 400%: the speed one boost adds back to the glide
        float  boostSpeed   = 16.0f; // 1600%: hard ceiling a boost may lift the speed to
        uint32 boostFadeMs  = 4000;  // how fast a boost's added speed bleeds back to the state speed
        float  boostCarryPct = 0.50f; // fraction of a boost folded into the glide to outlast the fade
        int    boostStacks  = 3;     // most boost stacks the button can hold
        uint32 stackMs      = 9000;  // ms between boost stacks in normal flight
        uint32 stackDiveMs  = 3000;  // ms between boost stacks while diving
    };

    Config g_cfg;

    float RateFor(Motion m)
    {
        switch (m)
        {
            case Motion::Climb: return g_cfg.climbSpeed;
            case Motion::Dive:  return g_cfg.diveSpeed;
            default:            return g_cfg.levelSpeed; // Level and Hover
        }
    }

    float MaxRate() { return std::max(g_cfg.climbSpeed, std::max(g_cfg.levelSpeed, g_cfg.diveSpeed)); }
    float MinRate() { return std::min(g_cfg.climbSpeed, std::min(g_cfg.levelSpeed, g_cfg.diveSpeed)); }

    struct PlayerState
    {
        bool   eligible  = false;         // client said the mount is AdvFly-capable
        bool   fromDive  = false;         // the running climb was entered straight out of a dive
        Motion motion    = Motion::Level; // state the current transition is heading to
        float  start     = 0.0f;          // rate when the current transition began
        float  target    = 0.0f;          // rate the current state calls for
        float  current   = 0.0f;          // rate being applied
        float  lastSent  = -1.0f;         // last rate handed to SetSpeed; < 0 = nothing applied
        uint32 elapsed   = 0;             // ms into the current transition
        uint32 duration  = 0;             // ms the current transition runs for; 0 = instant
        Curve  curve     = Curve::Linear; // shape of the current transition
        uint32 sendAccum = 0;
        uint32 sinceLink = 0;             // ms since the last link message, for the timeout
        bool   announced = false;         // the client has been told the current speeds
        uint32 lastAnnounce = 0;          // ms of the last speed announce, for the slow resend
        uint32 lastMsgMs = 0;             // ms of the last non-boost state message, for the flood guard
        int    stacks = 0;                // boost stacks held (to g_cfg.boostStacks)
        uint32 stackAccum = 0;            // ms accrued toward the next boost stack
        uint32 stackInterval = 0;         // cadence stackAccum was accrued at; lets a switch rescale
        // A boost is an additive offset on the running transition, not a new transition of its own:
        // the state blend keeps running underneath and the offset fades to nothing, so the speed
        // lands back on the glide the mount already had instead of snapping to the plain state speed.
        float  boostStart = 0.0f;         // the offset a boost added, before its fade
        float  boostOffset = 0.0f;        // the still-decaying part of that offset
        uint32 boostElapsed = 0;          // ms into the fade
        uint32 boostDuration = 0;         // ms the fade runs for; 0 = no boost running
        float  pendingCarry = 0.0f;       // boost carry banked while climbing, released when the climb ends
    };

    std::mutex g_lock;
    std::unordered_map<uint64, PlayerState> g_states;

    // Scales a fixed transition time by how far the speed actually has to travel, relative to the
    // configured full drop. A shallow pull-out recovers sooner than a deep one; the canonical dive
    // (current at the far rate) keeps the configured duration exactly.
    uint32 ScaledMs(uint32 baseMs, float dist, float reference)
    {
        if (reference <= 0.0f)
            return baseMs;
        // abs(): a caller may pass a signed distance, and a negative one cast to uint32 wraps to a
        // multi-day stall instead of a transition.
        return static_cast<uint32>(static_cast<float>(baseMs) * std::fabs(dist) / reference);
    }

    // Transition length for entering `next` from the state we are in.
    uint32 DurationFor(const PlayerState& st, Motion next)
    {
        const float nextRate = RateFor(next);
        // A hover (a parked mount) is a deliberately tiny accel/decel zone in both directions: the
        // speed drops to the parked level as the mount stops and comes straight back out when it
        // moves, so parking cannot bank a residual dive glide to spend on the next take-off.
        if (st.motion == Motion::Hover || next == Motion::Hover)
            return g_cfg.hoverMs;
        if (next == Motion::Dive)
            return g_cfg.diveMs;
        if (st.motion == Motion::Dive)
        {
            const float dist = std::fabs(nextRate - st.current);
            if (next == Motion::Climb)
                return ScaledMs(g_cfg.climbDecelMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.climbSpeed));
            if (next == Motion::Level)
                return ScaledMs(g_cfg.glideMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.levelSpeed));
        }
        // A climb entered from a dive and cut short to level still carries the leftover dive speed,
        // so it glides out like a pull-out rather than blending like a fresh climb.
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
        {
            const float dist = std::fabs(nextRate - st.current);
            return ScaledMs(g_cfg.glideMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.levelSpeed));
        }
        // Everything else keeps the old span-proportional blend.
        const float span = MaxRate() - MinRate();
        if (span <= 0.0f)
            return 0;
        const float dist = std::fabs(nextRate - st.current);
        return static_cast<uint32>(static_cast<float>(g_cfg.blendMs) * dist / span);
    }

    // Curve used to accelerate a dive (a kick so it responds at once, then faster and faster),
    // hold a glide's speed before bleeding it off (ease-in), or neither.
    Curve CurveFor(const PlayerState& st, Motion next)
    {
        if (next == Motion::Dive)
            return Curve::Dive;
        // Ease-in holds the built-up speed first, then brakes hard near the end. Used for every pull
        // out of a dive: level (the long glide) and climb (hold the speed, then lose it).
        if (st.motion == Motion::Dive)
            return Curve::EaseIn;
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
            return Curve::EaseIn;
        return Curve::Linear;
    }

    // The button boost: spends a stack and adds BoostAdd to the speed right now as an offset on top
    // of whatever blend is running. The offset fades over BoostFadeMs (fast at first, then slow).
    // BoostCarryPct of the added speed is not part of that offset: it is folded into the running
    // transition (or banked until a climb levels out) instead, so it bleeds off over the ordinary
    // glide and the boost still pays speed seconds after the burst is gone. There is no cooldown, so
    // held stacks can be chained.
    void ActivateBoost(PlayerState& st)
    {
        if (!st.eligible || st.stacks <= 0)
            return;
        // Usable while climbing, diving or gliding level. Only a hover (a parked mount) is left out,
        // so a boost cannot be spent to bank speed before a take-off.
        if (st.motion != Motion::Dive && st.motion != Motion::Level && st.motion != Motion::Climb)
            return;

        const float applied = st.current + st.boostOffset;
        const float boosted = std::min(g_cfg.boostSpeed, applied + g_cfg.boostAdd);
        if (boosted <= applied)
            return; // already at the ceiling

        st.stacks--;

        const float add = boosted - applied;
        float carry     = add * g_cfg.boostCarryPct;

        // Replenish part of the boost as ordinary glide time. A climb is not a glide, so its carry
        // is banked: folding it into the running transition would restart that transition on the
        // long GlideMs EaseIn curve and hold speed past the climb. SetTarget releases the banked
        // carry when the climb ends -- on leveling out it rides its own long glide, so the retained
        // half becomes glide time instead of emptying in a few seconds.
        if (carry > 0.0f && st.motion == Motion::Climb)
        {
            st.pendingCarry += carry;
        }
        // Otherwise raise the base transition and restart its fade from there, so the carry decays
        // on the long dive-glide curve, not the short boost one. Only fold it when the base is
        // decelerating (current at or above target); while a dive is still accelerating, folding it
        // would stall the climb to DiveSpeed, so the whole boost stays in the burst offset instead.
        // The distance is abs()'d: an early-dive fold could otherwise be negative and wrap the
        // duration to a multi-day stall.
        else if (carry > 0.0f && st.current >= st.target)
        {
            st.start    = st.current + carry;
            st.current  = st.start;
            st.target   = RateFor(st.motion);
            st.elapsed  = 0;
            st.duration = ScaledMs(g_cfg.glideMs, std::fabs(st.start - st.target),
                                   std::fabs(g_cfg.diveSpeed - g_cfg.levelSpeed));
            st.curve    = Curve::EaseIn;
        }
        else
            carry = 0.0f;

        // Add to any boost still fading so chained stacks build one bigger burst instead of replacing.
        st.boostStart    = st.boostOffset + (add - carry);
        st.boostOffset   = st.boostStart;
        st.boostElapsed  = 0;
        st.boostDuration = g_cfg.boostFadeMs;
    }

    // Records the state a link message named. The first message on a mount snaps the applied rate so
    // a take-off does not spend seconds ramping from the mount's own speed.
    void SetTarget(PlayerState& st, Motion m)
    {
        st.sinceLink = 0;

        if (!st.eligible)
        {
            st.eligible  = true;
            st.motion    = m;
            st.current   = RateFor(m);
            st.start     = st.current;
            st.target    = st.current;
            st.elapsed   = 0;
            st.duration  = 0;
            st.curve     = Curve::Linear;
            st.fromDive  = false;
            st.pendingCarry = 0.0f;
            st.lastSent  = -1.0f;
            st.sendAccum = g_cfg.sendMs; // force the first packet out on the next update
            return;
        }

        if (m == st.motion)
        {
            st.target = RateFor(m); // heartbeat: keep the running transition going
            return;
        }

        // A real state change: start a fresh transition from wherever the speed is now.
        const Motion prev = st.motion;
        if (m == Motion::Climb)
            st.fromDive = (prev == Motion::Dive);

        // Leaving a climb releases the boost carry that was banked there (a climb is not a glide, so
        // it had nowhere to fold). Leveling out merges the still-fading burst into the base first, so
        // the applied speed does not step up at the switch, and spends the carry on glide time (the
        // longer transition below) instead of adding it on top -- the bar glides out instead of
        // bumping. Any other change out of the climb just carries it into the new transition.
        float foldedCarry = 0.0f;
        if (prev == Motion::Climb && st.pendingCarry > 0.0f)
        {
            if (m == Motion::Level)
            {
                // Fold the fading burst into the base: the transition starts from the live speed, so
                // there is no upward step. Merging the offset here lets it bleed on the glide curve
                // instead of running as a second, separately-fading layer below.
                const float live = std::min(g_cfg.boostSpeed, st.current + st.boostOffset);
                st.current       = live;
                st.boostStart    = 0.0f;
                st.boostOffset   = 0.0f;
                st.boostElapsed  = 0;
                st.boostDuration = 0;
                foldedCarry      = std::min(st.pendingCarry, std::max(0.0f, g_cfg.boostSpeed - live));
            }
            else
            {
                // A pull-out that still carries dive speed keeps its higher current speed; everything
                // else keeps the banked half folded into the new transition.
                const float base = st.current;
                foldedCarry = std::min(st.pendingCarry, std::max(0.0f, g_cfg.boostSpeed - base));
                st.current = base + foldedCarry;
            }
            st.pendingCarry = 0.0f;
        }

        st.start    = st.current;
        st.target   = RateFor(m);
        st.elapsed  = 0;
        st.duration = DurationFor(st, m);
        st.curve    = CurveFor(st, m);
        // A banked carry pays for its own glide: a full BoostAdd of carry buys the full GlideMs, a
        // smaller carry proportionally less, so the retained speed is not dumped in three or four
        // seconds. Only when the mount is actually shedding speed -- an eased rise from below the
        // level speed would just delay settling, not carry momentum. Keep whichever glide is longer,
        // so a pull-out that still carries dive speed is never shortened.
        if (m == Motion::Level && foldedCarry > 0.0f && g_cfg.boostAdd > 0.0f && st.start > st.target)
        {
            const uint32 carryDur = ScaledMs(g_cfg.glideMs, foldedCarry, g_cfg.boostAdd);
            if (carryDur > st.duration)
            {
                st.duration = carryDur;
                st.curve    = Curve::EaseIn;
            }
        }
        st.motion   = m;
    }

    // Publish the configured speeds and the live stack count to the client so its bar, particles,
    // shake and boost button scale to the values actually in play and its stack count cannot drift from
    // the server's. Same "WXLMS" prefix the client already sends, body
    // "S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,
    // <stackDiveMs>,<stackAccumMs>,<stackIntervalMs>"; the client polls
    // its last addon message, ignores a body it cannot parse, and uses the last two fields to anchor
    // the orb's recharge bar to the server's exact position (an older server omits them). A UI reload
    // on the client drops the value, which the slow resend covers.
    void SendSpeedAnnounce(Player* player, int stacks, uint32 stackAccum, uint32 stackInterval)
    {
        char body[160];
        std::snprintf(body, sizeof(body), "S:%.3g,%.3g,%.3g,%u,%d,%u,%d,%u,%u,%u,%u",
                      g_cfg.levelSpeed * 100.0f, g_cfg.diveSpeed * 100.0f, g_cfg.boostSpeed * 100.0f, 0u,
                      g_cfg.boostStacks, g_cfg.boostFadeMs, stacks, g_cfg.stackMs, g_cfg.stackDiveMs,
                      stackAccum, stackInterval);

        // The client listens for the addon prefix on CHAT_MSG_ADDON, which the client's chat layer only
        // parses out of a LANG_ADDON chat packet -- so the prefix rides at the front, exactly as a
        // client-sent SendAddonMessage lays it out.
        WorldPacket data;
        std::string message = std::string("WXLMS\t") + body;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }
}

class ManaSpeedFlyingPlayer : public PlayerScript
{
public:
    ManaSpeedFlyingPlayer()
        : PlayerScript("ManaSpeedFlyingPlayer",
                       { PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT }) {}

    // The client link. wxl-mount-anim senders are the only ones that matter; everything else on the
    // addon channel is left as the core received it. The channel is untrusted: a '1' link is only
    // applied to a flying player and a boost off the ground is dropped, but the server cannot tell a
    // real client from a forged message -- it only ever sets a flight speed.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/, uint32& lang, std::string& msg) override
    {
        if (!player || lang != LANG_ADDON)
            return;
        if (msg.size() < 8 || msg.compare(0, 6, "WXLMS\t") != 0)
            return;

        const char advfly = msg[6];
        const char state  = msg[7];

        bool announce = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (!g_cfg.enabled)
                return;

            const uint64 guid = player->GetGUID().GetRawValue();
            if (advfly != '1')
            {
                // Not an AdvFly mount (or no mount): stand down without minting a state entry. The
                // update path restores the core's own speed for a link that was up.
                auto it = g_states.find(guid);
                if (it != g_states.end())
                {
                    it->second.eligible  = false;
                    it->second.announced = false;
                }
                return;
            }

            PlayerState& st = g_states[guid];

            const uint32 now = getMSTime();
            // A client sends at most one message per ~200 ms; anything faster is a flood. Boost
            // messages are never throttled (each spends a stack and must be answered), so the
            // client's stack count cannot desync from a dropped one.
            if (state != 'B' && st.lastMsgMs && now - st.lastMsgMs < 50)
                return;
            st.lastMsgMs = now;

            // First contact gets the server's speeds at once; after that a slow resend covers a client
            // UI reload (which wipes the value) without chattering on every state change.
            if (!st.announced || now - st.lastAnnounce >= 5000)
            {
                st.announced    = true;
                st.lastAnnounce = now;
                announce        = true;
            }

            // The button replenishes the glide: it lifts the speed and starts a fresh glide, so the
            // added speed is spent as extra glide time instead of a fixed timed burst. The server only
            // drives a flying mount, so a boost off the ground is answered (to keep the client's count
            // honest) but not applied.
            if (state == 'B')
            {
                st.sinceLink = 0;
                if (player->IsFlying())
                    ActivateBoost(st);
                // Answer every attempt with the authoritative count, spent or rejected, so a click the
                // server ignored cannot leave the client button one stack off until the slow resend.
                st.lastAnnounce = now;
                announce        = true;
            }
            else
            {
                switch (state)
                {
                    case 'C': SetTarget(st, Motion::Climb); break;
                    case 'D': SetTarget(st, Motion::Dive);  break;
                    case 'H': SetTarget(st, Motion::Hover); break;
                    default:  SetTarget(st, Motion::Level); break;
                }
            }
            if (announce)
                SendSpeedAnnounce(player, st.stacks, st.stackAccum,
                                  st.motion == Motion::Dive ? g_cfg.stackDiveMs : g_cfg.stackMs);
        }
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!player || player->IsInFlight() || player->GetVehicle())
            return;

        // A lagging or suspended server (a local worldserver resumed from sleep, say) can hand us a
        // multi-second p_time. Trusting it lets the recharge block below dump the whole boost stock --
        // and snap every transition -- in one tick. Bound it to a sane frame.
        p_time = std::min<uint32>(p_time, 1000);

        const uint64 guid   = player->GetGUID().GetRawValue();
        const bool   flying = player->IsFlying();

        // The whole read-modify-write runs under one lock: the chat hook can interleave a SetTarget or
        // a boost, and a second lock later would let that update be overwritten by this tick's stale
        // write-back. All config reads are inside this lock too, so a reload cannot tear them.
        std::lock_guard<std::mutex> guard(g_lock);

        auto it = g_states.find(guid);
        if (it == g_states.end())
            return;
        PlayerState& st = it->second;

        // Advance the link clock even while grounded, so a link that dies as the mount lands still
        // times out instead of freezing eligible and resuming stale on the next take-off.
        if (st.eligible && g_cfg.timeoutMs)
        {
            st.sinceLink += p_time;
            if (st.sinceLink > g_cfg.timeoutMs)
            {
                st.eligible  = false; // link went quiet: the client is gone, stop driving the mount
                st.announced = false; // force the speeds onto the next link
            }
        }

        if (!g_cfg.enabled || !flying || !st.eligible)
        {
            // Restore the mount's own speed once and clear the running blend and boost, so a grounded,
            // unlinked or switched-off mount cannot resume a stale rate on the next take-off. Banked
            // boost stacks are kept: a grounded mount earns none, but does not lose the ones it built.
            if (st.eligible)
            {
                st.eligible  = false;
                st.announced = false;
            }
            if (st.lastSent >= 0.0f)
            {
                player->UpdateSpeed(MOVE_FLIGHT, true);
                st.lastSent = -1.0f;
            }
            st.current       = 0.0f;
            st.target        = 0.0f;
            st.start         = 0.0f;
            st.elapsed       = 0;
            st.duration      = 0;
            st.fromDive      = false;
            st.sendAccum     = 0;
            st.stackAccum    = 0; // grounded earns nothing: do not resume half a stack on take-off
            st.stackInterval = 0;
            st.boostStart    = 0.0f;
            st.boostOffset   = 0.0f;
            st.boostElapsed  = 0;
            st.boostDuration = 0;
            st.pendingCarry  = 0.0f;
            return;
        }

        // Eased transition from `start` to `target` over `duration`. This keeps running under a
        // boost, so the boost is only an offset added on top of it below.
        if (st.duration == 0)
        {
            st.current = st.target;
        }
        else
        {
            st.elapsed += p_time;
            if (st.elapsed >= st.duration)
            {
                st.elapsed = st.duration;
                st.current = st.target;
            }
            else
            {
                const float p = static_cast<float>(st.elapsed) / static_cast<float>(st.duration);
                float e = p;
                if (st.curve == Curve::EaseIn)
                    e = p * p;
                else if (st.curve == Curve::Dive)
                    e = p * (0.35f + 0.65f * p); // kick off the line, then keep accelerating
                st.current = st.start + (st.target - st.start) * e;
            }
        }

        // Boost stacks build on a timer: one every StackIntervalMs, or every StackDiveIntervalMs
        // while diving, capped at the configured stock. The timer only runs while the mount is
        // link-eligible, so a grounded or unlinked mount earns nothing.
        const int    prevStacks       = st.stacks;
        const uint32 stackIntervalNow = st.motion == Motion::Dive ? g_cfg.stackDiveMs : g_cfg.stackMs;
        if (st.stacks < g_cfg.boostStacks)
        {
            if (stackIntervalNow)
            {
                // A cadence switch keeps the fraction of progress already made, not the raw
                // milliseconds: the bar holds its fill and the remaining work runs at the new rate.
                if (st.stackInterval && st.stackInterval != stackIntervalNow && st.stackAccum)
                    st.stackAccum = static_cast<uint32>(static_cast<uint64>(st.stackAccum) * stackIntervalNow /
                                                        st.stackInterval);
                st.stackAccum += p_time;
                if (st.stackAccum >= stackIntervalNow)
                {
                    st.stackAccum = 0; // one stack per update: a single long p_time must never fill the bar
                    ++st.stacks;
                }
            }
        }
        else
            st.stackAccum = 0;
        st.stackInterval = stackIntervalNow;

        // The boost is an additive offset on top of the running transition, fading to nothing over
        // BoostFadeMs: fast at first, then slow. Because the underlying transition keeps running, the
        // speed lands back on exactly the glide the mount had before the boost instead of snapping to
        // the plain state speed.
        if (st.boostDuration)
        {
            st.boostElapsed += p_time;
            if (st.boostElapsed >= st.boostDuration)
            {
                st.boostOffset   = 0.0f;
                st.boostElapsed  = 0;
                st.boostDuration = 0;
            }
            else
            {
                const float p    = static_cast<float>(st.boostElapsed) / static_cast<float>(st.boostDuration);
                const float keep = (1.0f - p) * (1.0f - p); // fast at first, then slow
                st.boostOffset = st.boostStart * keep;
            }
        }

        const float applied = std::min(g_cfg.boostSpeed, st.current + st.boostOffset);
        // Settled once the base transition is done and any boost has fully faded, so the exact final
        // value is always pushed. Testing `current == target` instead would fire every tick during a
        // boost fade (the base is settled but the offset is not), defeating the SendMs throttle.
        const bool settled = (applied == st.target && st.boostDuration == 0);

        st.sendAccum += p_time;
        if (st.sendAccum >= g_cfg.sendMs || settled)
        {
            st.sendAccum = 0;
            if (applied != st.lastSent)
            {
                player->SetSpeed(MOVE_FLIGHT, applied, true);
                st.lastSent = applied;
            }
        }

        // A stack gained while diving is pushed at once, so the client button never lags the count the
        // next boost will actually spend.
        if (st.stacks != prevStacks)
            SendSpeedAnnounce(player, st.stacks, st.stackAccum, st.stackInterval);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(g_lock);
        g_states.erase(player->GetGUID().GetRawValue());
    }
};

class ManaSpeedFlyingWorld : public WorldScript
{
public:
    ManaSpeedFlyingWorld()
        : WorldScript("ManaSpeedFlyingWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Config cfg;
        cfg.enabled      = sConfigMgr->GetOption<bool>("ManaSpeedFlying.Enable", true);
        cfg.climbSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.ClimbSpeed", 2.0f);
        cfg.levelSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.LevelSpeed", 2.8f);
        cfg.diveSpeed    = sConfigMgr->GetOption<float>("ManaSpeedFlying.DiveSpeed", 8.0f);
        cfg.blendMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BlendMs", 5500);
        cfg.diveMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.DiveMs", 4000);
        cfg.glideMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.GlideMs", 18000);
        cfg.climbDecelMs = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.ClimbDecelMs", 3500);
        cfg.hoverMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.HoverMs", 500);
        cfg.sendMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.SendMs", 150);
        cfg.timeoutMs    = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.TimeoutMs", 2500);
        cfg.boostAdd     = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostAdd", 4.0f);
        cfg.boostSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostSpeed", 16.0f);
        cfg.boostFadeMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BoostFadeMs", 4000);
        cfg.boostCarryPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostCarryPct", 0.50f);
        cfg.boostStacks  = sConfigMgr->GetOption<int>("ManaSpeedFlying.BoostMaxStacks", 3);
        cfg.stackMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackIntervalMs", 9000);
        cfg.stackDiveMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackDiveIntervalMs", 3000);

        cfg.climbSpeed   = std::max(cfg.climbSpeed,   0.1f);
        cfg.levelSpeed   = std::max(cfg.levelSpeed,   0.1f);
        cfg.diveSpeed    = std::max(cfg.diveSpeed,    0.1f);
        cfg.boostAdd     = std::max(cfg.boostAdd,     0.0f);
        // The boost ceiling must sit at or above the fastest state speed, or the min() in the update
        // path would clamp ordinary (unboosted) level/climb flight below its configured rate.
        cfg.boostSpeed   = std::max(cfg.boostSpeed,
                                    std::max(cfg.climbSpeed, std::max(cfg.levelSpeed, cfg.diveSpeed)));
        cfg.boostStacks  = std::max(0, std::min(cfg.boostStacks, 3));
        cfg.boostCarryPct = std::max(0.0f, std::min(cfg.boostCarryPct, 1.0f));
        if (!cfg.boostFadeMs) cfg.boostFadeMs = 1;
        if (!cfg.stackMs) cfg.stackMs = 1;
        if (!cfg.stackDiveMs) cfg.stackDiveMs = 1;
        if (!cfg.sendMs) cfg.sendMs = 1;

        // Publish the new config and drop every link under the lock readers take. The entries are kept
        // (only marked ineligible) so the update path can restore each mount's own speed: clearing the
        // map would leave the last blended rate applied with nothing left to repair it.
        std::lock_guard<std::mutex> guard(g_lock);
        g_cfg = cfg;
        for (auto& kv : g_states)
        {
            kv.second.eligible  = false;
            kv.second.announced = false;
        }
    }
};

void AddSC_ManaSpeedFlying()
{
    new ManaSpeedFlyingPlayer();
    new ManaSpeedFlyingWorld();
}
