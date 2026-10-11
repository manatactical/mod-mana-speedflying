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
        float  boostCarryPct = 0.20f; // fraction of a boost folded into the glide to outlast the fade
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
        int    stacks = 0;                // boost stacks held (to g_cfg.boostStacks)
        uint32 stackAccum = 0;            // ms accrued toward the next boost stack
        // A boost is an additive offset on the running transition, not a new transition of its own:
        // the state blend keeps running underneath and the offset fades to nothing, so the speed
        // lands back on the glide the mount already had instead of snapping to the plain state speed.
        float  boostStart = 0.0f;         // the offset a boost added, before its fade
        float  boostOffset = 0.0f;        // the still-decaying part of that offset
        uint32 boostElapsed = 0;          // ms into the fade
        uint32 boostDuration = 0;         // ms the fade runs for; 0 = no boost running
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
    // transition instead, so it bleeds off over the ordinary glide and the boost still pays speed
    // seconds after the burst is gone. There is no cooldown, so held stacks can be chained.
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

        // Replenish part of the boost as ordinary glide time: raise the base transition and restart
        // its fade from there, so the carry decays on the long dive-glide curve, not the short boost
        // one. Only fold it when the base is decelerating (current at or above target); while a dive
        // is still accelerating, folding it would stall the climb to DiveSpeed, so the whole boost
        // stays in the burst offset instead. A climb is excluded on purpose: it is not a glide, so
        // folding the carry here would restart the transition on the long GlideMs EaseIn curve and a
        // boost spent climbing would hold its speed far longer than the climb itself, draining the
        // bar slowly instead of settling back to ClimbSpeed. The distance is abs()'d: an early-dive
        // fold could otherwise be negative and wrap the duration to a multi-day stall.
        if (carry > 0.0f && st.current >= st.target && st.motion != Motion::Climb)
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
        st.start    = st.current;
        st.target   = RateFor(m);
        st.elapsed  = 0;
        st.duration = DurationFor(st, m);
        st.curve    = CurveFor(st, m);
        st.motion   = m;
    }

    // Publish the configured speeds and the live stack count to the client so its bar, particles,
    // shake and boost button scale to the values actually in play and its stack count cannot drift from
    // the server's. Same "WXLMS" prefix the client already sends, body
    // "S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,
    // <stackDiveMs>"; the client polls
    // its last addon message once a second and ignores a body it cannot parse. A UI reload on the client
    // drops the value, which the slow resend covers.
    void SendSpeedAnnounce(Player* player, int stacks)
    {
        char body[112];
        std::snprintf(body, sizeof(body), "S:%.3g,%.3g,%.3g,%u,%d,%u,%d,%u,%u", g_cfg.levelSpeed * 100.0f,
                      g_cfg.diveSpeed * 100.0f, g_cfg.boostSpeed * 100.0f, 0u,
                      g_cfg.boostStacks, g_cfg.boostFadeMs, stacks, g_cfg.stackMs, g_cfg.stackDiveMs);

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
    // addon channel is left as the core received it.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/, uint32& lang, std::string& msg) override
    {
        if (!g_cfg.enabled || !player || lang != LANG_ADDON)
            return;
        if (msg.size() < 8 || msg.compare(0, 6, "WXLMS\t") != 0)
            return;

        const char advfly = msg[6];
        const char state  = msg[7];

        bool announce = false;
        int  stacksOut = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            PlayerState& st = g_states[player->GetGUID().GetRawValue()];

            if (advfly != '1')
            {
                // Not an AdvFly mount (or no mount): the update path restores the core's own speed.
                st.eligible = false;
                return;
            }

            // First contact gets the server's speeds at once; after that a slow resend covers a client
            // UI reload (which wipes the value) without chattering on every state change.
            const uint32 now = getMSTime();
            if (!st.announced || now - st.lastAnnounce >= 5000)
            {
                st.announced    = true;
                st.lastAnnounce = now;
                announce        = true;
            }

            // The button replenishes the glide: it lifts the speed and starts a fresh glide, so the
            // added speed is spent as extra glide time instead of a fixed timed burst.
            if (state == 'B')
            {
                st.sinceLink = 0;
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
            stacksOut = st.stacks;
        }

        if (announce)
            SendSpeedAnnounce(player, stacksOut);
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!g_cfg.enabled || !player || !player->IsFlying() || player->IsInFlight() || player->GetVehicle())
            return;

        // A lagging or suspended server (a local worldserver resumed from sleep, say) can hand us a
        // multi-second p_time. Trusting it lets the recharge block below dump the whole boost stock --
        // and snap every transition -- in one tick. Bound it to a sane frame.
        p_time = std::min<uint32>(p_time, 1000);

        const uint64 guid = player->GetGUID().GetRawValue();

        bool   eligible;
        bool   timedOut = false;
        Curve  curve;
        float  current, target, start, lastSent;
        uint32 elapsed, duration, sendAccum, sinceLink, stackAccum;
        float  boostStart, boostOffset;
        uint32 boostElapsed, boostDuration;
        Motion motion;
        int    stacks;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_states.find(guid);
            if (it == g_states.end())
                return;
            eligible  = it->second.eligible;
            current   = it->second.current;
            target    = it->second.target;
            start     = it->second.start;
            elapsed   = it->second.elapsed;
            duration  = it->second.duration;
            curve     = it->second.curve;
            lastSent  = it->second.lastSent;
            sendAccum = it->second.sendAccum;
            sinceLink = it->second.sinceLink;
            stacks     = it->second.stacks;
            stackAccum = it->second.stackAccum;
            motion     = it->second.motion;
            boostStart    = it->second.boostStart;
            boostOffset   = it->second.boostOffset;
            boostElapsed  = it->second.boostElapsed;
            boostDuration = it->second.boostDuration;
        }
        const int prevStacks = stacks;

        if (eligible && g_cfg.timeoutMs)
        {
            sinceLink += p_time;
            if (sinceLink > g_cfg.timeoutMs)
            {
                eligible = false; // link went quiet: the client is gone, stop driving the mount
                timedOut = true;
            }
        }

        if (!eligible)
        {
            // Restore the mount's own speed once, then stay out of the way.
            if (lastSent >= 0.0f)
                player->UpdateSpeed(MOVE_FLIGHT, true);
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_states.find(guid);
            if (it != g_states.end())
            {
                if (timedOut)
                    it->second.eligible = false;
                it->second.current   = 0.0f;
                it->second.lastSent  = -1.0f;
                it->second.sendAccum = 0;
                it->second.sinceLink = sinceLink;
                it->second.stacks    = 0;
                it->second.stackAccum = 0;
                it->second.boostStart = 0.0f;
                it->second.boostOffset = 0.0f;
                it->second.boostElapsed = 0;
                it->second.boostDuration = 0;
            }
            return;
        }

        // Eased transition from `start` to `target` over `duration`. This keeps running under a
        // boost, so the boost is only an offset added on top of it below.
        if (duration == 0)
        {
            current = target;
        }
        else
        {
            elapsed += p_time;
            if (elapsed >= duration)
            {
                elapsed = duration;
                current = target;
            }
            else
            {
                const float p = static_cast<float>(elapsed) / static_cast<float>(duration);
                float e = p;
                if (curve == Curve::EaseIn)
                    e = p * p;
                else if (curve == Curve::Dive)
                    e = p * (0.35f + 0.65f * p); // kick off the line, then keep accelerating
                current = start + (target - start) * e;
            }
        }

        // Boost stacks build on a timer: one every StackIntervalMs, or every StackDiveIntervalMs
        // while diving, capped at the configured stock. The timer only runs while the mount is
        // link-eligible, so a grounded or unlinked mount earns nothing.
        if (stacks < g_cfg.boostStacks)
        {
            const uint32 interval = motion == Motion::Dive ? g_cfg.stackDiveMs : g_cfg.stackMs;
            if (interval)
            {
                stackAccum += p_time;
                if (stackAccum >= interval)
                {
                    stackAccum = 0; // one stack per update: a single long p_time must never fill the bar
                    ++stacks;
                }
            }
        }
        else
            stackAccum = 0;

        // The boost is an additive offset on top of the running transition, fading to nothing over
        // BoostFadeMs: fast at first, then slow. Because the underlying transition keeps running, the
        // speed lands back on exactly the glide the mount had before the boost instead of snapping to
        // the plain state speed.
        if (boostDuration)
        {
            boostElapsed += p_time;
            if (boostElapsed >= boostDuration)
            {
                boostOffset   = 0.0f;
                boostElapsed  = 0;
                boostDuration = 0;
            }
            else
            {
                const float p    = static_cast<float>(boostElapsed) / static_cast<float>(boostDuration);
                const float keep = (1.0f - p) * (1.0f - p); // fast at first, then slow
                boostOffset = boostStart * keep;
            }
        }

        const float applied = std::min(g_cfg.boostSpeed, current + boostOffset);
        // Settled once the base transition is done and any boost has fully faded, so the exact final
        // value is always pushed. Testing `current == target` instead would fire every tick during a
        // boost fade (the base is settled but the offset is not), defeating the SendMs throttle.
        const bool settled = (applied == target && boostDuration == 0);

        sendAccum += p_time;
        if (sendAccum >= g_cfg.sendMs || settled)
        {
            sendAccum = 0;
            if (applied != lastSent)
            {
                player->SetSpeed(MOVE_FLIGHT, applied, true);
                lastSent = applied;
            }
        }

        bool stackGained = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_states.find(guid);
            if (it != g_states.end())
            {
                stackGained          = stacks != prevStacks;
                it->second.current   = current;
                it->second.elapsed   = elapsed;
                it->second.lastSent  = lastSent;
                it->second.sendAccum = sendAccum;
                it->second.sinceLink = sinceLink;
                it->second.stacks    = stacks;
                it->second.stackAccum = stackAccum;
                it->second.boostStart = boostStart;
                it->second.boostOffset = boostOffset;
                it->second.boostElapsed = boostElapsed;
                it->second.boostDuration = boostDuration;
            }
        }
        // A stack gained while diving is pushed at once, so the client button never lags the count the
        // next boost will actually spend.
        if (stackGained)
            SendSpeedAnnounce(player, stacks);
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
        g_cfg.enabled      = sConfigMgr->GetOption<bool>("ManaSpeedFlying.Enable", true);
        g_cfg.climbSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.ClimbSpeed", 2.0f);
        g_cfg.levelSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.LevelSpeed", 2.8f);
        g_cfg.diveSpeed    = sConfigMgr->GetOption<float>("ManaSpeedFlying.DiveSpeed", 8.0f);
        g_cfg.blendMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BlendMs", 5500);
        g_cfg.diveMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.DiveMs", 4000);
        g_cfg.glideMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.GlideMs", 18000);
        g_cfg.climbDecelMs = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.ClimbDecelMs", 3500);
        g_cfg.hoverMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.HoverMs", 500);
        g_cfg.sendMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.SendMs", 150);
        g_cfg.timeoutMs    = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.TimeoutMs", 2500);
        g_cfg.boostAdd     = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostAdd", 4.0f);
        g_cfg.boostSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostSpeed", 16.0f);
        g_cfg.boostFadeMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BoostFadeMs", 4000);
        g_cfg.boostCarryPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostCarryPct", 0.20f);
        g_cfg.boostStacks  = sConfigMgr->GetOption<int>("ManaSpeedFlying.BoostMaxStacks", 3);
        g_cfg.stackMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackIntervalMs", 9000);
        g_cfg.stackDiveMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackDiveIntervalMs", 3000);

        g_cfg.climbSpeed   = std::max(g_cfg.climbSpeed,   0.1f);
        g_cfg.levelSpeed   = std::max(g_cfg.levelSpeed,   0.1f);
        g_cfg.diveSpeed    = std::max(g_cfg.diveSpeed,    0.1f);
        g_cfg.boostAdd     = std::max(g_cfg.boostAdd,     0.0f);
        g_cfg.boostSpeed   = std::max(g_cfg.boostSpeed,   g_cfg.diveSpeed);
        g_cfg.boostStacks  = std::max(0, std::min(g_cfg.boostStacks, 3));
        g_cfg.boostCarryPct = std::max(0.0f, std::min(g_cfg.boostCarryPct, 1.0f));
        if (!g_cfg.boostFadeMs) g_cfg.boostFadeMs = 1;
        if (!g_cfg.stackMs) g_cfg.stackMs = 1;
        if (!g_cfg.stackDiveMs) g_cfg.stackDiveMs = 1;
        if (!g_cfg.sendMs) g_cfg.sendMs = 1;

        // A config change drops every link; the client's next heartbeat picks it back up.
        std::lock_guard<std::mutex> guard(g_lock);
        g_states.clear();
    }
};

void AddSC_ManaSpeedFlying()
{
    new ManaSpeedFlyingPlayer();
    new ManaSpeedFlyingWorld();
}
