/*
 * mod-mana-speedflying: vary a flying mount's speed with its flight state.
 *
 * Server-side companion to the client's wxl-mount-anim module. That module already works out the
 * state (climbing / leveling / descending) and whether the mount carries the AdvFly clips, so it
 * says so directly: it whispers an addon message with the "WXLMS" prefix, and this module blends the
 * mount's flight speed toward the speed the state calls for. No server-side guesswork and no mount
 * list -- a mount the client does not drive with AdvFly reports '0' and its speed is left alone.
 *
 * Protocol, an addon whisper:  "WXLMS\t<advfly:0|1><state:L|C|D|H>"
 *   advfly  '1' = the mount is one wxl-mount-anim drives (AdvFly clips), '0' = leave the speed alone
 *   state   C climb, D descend, H hovering, L level / strafe / backward
 *
 * The state is applied with a linear blend over BlendMs, so a dive-to-climb reversal eases the speed
 * down over several seconds. If the link goes quiet for TimeoutMs, the mount's own speed is restored.
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
        Descend
    };

    struct Config
    {
        bool   enabled      = true;
        float  climbSpeed   = 2.0f;  // 200% of base run speed
        float  levelSpeed   = 2.8f;  // 280%
        float  descendSpeed = 3.8f;  // 380%
        uint32 blendMs      = 5500;  // time for the full climb<->descend span
        uint32 sendMs       = 150;   // how often the blended speed may reach the client
        uint32 timeoutMs    = 2500;  // drop the link if no message arrives for this long
    };

    Config g_cfg;

    float RateFor(Motion m)
    {
        switch (m)
        {
            case Motion::Climb:   return g_cfg.climbSpeed;
            case Motion::Descend: return g_cfg.descendSpeed;
            default:              return g_cfg.levelSpeed;
        }
    }

    float MaxRate() { return std::max(g_cfg.climbSpeed, std::max(g_cfg.levelSpeed, g_cfg.descendSpeed)); }
    float MinRate() { return std::min(g_cfg.climbSpeed, std::min(g_cfg.levelSpeed, g_cfg.descendSpeed)); }

    struct PlayerState
    {
        bool   eligible  = false;  // client said the mount is AdvFly-capable
        float  target    = 0.0f;   // rate the current state calls for
        float  current   = 0.0f;   // blended rate being applied
        float  lastSent  = -1.0f;  // last rate handed to SetSpeed; < 0 = nothing applied
        uint32 sendAccum = 0;
        uint32 sinceLink = 0;      // ms since the last link message, for the timeout
    };

    std::mutex g_lock;
    std::unordered_map<uint64, PlayerState> g_states;

    // Records the state a link message named. The first message on a mount snaps the applied rate so
    // a take-off does not spend five seconds ramping from the mount's own speed.
    void SetTarget(PlayerState& st, Motion m)
    {
        st.target = RateFor(m);
        if (!st.eligible)
        {
            st.eligible  = true;
            st.current   = st.target;
            st.lastSent  = -1.0f;
            st.sendAccum = g_cfg.sendMs; // force the first packet out on the next update
        }
        st.sinceLink = 0;
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

        switch (state)
        {
            case 'C': SetTarget(st, Motion::Climb);   break;
            case 'D': SetTarget(st, Motion::Descend); break;
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
        float  current, target, lastSent;
        uint32 sendAccum, sinceLink;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_states.find(guid);
            if (it == g_states.end())
                return;
            eligible  = it->second.eligible;
            current   = it->second.current;
            target    = it->second.target;
            lastSent  = it->second.lastSent;
            sendAccum = it->second.sendAccum;
            sinceLink = it->second.sinceLink;
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
            }
            return;
        }

        // Linear blend: the full climb<->descend span takes BlendMs, so a smaller hop takes less.
        const float span  = MaxRate() - MinRate();
        const float perMs = (g_cfg.blendMs && span > 0.0f) ? span / static_cast<float>(g_cfg.blendMs) : 1.0f;
        const float step  = perMs * static_cast<float>(p_time);

        const float diff = target - current;
        if (std::fabs(diff) <= step)
            current = target;
        else
            current += (diff > 0.0f ? step : -step);

        sendAccum += p_time;
        if (sendAccum >= g_cfg.sendMs || current == target)
            sendAccum = 0;

        if (current != lastSent)
        {
            player->SetSpeed(MOVE_FLIGHT, current, true);
            lastSent = current;
        }

        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_states.find(guid);
        if (it != g_states.end())
        {
            it->second.current   = current;
            it->second.lastSent  = lastSent;
            it->second.sendAccum = sendAccum;
            it->second.sinceLink = sinceLink;
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
        g_cfg.descendSpeed = sConfigMgr->GetOption<float>("ManaSpeedFlying.DescendSpeed", 3.8f);
        g_cfg.blendMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BlendMs", 5500);
        g_cfg.sendMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.SendMs", 150);
        g_cfg.timeoutMs    = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.TimeoutMs", 2500);

        g_cfg.climbSpeed   = std::max(g_cfg.climbSpeed,   0.1f);
        g_cfg.levelSpeed   = std::max(g_cfg.levelSpeed,   0.1f);
        g_cfg.descendSpeed = std::max(g_cfg.descendSpeed, 0.1f);
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
