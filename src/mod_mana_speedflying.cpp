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
 * The state is applied over a transition whose length and curve depend on where the speed is going:
 * a dive accelerates into DiveSpeed (faster and faster), pulling out to level is a long glide that
 * bleeds speed slowly at first and then hard, and pulling up into a climb holds the built-up speed
 * before braking hard. A climb cut short to level keeps that glide. Other hops keep the
 * span-proportional blend. If the link goes quiet for TimeoutMs, the
 * mount's own speed is restored.
 */

#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cmath>
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
        float  boostAdd     = 2.0f;  // extra flight-speed multiplier the button adds (200%)
        uint32 boostMs      = 4000;  // how long the button boost lasts, ms
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
        uint32 boostLeft = 0;             // ms of button-boost left to apply (0 = none)
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
        return static_cast<uint32>(static_cast<float>(baseMs) * dist / reference);
    }

    // Transition length for entering `next` from the state we are in.
    uint32 DurationFor(const PlayerState& st, Motion next)
    {
        // Leaving a hover (a parked mount) is a deliberately tiny accel/decel zone whatever state
        // it heads into, so taking off responds at once instead of ramping from a standstill.
        if (st.motion == Motion::Hover)
            return g_cfg.hoverMs;
        if (next == Motion::Dive)
            return g_cfg.diveMs;
        if (st.motion == Motion::Dive)
        {
            const float dist = std::fabs(RateFor(next) - st.current);
            if (next == Motion::Climb)
                return ScaledMs(g_cfg.climbDecelMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.climbSpeed));
            if (next == Motion::Level)
                return ScaledMs(g_cfg.glideMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.levelSpeed));
        }
        // A climb entered from a dive and cut short to level still carries the leftover dive speed,
        // so it glides out like a pull-out rather than blending like a fresh climb.
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
        {
            const float dist = std::fabs(RateFor(next) - st.current);
            return ScaledMs(g_cfg.glideMs, dist, std::fabs(g_cfg.diveSpeed - g_cfg.levelSpeed));
        }
        // Everything else keeps the old span-proportional blend.
        const float span = MaxRate() - MinRate();
        if (span <= 0.0f)
            return 0;
        const float dist = std::fabs(RateFor(next) - st.current);
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

        std::lock_guard<std::mutex> guard(g_lock);
        PlayerState& st = g_states[player->GetGUID().GetRawValue()];

        if (advfly != '1')
        {
            // Not an AdvFly mount (or no mount): the update path restores the core's own speed.
            st.eligible = false;
            return;
        }

        // The button asks for a burst. It rides on top of whatever state is being blended and does
        // not change that state, so a boost during a dive stacks with it and expires on its own.
        if (state == 'B')
        {
            st.sinceLink = 0;
            st.boostLeft = g_cfg.boostMs;
            return;
        }

        switch (state)
        {
            case 'C': SetTarget(st, Motion::Climb);   break;
            case 'D': SetTarget(st, Motion::Dive);     break;
            case 'H': SetTarget(st, Motion::Hover);   break;
            default:  SetTarget(st, Motion::Level);   break;
        }
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!g_cfg.enabled || !player || !player->IsFlying() || player->IsInFlight() || player->GetVehicle())
            return;

        const uint64 guid = player->GetGUID().GetRawValue();

        bool   eligible;
        bool   timedOut = false;
        Curve  curve;
        float  current, target, start, lastSent;
        uint32 elapsed, duration, sendAccum, sinceLink, boostLeft;
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
            boostLeft = it->second.boostLeft;
        }

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
                it->second.boostLeft = 0;
            }
            return;
        }

        // Eased transition from `start` to `target` over `duration`.
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

        // A button boost rides on top of the blended state speed and fades in and out over a short
        // ramp, so it reads as a burst without disturbing the state machine above.
        float boost = 0.0f;
        if (boostLeft > 0)
        {
            const float ramp = std::min(static_cast<float>(g_cfg.boostMs) * 0.5f, 300.0f);
            if (ramp > 0.0f)
            {
                const float in  = std::min(1.0f, static_cast<float>(g_cfg.boostMs - boostLeft) / ramp);
                const float out = std::min(1.0f, static_cast<float>(boostLeft) / ramp);
                boost = g_cfg.boostAdd * std::min(in, out);
            }
            else
            {
                boost = g_cfg.boostAdd;
            }
            boostLeft = (boostLeft > p_time) ? boostLeft - p_time : 0;
        }
        const float applied = current + boost;

        sendAccum += p_time;
        if (sendAccum >= g_cfg.sendMs || current == target)
            sendAccum = 0;

        if (applied != lastSent)
        {
            player->SetSpeed(MOVE_FLIGHT, applied, true);
            lastSent = applied;
        }

        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_states.find(guid);
        if (it != g_states.end())
        {
            it->second.current   = current;
            it->second.elapsed   = elapsed;
            it->second.lastSent  = lastSent;
            it->second.sendAccum = sendAccum;
            it->second.sinceLink = sinceLink;
            it->second.boostLeft = boostLeft;
        }
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
        g_cfg.boostAdd     = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostAdd", 2.0f);
        g_cfg.boostMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BoostMs", 4000);

        g_cfg.climbSpeed   = std::max(g_cfg.climbSpeed,   0.1f);
        g_cfg.levelSpeed   = std::max(g_cfg.levelSpeed,   0.1f);
        g_cfg.diveSpeed    = std::max(g_cfg.diveSpeed,    0.1f);
        g_cfg.boostAdd     = std::max(g_cfg.boostAdd,     0.0f);
        if (!g_cfg.sendMs) g_cfg.sendMs = 1;
        if (!g_cfg.boostMs) g_cfg.boostMs = 1;

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
