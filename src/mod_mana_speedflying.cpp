/*
 * mod-mana-speedflying: vary a flying mount's speed with its flight state.
 *
 * Server-side companion to the client's wxl-mana-speedflying module. That module already works out the
 * state (climbing / leveling / diving) and whether the mount carries the AdvFly clips, so it
 * says so directly: it whispers an addon message with the "WXLMS" prefix, and this module blends the
 * mount's flight speed toward the speed the state calls for. No server-side guesswork and no mount
 * list -- a mount the client does not drive with AdvFly reports '0' and its speed is left alone.
 *
 * Protocol, an addon whisper:  "WXLMS\t<advfly:0|1><state:L|C|D|H>"
 *   advfly  '1' = the mount is one wxl-mana-speedflying drives (AdvFly clips), '0' = leave the speed alone
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
 *
 * The talent tree rides a second addon prefix, "WXLTT", so the link parser above is untouched:
 *
 *   client -> server:  WXLTT\tW\t<talentId>   learn one rank
 *                      WXLTT\tB\t<id>:<target>[,<id>:<target>,...]   learn up to an absolute rank each
 *                      WXLTT\tR\t<1|2|3|A>    refund a tree (1 dive, 2 glide, 3 wings) or A for everything
 *                      WXLTT\tQ                request the tree
 *   server -> client:  T:<points>,<spent>,<unspent>,<n>,<ranks>,P<permille>,N<yards>
 *                      (<ranks> is n characters, one per talent in table order: "0"-"9", then "a"-"z")
 *                      P:<permille>,<yards>   the partial point alone, for the progress bar
 *                      F:<reason>             a refusal, echoed for the panel
 *
 * The tally rides a spend, a refund or a 'Q' -- anything that can move a rank. The partial point moves
 * on its own, about twice a second while flight accrues yards, and answering that with a whole tally
 * (every rank pair, several hundred bytes) is what made the module chatty. 'P' carries the two bar
 * fields and nothing else, so the same information costs tens of bytes. A client that does not know
 * 'P' ignores the body and keeps the bar it last drew.
 *
 * Points are earned per character by flying: yards travelled on an AdvFly mount while the link is
 * up, at Talents.YardsPerPoint per whole point, capped per rolling minute. Ranks are authoritative
 * here and the client only renders them. See the README for the talent list and the wire contract.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // --- the talent tree -----------------------------------------------------------------------
    // The ids are the wire contract: they ride "WXLTT" as integers and key the
    // character_speedflying_talents table. The table below is generated from talents.json by
    // gen_headers.py and must match the client's table id for id. Never renumber a stored id.
    enum Tree
    {
        TreeDive = 0,
        TreeGlide,
        TreeWings,
        TreeNone
    };

    enum Kind
    {
        KindCore = 0,
        KindNode,
        KindBridge,
        KindCap,
        KindLinked
    };

    // Every effect scales one leaf of Config, or is a flag the tree reads by id (Aftershock and the two
    // client-side thresholds, which never enter the multiplier array).
    enum Effect
    {
    E_SpeedAll,  // speed_all
    E_DiveMs,  // dive_ms
    E_ClimbDecel,  // climb_decel
    E_DiveSpeed,  // dive_speed
    E_BlendMs,  // blend_ms
    E_DiveYield,  // dive_yield
    E_ShallowPull,  // shallow_pull
    E_Aftershock,  // aftershock
    E_BoostCarry,  // boost_carry
    E_BoostAddDive,  // boost_add_dive
    E_SpeedCap,  // speed_cap
    E_GlideMs,  // glide_ms
    E_ClimbSpeed,  // climb_speed
    E_LevelSpeed,  // level_speed
    E_YardStep,  // yard_step
    E_HoverMs,  // hover_ms
    E_LevelYield,  // level_yield
    E_BoostGlide,  // boost_glide
    E_BoostAdd,  // boost_add
    E_StackInterval,  // stack_interval
    E_StackDiveInterval,  // stack_dive_interval
    E_BoostFade,  // boost_fade
    E_BoostStacks,  // boost_stacks
    E_BoostSpeed,  // boost_speed
    E_StartStacks,  // start_stacks
    E_ClimbThreshold,  // climb_threshold
    E_DiveThreshold,  // dive_threshold
    E_Count
    };

    constexpr int kEffectCount = static_cast<int>(E_Count);

    struct TalentEffect
    {
        uint8 effect;  // an Effect
        float per;     // added per rank: a percentage is a fraction (0.08 = 8%), degrees and counts are raw
    };

    struct TalentDef
    {
        uint16       id;
        uint8        tree;        // Tree
        uint8        ring;        // 0 core, 1-5 tree rows, 3 and 5 bridges, 6 caps, 7 linked
        uint8        kind;        // Kind
        uint8        maxRank;
        uint8        nEffects;
        TalentEffect effects[3];
        uint16       parents[3];  // ids, 0 = none; any parent at full rank opens the node
        const char*  name;
        const char*  desc;
    };

    const TalentDef kTalents[] =
    {
    {100, 3, 0, 0, 1, 1, {{E_SpeedAll, 0.2f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {0, 0, 0}, "Windborn", "All flight speeds +20%."},  // Windborn
    {101, 0, 1, 1, 3, 1, {{E_DiveMs, -0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Steep Descent", "Dives start 2% faster per rank."},  // Steep Descent
    {102, 0, 1, 1, 3, 1, {{E_ClimbDecel, 0.03f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Held Breath", "Keep your dive speed 3% longer when you climb, per rank."},  // Held Breath
    {103, 0, 2, 1, 3, 1, {{E_DiveSpeed, 0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {101, 0, 0}, "Terminal Velocity", "Dive speed +2% per rank."},  // Terminal Velocity
    {104, 0, 2, 1, 3, 1, {{E_BlendMs, -0.03f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {101, 102, 0}, "Sharp Banking", "Speed changes 3% faster per rank."},  // Sharp Banking
    {105, 0, 2, 1, 3, 1, {{E_DiveYield, 0.04f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {102, 0, 0}, "Dive Yield", "4% more progress while diving, per rank."},  // Dive Yield
    {106, 0, 3, 1, 3, 1, {{E_ShallowPull, 9.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {103, 104, 0}, "Shallow Pull", "Pull out of a dive 9 degrees shallower, per rank."},  // Shallow Pull
    {107, 0, 3, 1, 1, 1, {{E_Aftershock, 1.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {104, 105, 0}, "Aftershock", "Releasing the boost key in a dive boosts at once."},  // Aftershock
    {108, 0, 4, 1, 3, 1, {{E_DiveSpeed, 0.04f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {106, 0, 0}, "Gravity Well", "Dive speed +4% per rank."},  // Gravity Well
    {109, 0, 4, 1, 3, 1, {{E_BoostCarry, 0.06f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {106, 107, 0}, "Rebound", "Keep 6% more of a boost after it ends, per rank."},  // Rebound
    {110, 0, 4, 1, 2, 1, {{E_BoostAddDive, 0.16f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {107, 0, 0}, "Dive Bomb", "Boosts add 16% more speed while diving, per rank."},  // Dive Bomb
    {111, 0, 5, 1, 1, 1, {{E_DiveMs, -0.35f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {108, 109, 0}, "Meteor", "Dives start 35% faster."},  // Meteor
    {112, 0, 5, 1, 2, 1, {{E_DiveSpeed, 0.1f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {109, 0, 0}, "Hammerfall", "Dive speed +10% per rank."},  // Hammerfall
    {113, 0, 5, 1, 2, 2, {{E_SpeedCap, 200.0f}, {E_DiveSpeed, 0.04f}, {E_Count, 0.0f}}, {109, 110, 0}, "Redline", "Dive speed +4% per rank and a higher speed limit."},  // Redline
    {114, 1, 1, 1, 3, 1, {{E_GlideMs, 0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Long Glide", "Glide 2% longer after a dive, per rank."},  // Long Glide
    {115, 1, 1, 1, 3, 1, {{E_ClimbSpeed, 0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Updraft", "Climb speed +1% per rank."},  // Updraft
    {116, 1, 2, 1, 3, 1, {{E_LevelSpeed, 0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {114, 0, 0}, "Skyward Path", "Level flight speed +2% per rank."},  // Skyward Path
    {117, 1, 2, 1, 3, 1, {{E_YardStep, -0.05f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {114, 115, 0}, "Slipstream", "Point costs grow 5% slower, per rank."},  // Slipstream
    {118, 1, 2, 1, 2, 1, {{E_HoverMs, -0.12f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {115, 0, 0}, "Hover Poise", "Leave a hover 12% faster, per rank."},  // Hover Poise
    {119, 1, 3, 1, 3, 1, {{E_LevelYield, 0.05f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {116, 117, 0}, "Long Haul", "5% more progress in level flight and climbs, per rank."},  // Long Haul
    {120, 1, 3, 1, 1, 1, {{E_BoostGlide, 0.09f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {117, 118, 0}, "Glide Anchor", "Boosts keep 9% more of your glide speed."},  // Glide Anchor
    {121, 1, 4, 1, 3, 1, {{E_LevelSpeed, 0.05f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {119, 0, 0}, "Jetstream", "Level flight speed +5% per rank."},  // Jetstream
    {122, 1, 4, 1, 3, 1, {{E_BlendMs, -0.05f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {119, 120, 0}, "Draft", "Speed changes 5% faster per rank."},  // Draft
    {123, 1, 4, 1, 3, 1, {{E_ClimbSpeed, 0.06f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {120, 0, 0}, "Updraft Master", "Climb speed +6% per rank."},  // Updraft Master
    {124, 1, 5, 1, 1, 1, {{E_GlideMs, 0.5f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {121, 122, 0}, "Eternal Current", "Glide 50% longer after a dive."},  // Eternal Current
    {125, 1, 5, 1, 2, 1, {{E_LevelSpeed, 0.1f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {122, 0, 0}, "Highway", "Level flight speed +10% per rank."},  // Highway
    {126, 1, 5, 1, 2, 1, {{E_BoostGlide, 0.2f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {122, 123, 0}, "Skybound", "Boosts keep 20% more of your glide speed, per rank."},  // Skybound
    {127, 2, 1, 1, 3, 1, {{E_BoostAdd, 0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Iron Wings", "Boosts add 1% more speed per rank."},  // Iron Wings
    {128, 2, 1, 1, 3, 1, {{E_StackInterval, -0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {100, 0, 0}, "Quick Recovery", "Boost charges refill 1% faster per rank."},  // Quick Recovery
    {129, 2, 2, 1, 3, 1, {{E_StackDiveInterval, -0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {127, 0, 0}, "Diving Wings", "Boost charges refill 2% faster while diving, per rank."},  // Diving Wings
    {130, 2, 2, 1, 3, 1, {{E_BoostFade, 0.02f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {127, 128, 0}, "Afterburn", "Boosts last 2% longer per rank."},  // Afterburn
    {131, 2, 2, 1, 1, 1, {{E_BoostStacks, 1.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {128, 0, 0}, "Wing Reserve", "Hold 1 more boost charge."},  // Wing Reserve
    {132, 2, 3, 1, 3, 1, {{E_BoostCarry, 0.06f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {129, 130, 0}, "Banked Momentum", "Keep 6% more of a boost after it ends, per rank."},  // Banked Momentum
    {133, 2, 3, 1, 1, 1, {{E_BoostSpeed, 0.04f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {130, 131, 0}, "Overcharge", "Boost speed limit +4%."},  // Overcharge
    {134, 2, 4, 1, 3, 1, {{E_BoostAdd, 0.05f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {132, 0, 0}, "Surge Cells", "Boosts add 5% more speed per rank."},  // Surge Cells
    {135, 2, 4, 1, 3, 1, {{E_StackInterval, -0.03f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {132, 133, 0}, "Fast Cells", "Boost charges refill 3% faster per rank."},  // Fast Cells
    {136, 2, 4, 1, 1, 1, {{E_StartStacks, 1.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {133, 0, 0}, "Reserve Charge", "Start each flight with 1 more boost charge."},  // Reserve Charge
    {137, 2, 5, 1, 1, 1, {{E_BoostStacks, 1.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {134, 135, 0}, "Twin Vault", "Hold 1 more boost charge."},  // Twin Vault
    {138, 2, 5, 1, 2, 1, {{E_BoostSpeed, 0.06f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {135, 0, 0}, "Overdrive", "Boost speed limit +6% per rank."},  // Overdrive
    {139, 2, 5, 1, 2, 1, {{E_BoostFade, 0.1f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {135, 136, 0}, "Last Gasp", "Boosts last 10% longer per rank."},  // Last Gasp
    {140, 0, 3, 2, 1, 1, {{E_ClimbThreshold, 25.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {103, 118, 0}, "Still Air", "Climbing starts at 25 degrees instead of 15."},  // Still Air
    {141, 1, 3, 2, 1, 1, {{E_DiveThreshold, 25.0f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {116, 131, 0}, "Tucked Wings", "Diving starts at 25 degrees instead of 35."},  // Tucked Wings
    {142, 2, 3, 2, 1, 1, {{E_ClimbDecel, 0.24f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {105, 129, 0}, "Rising Current", "Climbs keep 24% more of your speed."},  // Rising Current
    {143, 0, 5, 2, 2, 2, {{E_LevelSpeed, 0.04f}, {E_DiveSpeed, 0.04f}, {E_Count, 0.0f}}, {108, 123, 0}, "Skimmer", "Level and dive speed +4% per rank."},  // Skimmer
    {144, 1, 5, 2, 2, 2, {{E_BoostAdd, 0.06f}, {E_LevelSpeed, 0.03f}, {E_Count, 0.0f}}, {121, 136, 0}, "Windfall", "Boosts add 6% more speed and level speed +3%, per rank."},  // Windfall
    {145, 2, 5, 2, 2, 2, {{E_StackDiveInterval, -0.08f}, {E_DiveSpeed, 0.03f}, {E_Count, 0.0f}}, {110, 134, 0}, "Thunderhead", "Diving charges refill 8% faster and dive speed +3%, per rank."},  // Thunderhead
    {146, 0, 6, 3, 1, 2, {{E_DiveSpeed, 0.5f}, {E_DiveMs, -0.3f}, {E_Count, 0.0f}}, {111, 112, 113}, "Falcon's Stoop", "Dive speed +50% and dives start 30% faster."},  // Falcon's Stoop
    {147, 1, 6, 3, 1, 2, {{E_LevelSpeed, 0.4f}, {E_ClimbSpeed, 0.4f}, {E_Count, 0.0f}}, {124, 125, 126}, "Endless Horizon", "Level and climb speed +40%."},  // Endless Horizon
    {148, 2, 6, 3, 1, 3, {{E_BoostStacks, 1.0f}, {E_BoostAdd, 0.25f}, {E_BoostSpeed, 0.08f}}, {137, 138, 139}, "Tempest Heart", "1 more boost charge, boosts add 25% more speed, boost speed limit +8%."},  // Tempest Heart
    {149, 0, 7, 4, 35, 1, {{E_SpeedAll, 0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {146, 0, 0}, "Stoop Mastery", "All flight speeds +1% per rank."},  // Stoop Mastery
    {150, 1, 7, 4, 35, 1, {{E_SpeedAll, 0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {147, 0, 0}, "Horizon Mastery", "All flight speeds +1% per rank."},  // Horizon Mastery
    {151, 2, 7, 4, 35, 1, {{E_SpeedAll, 0.01f}, {E_Count, 0.0f}, {E_Count, 0.0f}}, {148, 0, 0}, "Tempest Mastery", "All flight speeds +1% per rank."},  // Tempest Mastery
    };

    constexpr int kTalentCount = static_cast<int>(sizeof(kTalents) / sizeof(kTalents[0]));

    // Table index of a wire id, or -1. Both the single-rank and the batch path go through this, so an
    // unknown id is refused identically either way. Rows from the retired tree (ids 1..17) are not in
    // the table, so a stored row for one is skipped on load and its points come back unspent.
    int IndexOfTalentId(int id)
    {
        for (int i = 0; i < kTalentCount; ++i)
            if (kTalents[i].id == id)
                return i;
        return -1;
    }

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
        float  climbSpeed   = 2.2f;  // 220% of base run speed
        float  levelSpeed   = 3.2f;  // 320%
        float  diveSpeed    = 6.0f;  // 600%
        uint32 blendMs      = 5500;  // default blend, proportional to the span it must cover
        uint32 diveMs       = 4000;  // accelerate into a dive
        uint32 glideMs      = 18000; // long ease-in glide from a dive back to level
        uint32 climbDecelMs = 3500;  // dive -> climb: hold the dive speed, then brake hard
        uint32 hoverMs      = 500;   // short accel/decel zone leaving a hover
        uint32 sendMs       = 100;   // how often the blended speed may reach the client
        uint32 timeoutMs    = 2500;  // drop the link if no message arrives for this long
        float  boostAdd     = 4.0f;  // 400%: the speed one boost adds back to the glide
        float  boostSpeed   = 12.0f; // 1200%: hard ceiling a boost may lift the speed to
        uint32 boostFadeMs  = 4000;  // how fast a boost's added speed bleeds back to the state speed
        float  boostCarryPct = 0.50f; // fraction of a boost folded into the glide to outlast the fade
        float  boostGlidePct = 0.70f; // fraction of the ordinary glide a boost's carried speed may buy
        int    boostStacks  = 3;     // most boost stacks the button can hold
        uint32 stackMs      = 9000;  // ms between boost stacks in normal flight
        uint32 stackDiveMs  = 3000;  // ms between boost stacks while diving
        // Talent-derived, never read from the INI. The level-out settle belonged to the retired Skyward
        // Path talent: levelElev stays 0 and settleMs is read by nothing.
        float  shallowPullDeg = 30.0f;  // Shallow Pull: degrees a pull-out may be and still keep its glide
        float  boostAddDive   = 1.0f;   // Dive Bomb: multiplier on boostAdd for a boost pressed while diving
        int    startStacks    = 0;      // Reserve Charge: stacks granted when a flight starts
        float  levelElev      = 0.0f;
        uint32 settleMs       = 1500;
    };

    // Talent-tree configuration. Talents.Enable is the master switch for earning and spending;
    // the effects are applied through an effective per-player Config, never by mutating g_cfg.
    struct TalentConfig
    {
        bool   enabled        = true;
        uint32 maxPoints      = 103;    // lifetime cap: the most ranks any legal build can hold (core 1 + 102)
        float  baseYards      = 100.0f; // yards of flight for the first point
        float  yardStep       = 1.00f;  // the opening step: point 1 needs 100% more yards than point 0
        float  yardStepLate   = 0.10f;  // the step once the curve has soothed
        uint32 yardSoothe     = 25;     // the step eases linearly from yardStep to yardStepLate over this many points
        uint32 maxPointsPerMinute = 4;  // rolling-window cap; 0 disables (an AFK orbit is free)
        uint32 flushMs        = 20000;  // how often a dirty progress map is written back
        float  diveAccrual    = 1.5f;   // yards count this much more while diving -- diving pays best
        float  teleportYards  = 200.0f; // a tick delta past this is a teleport, not flight
        uint32 respecCost     = 0;      // copper per respec; 0 (the default) is free
    };

    Config g_cfg;
    TalentConfig g_talentCfg;

    // Multipliers the tree produces, recomputed from a rank set. Read by the award loop and by the
    // per-player effective config, never by g_cfg.
    struct Talents
    {
        int   ranks[kTalentCount] = {};
        float eff[kEffectCount];

        Talents() { for (int i = 0; i < kEffectCount; ++i) eff[i] = 1.0f; }

        int Spent() const { int n = 0; for (int i = 0; i < kTalentCount; ++i) n += ranks[i]; return n; }
    };

    // The rule set both the single-rank and the batch path run (PLAN section 1). Returns nullptr when
    // the talent may take a rank, else the refusal reason the panel shows. Mirrored by TalentUI.cpp.
    // A cap is refused while another cap holds a rank; any non-core node opens once ANY parent is at full rank.
    // ParentFull is the one definition of that, shared with the refund cascade.
    struct BridgeReach { uint16 bridge; uint16 node[2]; };
    constexpr BridgeReach kBridgeReach[] = { {140, {108, 123}}, {141, {121, 136}}, {142, {110, 134}},
                                       {143, {111, 126}}, {144, {124, 139}}, {145, {113, 137}} };

    bool ParentFull(const int* ranks, int index)
    {
        for (int k = 0; k < 3; ++k)
        {
            const int pi = kTalents[index].parents[k] ? IndexOfTalentId(kTalents[index].parents[k]) : -1;
            if (pi >= 0 && ranks[pi] >= kTalents[pi].maxRank)
                return true;
        }
        // A bridge (ring 3 or 5) at full rank opens one talent of the next ring in each tree it joins (the one closest to it, kBridgeReach),
        // once the bridge's parent in the OTHER tree is full: it leads forward into the other tree, never back to the
        // tree the player came from.
        if (kTalents[index].kind == KindNode)
        {
            for (const BridgeReach& r : kBridgeReach)
            {
                const int b = IndexOfTalentId(r.bridge);
                if (b < 0 || ranks[b] < kTalents[b].maxRank)
                    continue;
                for (int s = 0; s < 2; ++s)
                {
                    const int other = IndexOfTalentId(r.node[1 - s]);
                    if (r.node[s] != kTalents[index].id || other < 0)
                        continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        const int pi = kTalents[b].parents[k] ? IndexOfTalentId(kTalents[b].parents[k]) : -1;
                        if (pi >= 0 && kTalents[pi].tree == kTalents[other].tree && ranks[pi] >= kTalents[pi].maxRank)
                            return true;
                    }
                }
            }
        }
        return false;
    }

    // A plain talent is refused while another plain talent of the same tree and ring is at full rank: a full
    // talent closes its ring, so the next ring is the only way on.
    bool SiblingFull(const int* ranks, int index)
    {
        const TalentDef& d = kTalents[index];
        if (d.kind != KindNode)
            return false;
        for (int i = 0; i < kTalentCount; ++i)
            if (i != index && kTalents[i].kind == KindNode && kTalents[i].tree == d.tree &&
                kTalents[i].ring == d.ring && ranks[i] >= kTalents[i].maxRank)
                return true;
        return false;
    }

    // A bridge is refused while another bridge joining the same two trees holds a rank: the second would lead back
    // to the tree the player came from. A bridge's two trees are the trees of the two nodes it reaches.
    bool BridgeTaken(const int* ranks, int index)
    {
        if (kTalents[index].kind != KindBridge)
            return false;
        auto pairOf = [](uint16 bridge, int& a, int& b)
        {
            for (const BridgeReach& r : kBridgeReach)
                if (r.bridge == bridge)
                {
                    a = kTalents[IndexOfTalentId(r.node[0])].tree;
                    b = kTalents[IndexOfTalentId(r.node[1])].tree;
                    return;
                }
        };
        int a = -1, b = -1;
        pairOf(kTalents[index].id, a, b);
        for (const BridgeReach& r : kBridgeReach)
        {
            const int i = IndexOfTalentId(r.bridge);
            if (i < 0 || i == index || ranks[i] <= 0)
                continue;
            int c = -1, e = -1;
            pairOf(r.bridge, c, e);
            if ((a == c && b == e) || (a == e && b == c))
                return true;
        }
        return false;
    }

    const char* CanRank(const Talents& t, int index, int unspent)
    {
        const TalentDef& d = kTalents[index];
        if (t.ranks[index] >= d.maxRank)
            return "max-rank";
        if (BridgeTaken(t.ranks, index))
            return "bridge-taken";
        if (d.kind == KindCap)
        {
            for (int i = 0; i < kTalentCount; ++i)
                if (i != index && kTalents[i].kind == KindCap && t.ranks[i] > 0)
                    return "cap-taken";
        }
        if (d.kind != KindCore && !ParentFull(t.ranks, index))
            return "locked";
        if (SiblingFull(t.ranks, index))
            return "ring-full";
        if (unspent <= 0)
            return "no-points";
        return nullptr;
    }

    // The challenge curve lives here once and is read by the award loop. The yard_step talents shorten
    // the compounding step rather than the rate yards are banked at, so a rank can only shorten the tree.
    // `index` is the point being paid for: the 0th point costs BaseYards.
    double PointYards(uint32 index, const Talents& t)
    {
        // Slipstream (yard_step) shrinks the compounding step, never below a tenth of the config value.
        const double mult = std::max(0.1, static_cast<double>(t.eff[E_YardStep]));
        double need = static_cast<double>(g_talentCfg.baseYards);
        const double early = static_cast<double>(g_talentCfg.yardStep);
        const double late = static_cast<double>(g_talentCfg.yardStepLate);
        const double soothe = std::max(1.0, static_cast<double>(g_talentCfg.yardSoothe));
        for (uint32 i = 0; i < index; ++i)
        {
            const double k = std::max(0.0, 1.0 - i / soothe);   // 1 at the first point, 0 once soothed
            need *= 1.0 + (late + (early - late) * k) * mult;
        }
        return need;
    }

    // The progress bar's fill, in permille of the point being paid for. yardPartial is a yard count,
    // not a fraction, so it is the next point's own cost that it is measured against. A tree at the
    // cap has no next point: the bar is full. One definition, so the tally and the bar tick agree.
    uint32 PartialPermille(uint32 points, double yardPartial, const Talents& t)
    {
        if (points >= g_talentCfg.maxPoints) return 1000u;
        const double need = PointYards(points, t);
        if (need <= 0.0) return 1000u;
        const double q = yardPartial / need * 1000.0;
        return q <= 0.0 ? 0u : (q >= 1000.0 ? 1000u : static_cast<uint32>(q));
    }

    // The shared cap on a talent-raised state speed, as a percentage. Applied to the three base state
    // speeds *before* boostSpeed is derived from their max(), or the derivation undoes the clamp. Read
    // through ApplyTalents' capPct argument, never directly by a caller.
    float g_speedCapPct = 1600.0f;

    // A player's flush timer fired. The write itself rides the world thread's next tick, outside
    // g_lock: the update path must not block on the character database while it holds the lock.
    std::atomic<bool> g_flushWanted = false;

    // Rebuilds a rank set's multipliers from the table: every effect starts at 1.0 and takes
    // per * rank for each effect row of each ranked talent. Rows on one effect sum, never compound.
    void BuildTalents(const int ranks[kTalentCount], Talents& out)
    {
        out = Talents();
        for (int i = 0; i < kTalentCount; ++i)
        {
            const int r = std::max(0, std::min<int>(ranks[i], kTalents[i].maxRank));
            out.ranks[i] = r;
            if (!r)
                continue;
            for (int k = 0; k < kTalents[i].nEffects; ++k)
            {
                const uint8 e = kTalents[i].effects[k].effect;
                // Flags are read by id, not multiplied in: Aftershock and the client-side thresholds.
                if (e == E_Aftershock || e == E_ClimbThreshold || e == E_DiveThreshold)
                    continue;
                out.eff[e] += kTalents[i].effects[k].per * r;
            }
        }
        // A negative multiplier would invert a duration into a negative one.
        for (int i = 0; i < kEffectCount; ++i)
            out.eff[i] = std::max(0.0f, out.eff[i]);
    }


    // The same three overrides, applied to a per-player copy. diveSpeed is raised before boostSpeed is
    // derived from it, which is the whole point of clamping the base states and not the ceiling.
    // capPct is passed in, not read from g_speedCapPct: a caller that is not holding g_lock (the login
    // thread) would otherwise read the shared config while a reload is writing it.
    void ApplyTalents(Config& c, const Talents& t, float capPct)
    {
        // Every eff[] is already 1 + the summed ranks, so each line is one multiplier, applied once.
        // Durations are floored at a tenth so a stack of cuts cannot reach zero.
        const float all = t.eff[E_SpeedAll];
        c.climbSpeed *= all * t.eff[E_ClimbSpeed];
        c.levelSpeed *= all * t.eff[E_LevelSpeed];
        c.diveSpeed  *= all * t.eff[E_DiveSpeed];
        c.diveMs      = static_cast<uint32>(static_cast<float>(c.diveMs) * std::max(0.1f, t.eff[E_DiveMs]));
        c.glideMs     = static_cast<uint32>(static_cast<float>(c.glideMs) * t.eff[E_GlideMs]);
        c.climbDecelMs = static_cast<uint32>(static_cast<float>(c.climbDecelMs) * t.eff[E_ClimbDecel]);
        c.blendMs     = static_cast<uint32>(static_cast<float>(c.blendMs) * std::max(0.1f, t.eff[E_BlendMs]));
        c.hoverMs     = static_cast<uint32>(static_cast<float>(c.hoverMs) * std::max(0.1f, t.eff[E_HoverMs]));
        // Boost add and ceiling take the all-speed bonus too, or a boost (a flat add under a flat ceiling) would
        // swallow every state-speed talent and leave only the Wings tree worth buying.
        c.boostAdd   *= all * t.eff[E_BoostAdd];
        c.boostAddDive = t.eff[E_BoostAddDive];
        c.boostSpeed *= all * t.eff[E_BoostSpeed];
        c.boostFadeMs = static_cast<uint32>(static_cast<float>(c.boostFadeMs) * t.eff[E_BoostFade]);
        c.boostCarryPct = std::min(1.0f, c.boostCarryPct + (t.eff[E_BoostCarry] - 1.0f));
        c.boostGlidePct = std::min(1.0f, c.boostGlidePct + (t.eff[E_BoostGlide] - 1.0f));
        c.boostStacks = c.boostStacks + static_cast<int>(t.eff[E_BoostStacks] - 1.0f + 0.5f);
        c.startStacks = static_cast<int>(t.eff[E_StartStacks] - 1.0f + 0.5f);
        c.stackMs     = static_cast<uint32>(static_cast<float>(c.stackMs) * std::max(0.1f, t.eff[E_StackInterval]));
        c.stackDiveMs = static_cast<uint32>(static_cast<float>(c.stackDiveMs) *
                                            std::max(0.1f, t.eff[E_StackDiveInterval]));
        // The level-out settle belonged to the retired Skyward Path talent: no elevated glide any more.
        c.levelElev = 0.0f;

        // One shared cap (Redline adds points to it), applied to the base states before the ceiling is
        // derived from them.
        const float cap = (capPct + (t.eff[E_SpeedCap] - 1.0f)) * 0.01f;
        c.climbSpeed = std::min(c.climbSpeed, cap);
        c.levelSpeed = std::min(c.levelSpeed, cap);
        c.diveSpeed  = std::min(c.diveSpeed,  cap);
        // Shallow Pull is additive degrees, not a percentage: 30 degrees plus 15 per rank.
        c.shallowPullDeg = 30.0f + (t.eff[E_ShallowPull] - 1.0f);
        c.boostSpeed = std::max(c.boostSpeed,
                                std::max(c.climbSpeed, std::max(c.levelSpeed, c.diveSpeed)));

        if (!c.diveMs) c.diveMs = 1;
        if (!c.glideMs) c.glideMs = 1;
        if (!c.climbDecelMs) c.climbDecelMs = 1;
        if (!c.stackMs) c.stackMs = 1;
        if (!c.stackDiveMs) c.stackDiveMs = 1;
        if (c.boostStacks > 7) c.boostStacks = 7; // 3 configured + 4 from Wing Reserve, Twin Vault, Tempest Heart
    }

    // --- per-character talent progress -------------------------------------------------------
    // Unspent points are derived (points - sum of ranks), never stored twice. yard_partial carries
    // the fraction under a whole point across a flush and across a relog.
    struct Progress
    {
        Talents  tree;                      // the rank set and its multipliers
        Config   eff;                       // g_cfg with this player's talents applied
        uint32   points     = 0;            // lifetime points earned
        double   yardPartial = 0.0;         // yards not yet worth a whole point
        bool     dirty      = false;        // something to write back
        bool     loaded     = false;        // the two tables have been read
        uint32   accMs      = 0;            // ms since the last flush check
        // The rolling minute's cap counts the points paid inside it, oldest first, so the sample
        // that leaves the window is the one dropped. Halving the count every minute instead (the
        // earlier decay) held an unbroken flight at half the configured rate.
        std::vector<uint32> recentMs;       // age in ms of each point paid inside the window
        // The partial point the progress bar last showed (a permille), so a bar tick that moved it can
        // push a tally once. A points change forces one regardless.
        uint32   shownPartial = 0;
        uint32   shownPoints  = 0;
        uint32   progMs       = 0;            // ms since the progress bar was last re-announced

        // The flight position the previous accrual tick saw, so a tick pays the distance it covered.
        bool     havePos    = false;
        float    lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;

        int Spent() const { return tree.Spent(); }
        int Unspent() const
        {
            const int n = static_cast<int>(points) - Spent();
            return n > 0 ? n : 0;
        }
        // Both base values are passed in rather than read from the globals: LoadProgress runs on the
        // login thread, and g_cfg/g_speedCapPct are only writable under g_lock.
        void Rebuild(const Config& base, float capPct)
        {
            // Copy the ranks first: BuildTalents resets `tree` (including tree.ranks), so passing the
            // live array across would zero the very ranks it is about to read. And start the effective
            // config from the base every time -- applying talents on top of an already-talented copy
            // would compound them on every rank bought.
            int ranks[kTalentCount];
            for (int i = 0; i < kTalentCount; ++i)
                ranks[i] = tree.ranks[i];
            BuildTalents(ranks, tree);
            eff = base;                       // the base ...
            ApplyTalents(eff, tree, capPct);  // ... then this character's ranks on top
        }
    };

    // The character guid the two tables key on is the guid's *low* half: `characters.guid` is
    // GetGUID().GetCounter(), which for a player is exactly the low 32 bits of GetRawValue() (a player
    // guid has no entry bits). Keying the SQL on the raw value instead would never match a row.
    uint32 DbGuid(uint64 raw) { return static_cast<uint32>(raw & 0xFFFFFFFFull); }

    // A module cannot add a CharacterDatabasePreparedStatement entry -- that enum lives in the core.
    // These queries interpolate integers only (guids, talent ids, ranks), never a string.
    // `base` and `capPct` are an already-taken copy of the published config. This runs on the login
    // thread, before anything is under g_lock: reading the globals here and letting OnAfterConfigLoad
    // assign them would be an unsynchronized race. The caller copies under the lock instead.
    void LoadProgress(uint64 guid, const Config& base, float capPct, Progress& p)
    {
        p.points = 0;
        p.yardPartial = 0.0;
        p.shownPartial = 0;
        p.shownPoints = 0;
        p.tree = Talents();

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT `points`, `yard_partial` FROM `character_speedflying` WHERE `guid` = {}",
                DbGuid(guid)))
        {
            if (Field* f = r->Fetch())
            {
                p.points      = f[0].Get<uint32>();
                p.yardPartial = f[1].Get<double>();
            }
        }

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT `talent_id`, `rank` FROM `character_speedflying_talents` WHERE `guid` = {}",
                DbGuid(guid)))
        {
            do
            {
                Field* f = r->Fetch();
                if (!f)
                    break;
                // A row whose id is not in kTalents (a retired tree id) matches nothing and is skipped, so
                // its points come back as unspent on the next tally.
                const int id = f[0].Get<uint16>();
                const int rank = f[1].Get<uint8>();
                for (int i = 0; i < kTalentCount; ++i)
                    if (kTalents[i].id == id)
                        p.tree.ranks[i] = std::max(0, std::min<int>(rank, kTalents[i].maxRank));
            } while (r->NextRow());
        }

        p.Rebuild(base, capPct);
        p.dirty  = false;
        p.loaded = true;
    }

    // Writes the progress row, the ranks that changed and nothing else. Clears `dirty` on whatever it
    // is handed: a caller writing a snapshot under the lock must clear the live flag itself.
    void WriteProgress(uint64 guid, Progress& p)
    {
        CharacterDatabase.Execute(
            "INSERT INTO `character_speedflying` (`guid`, `points`, `yard_partial`) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE `points` = VALUES(`points`), `yard_partial` = VALUES(`yard_partial`)",
            DbGuid(guid), p.points, p.yardPartial);

        // Talent rows exist only while rank >= 1; a respec deletes them. Rewriting the whole (small)
        // set keeps the delete path honest without tracking which row moved.
        CharacterDatabase.Execute("DELETE FROM `character_speedflying_talents` WHERE `guid` = {}",
                                   DbGuid(guid));
        for (int i = 0; i < kTalentCount; ++i)
        {
            if (p.tree.ranks[i] <= 0)
                continue;
            CharacterDatabase.Execute(
                "INSERT INTO `character_speedflying_talents` (`guid`, `talent_id`, `rank`) VALUES ({}, {}, {})",
                DbGuid(guid), static_cast<uint32>(kTalents[i].id), static_cast<uint32>(p.tree.ranks[i]));
        }
        p.dirty = false;
    }

    constexpr uint32 kGroundStackSlowdown = 3; // mounted-on-ground stack charge runs this many times slower

    struct PlayerState
    {
        bool   eligible  = false;         // client said the mount is AdvFly-capable
        bool   advFly    = false;         // last link message said AdvFly mount; survives landing, unlike eligible
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
        // The ceiling last announced to the client. Twin Vault (G9) raises it, and the stack count
        // alone does not move when it does: the client polls `S:` and reads field 5 as MaxStacks, so
        // without a resend the orb row stayed at three and the fourth stack had nowhere to appear.
        int    lastMaxStacks = -1;
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
        float  divePitch  = 0.0f;         // degrees below the horizon of the last dive tick (D4 reads it)
        bool   settling   = false;        // G10: the glide landed on the elevated rate, settling to Level now
        float  settleFrom = 0.0f;         // rate that settle started at
        uint32 settleAccum = 0;           // ms into that settle
    };

    std::mutex g_lock;
    std::unordered_map<uint64, PlayerState> g_states;
    std::unordered_map<uint64, Progress>    g_progress;

    // The per-player effective config, built once per update and never mutated in place. Returned by
    // value on purpose: a Progress lives in a map another player's message may be inserting into.
    Config EffectiveConfig(uint64 guid)
    {
        auto it = g_progress.find(guid);
        return it != g_progress.end() ? it->second.eff : g_cfg;
    }

    // Flushes every dirty progress map while g_lock is held: a per-tick caller would block the whole
    // link-tick path on the character database, so the tick only raises g_flushWanted and the write
    // rides the world thread's own tick, which does not hold the lock. Logout and shutdown keep the
    // synchronous call -- they need the rows on disk before the process or the session goes away.
    void FlushAllProgress(bool force = false)
    {
        for (auto& kv : g_progress)
        {
            Progress& p = kv.second;
            if (p.loaded && (p.dirty || force))
                WriteProgress(kv.first, p);
        }
    }

    // The same flush with no lock held. The world thread's tick calls this: it copies the dirty rows
    // under the lock, releases it, and only then talks to the database, so a flushed character DB
    // cannot stall another player's chat hook behind the mutex. Writing a snapshot cannot be overtaken:
    // this flush, every other write and every erase run on the world thread -- the login thread only
    // ever inserts into the map, which is why the write works from a copy.
    void FlushAllProgressUnlocked()
    {
        std::vector<std::pair<uint64, Progress>> rows;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            rows.reserve(g_progress.size());
            for (auto& kv : g_progress)
                if (kv.second.loaded && kv.second.dirty)
                {
                    // Clear the flag on the live entry: WriteProgress clears it on the copy it is
                    // handed, so a write that skipped this would leave the row dirty for every tick
                    // until it happened to land inside the lock again.
                    kv.second.dirty = false;
                    rows.emplace_back(kv.first, kv.second);
                }
        }
        // Writes run free of the lock; WriteProgress only needs its own copy.
        for (auto& r : rows)
            WriteProgress(r.first, r.second);
    }

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

    // Glide a boost's carried speed buys: a fraction (BoostGlidePct) of the ordinary glide, so a full
    // BoostAdd of carry no longer pays the whole GlideMs and outlasts the next stack recharge. The
    // carry is capped at one BoostAdd, so stacks banked over a long climb cannot buy an unbounded
    // glide when they are finally released. Always called with the player's effective config.
    uint32 BoostCarryMs(const Config& c, float carry)
    {
        if (carry > c.boostAdd)
            carry = c.boostAdd;
        return ScaledMs(static_cast<uint32>(static_cast<float>(c.glideMs) * c.boostGlidePct),
                        carry, c.boostAdd);
    }
    // Every transition helper below reads the *effective* config of the player it is moving, never
    // g_cfg: the base value stays the ManaSpeedFlying.* key and a talent is a percentage of it, so
    // an update loop that read g_cfg would quietly ignore every rank a player owns.
    float RateFor(const Config& c, Motion m)
    {
        switch (m)
        {
            case Motion::Climb: return c.climbSpeed;
            case Motion::Dive:  return c.diveSpeed;
            default:            return c.levelSpeed; // Level and Hover
        }
    }

    float MaxRate(const Config& c) { return std::max(c.climbSpeed, std::max(c.levelSpeed, c.diveSpeed)); }
    float MinRate(const Config& c) { return std::min(c.climbSpeed, std::min(c.levelSpeed, c.diveSpeed)); }

    // Transition length for entering `next` from the state we are in.
    uint32 DurationFor(const Config& c, const PlayerState& st, Motion next)
    {
        const float nextRate = RateFor(c, next);
        // A hover (a parked mount) is a deliberately tiny accel/decel zone in both directions: the
        // speed drops to the parked level as the mount stops and comes straight back out when it
        // moves, so parking cannot bank a residual dive glide to spend on the next take-off.
        if (st.motion == Motion::Hover || next == Motion::Hover)
            return c.hoverMs;
        if (next == Motion::Dive)
            return c.diveMs;
        if (st.motion == Motion::Dive)
        {
            const float dist = std::fabs(nextRate - st.current);
            if (next == Motion::Climb)
                return ScaledMs(c.climbDecelMs, dist, std::fabs(c.diveSpeed - c.climbSpeed));
            if (next == Motion::Level)
            {
                // D4 Shallow Pull: a pull-out from a shallow dive keeps the whole glide instead of
                // only the slice the speed being shed would pay for. The pitch is tracked from the
                // dive's own position deltas, since the server never sees the client's camera.
                if (st.divePitch <= c.shallowPullDeg)
                    return c.glideMs;
                return ScaledMs(c.glideMs, dist, std::fabs(c.diveSpeed - c.levelSpeed));
            }
        }
        // A climb entered from a dive and cut short to level still carries the leftover dive speed,
        // so it glides out like a pull-out rather than blending like a fresh climb.
        if (st.motion == Motion::Climb && st.fromDive && next == Motion::Level)
        {
            const float dist = std::fabs(nextRate - st.current);
            if (st.divePitch <= c.shallowPullDeg)
                return c.glideMs;
            return ScaledMs(c.glideMs, dist, std::fabs(c.diveSpeed - c.levelSpeed));
        }
        // Everything else keeps the old span-proportional blend.
        const float span = MaxRate(c) - MinRate(c);
        if (span <= 0.0f)
            return 0;
        const float dist = std::fabs(nextRate - st.current);
        return static_cast<uint32>(static_cast<float>(c.blendMs) * dist / span);
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
    // held stacks can be chained. D5 Aftershock is not a second activation: it folds the live burst
    // into the running transition (see CommitBoost), so releasing the key spends no further stack.
    void ActivateBoost(PlayerState& st, const Config& c)
    {
        if (!st.eligible || st.stacks <= 0)
            return;
        // Usable while climbing, diving or gliding level. Only a hover (a parked mount) is left out,
        // so a boost cannot be spent to bank speed before a take-off.
        if (st.motion != Motion::Dive && st.motion != Motion::Level && st.motion != Motion::Climb)
            return;

        // Dive Bomb: a boost pressed while the last link state is Dive adds more (boostAddDive multiplies it).
        const float addMul = st.motion == Motion::Dive ? c.boostAddDive : 1.0f;
        const float applied = st.current + st.boostOffset;
        const float boosted = std::min(c.boostSpeed, applied + c.boostAdd * addMul);
        if (boosted <= applied)
            return; // already at the ceiling

        st.stacks--;

        const float add = boosted - applied;
        float carry     = add * c.boostCarryPct;

        // A boost may raise the *base* glide only up to the fastest normal state (the dive ceiling);
        // speed past that stays in the fading burst offset instead. Without this, a boost spent at or
        // near the dive ceiling pushed the base above it, and a pull-out glide is scaled by the speed
        // being shed, so that one boost stretched the whole following glide -- the HUD's boost band
        // stayed lit for tens of seconds and the boost read as never emptying. Keeping the overrun in
        // the short burst means it fades on BoostFadeMs, not on the ordinary glide.
        const float room = std::max(0.0f, MaxRate(c) - st.current);
        if (carry > room)
            carry = room;

        // Replenish part of the boost as ordinary glide time. A climb is not a glide, so its carry
        // is banked: folding it into the running climb would hold speed past the climb. SetTarget
        // releases the banked carry when the climb ends -- on leveling out it rides its own glide, so
        // the retained half becomes glide time instead of emptying in a few seconds.
        if (carry > 0.0f && st.motion == Motion::Climb)
        {
            st.pendingCarry += carry;
        }
        // Otherwise raise the base transition and extend it only by what the carried speed itself
        // needs to shed -- a fraction of the ordinary glide -- instead of restarting the long
        // pull-out curve, so a boost lands back on the glide it had rather than outlasting the next
        // stack. The running transition keeps its own elapsed time and remaining span, so a boost
        // taken mid-glide does not reset the ease-in to the part that holds speed longest. Only fold
        // it when the base is decelerating (current at or above target); while a dive is still
        // accelerating, folding it would stall the climb to DiveSpeed, so the whole boost stays in
        // the burst offset instead.
        else if (carry > 0.0f && st.current >= st.target)
        {
            const uint32 left = st.elapsed < st.duration ? st.duration - st.elapsed : 0;
            st.start    = st.current + carry;
            st.current  = st.start;
            st.target   = RateFor(c, st.motion);
            st.duration = st.elapsed + left + BoostCarryMs(c, carry);
            st.curve    = Curve::EaseIn;
        }
        else
            carry = 0.0f;

        // Add to any boost still fading so chained stacks build one bigger burst instead of replacing.
        st.boostStart    = st.boostOffset + (add - carry);
        st.boostOffset   = st.boostStart;
        st.boostElapsed  = 0;
        st.boostDuration = c.boostFadeMs;
    }

    // D5 Aftershock: the boost key came up while the burst is still fading, so the burst is committed
    // to the running transition now instead of bleeding off over BoostFadeMs. No stack is spent -- the
    // key press that started the boost already paid for it -- so a diver whose stock is empty can
    // still land the burst it is riding. The transition is restarted from the committed rate because
    // folding into `current` alone is not durable: the update tick recomputes `current` from
    // start/target/elapsed, and would erase the commit on the very next frame.
    void CommitBoost(PlayerState& st, const Config& c)
    {
        if (st.boostOffset <= 0.0f)
            return; // the fade is already empty: nothing to commit, and restarting the curve would hurt
        st.current       = std::min(c.boostSpeed, st.current + st.boostOffset);
        st.start         = st.current;
        st.target        = RateFor(c, st.motion);
        st.elapsed       = 0;
        // A full dive accel is too long for a nudge that is already at the dive ceiling, so the
        // handoff keeps the usual span-proportional blend back down to the state rate.
        st.duration      = ScaledMs(c.diveMs, st.start - st.target, std::fabs(c.diveSpeed - c.climbSpeed));
        st.curve         = Curve::Dive;
        st.boostStart    = 0.0f;
        st.boostOffset   = 0.0f;
        st.boostElapsed  = 0;
        st.boostDuration = 0;
    }

    // Records the state a link message named. The first message on a mount snaps the applied rate so
    // a take-off does not spend seconds ramping from the mount's own speed.
    void SetTarget(PlayerState& st, const Config& c, Motion m)
    {
        st.sinceLink = 0;

        if (!st.eligible)
        {
            st.eligible  = true;
            // Reserve Charge: a new flight starts with extra boost stacks, never past the ceiling.
            st.stacks    = std::min(c.boostStacks, st.stacks + c.startStacks);
            st.motion    = m;
            st.current   = RateFor(c, m);
            st.start     = st.current;
            st.target    = st.current;
            st.elapsed   = 0;
            st.duration  = 0;
            st.curve     = Curve::Linear;
            st.fromDive  = false;
            st.settling  = false;
            st.settleAccum = 0;
            st.pendingCarry = 0.0f;
            st.lastSent  = -1.0f;
            st.sendAccum = c.sendMs; // force the first packet out on the next update
            return;
        }

        if (m == st.motion)
        {
            st.target = RateFor(c, m); // heartbeat: keep the running transition going
            return;
        }
        // A real state change: start a fresh transition from wherever the speed is now. It also
        // cancels a pending Skyward settle -- the state the player just chose outranks the elevated
        // level-out, and a settle left armed would pull the new transition back to levelSpeed.
        st.settling    = false;
        st.settleAccum = 0;
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
                // Merge the fading burst into the base so the transition starts from the live speed
                // (no upward step), but only up to the fastest normal state (the dive ceiling):
                // anything past that is speed no plain state reaches, so it stays a fading burst
                // instead of raising the base and stretching the glide this level-out runs.
                const float live = std::min(c.boostSpeed, st.current + st.boostOffset);
                const float base = std::min(live, std::max(MaxRate(c), st.current));
                st.current       = base;
                st.boostStart    = live - base;
                st.boostOffset   = live - base;
                st.boostElapsed  = 0;
                st.boostDuration = st.boostStart > 0.0f ? c.boostFadeMs : 0;
                foldedCarry      = std::min(st.pendingCarry, std::max(0.0f, c.boostSpeed - base));
            }
            else
            {
                // A pull-out that still carries dive speed keeps its higher current speed; everything
                // else keeps the banked half folded into the new transition.
                const float base = st.current;
                foldedCarry = std::min(st.pendingCarry,
                                       std::max(0.0f, std::max(MaxRate(c), base) - base));
                st.current = base + foldedCarry;
            }
            st.pendingCarry = 0.0f;
        }

        st.start    = st.current;
        st.target   = RateFor(c, m);
        // G10 Skyward Path: a level-out ends above the level bar on the elevated rate, and the update
        // path then waits out SettleMs before dropping the rest of the way. Gated on coming *down* from
        // a dive or a climb: leveling out of a hover must not lift the mount above the bar.
        if (m == Motion::Level && c.levelElev > 0.0f && st.target < c.levelElev &&
            (prev == Motion::Dive || prev == Motion::Climb))
        {
            st.target     = c.levelElev;
            st.settleFrom = st.target;
            st.settling   = true;
        }
        st.elapsed  = 0;
        st.duration = DurationFor(c, st, m);
        st.curve    = CurveFor(st, m);
        // A banked carry pays for its own glide: a full BoostAdd of carry buys BoostGlidePct of the
        // ordinary GlideMs, a smaller carry proportionally less, so the retained speed is not dumped
        // in three or four seconds. Only when the mount is actually shedding speed -- an eased rise
        // from below the level speed would just delay settling, not carry momentum. Keep whichever
        // glide is longer, so a pull-out that still carries dive speed is never shortened.
        if (m == Motion::Level && foldedCarry > 0.0f && c.boostAdd > 0.0f && st.start > st.target)
        {
            const uint32 carryDur = BoostCarryMs(c, foldedCarry);
            if (carryDur > st.duration)
            {
                st.duration = carryDur;
                st.curve    = Curve::EaseIn;
            }
        }
        st.motion   = m;
        // Only a state change arms or clears a settle; the block above set the flag it needs, and this
        // line must not wipe it. The old settle was already cleared on entry to the !eligible branch.
    }

    // Publish the configured speeds and the live stack count to the client so its bar, particles,
    // shake and boost button scale to the values actually in play and its stack count cannot drift from
    // the server's. Same "WXLMS" prefix the client already sends, body
    // "S:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,<stackMs>,
    // <stackDiveMs>,<stackAccumMs>,<stackIntervalMs>"; the client polls
    // its last addon message, ignores a body it cannot parse, and uses the last two fields to anchor
    // the orb's recharge bar to the server's exact position (an older server omits them). A UI reload
    // on the client drops the value, which the slow resend covers.
    void SendSpeedAnnounce(Player* player, const Config& c, int stacks, uint32 stackAccum, uint32 stackInterval)
    {
        char body[160];
        std::snprintf(body, sizeof(body), "S:%.4g,%.4g,%.4g,%u,%d,%u,%d,%u,%u,%u,%u",
                      c.levelSpeed * 100.0f, c.diveSpeed * 100.0f, c.boostSpeed * 100.0f, 0u,
                      c.boostStacks, c.boostFadeMs, stacks, c.stackMs, c.stackDiveMs,
                      stackAccum, stackInterval);

        // The client listens for the addon prefix on CHAT_MSG_ADDON, which the client's chat layer only
        // parses out of a LANG_ADDON chat packet -- so the prefix rides at the front, exactly as a
        // client-sent SendAddonMessage lays it out.
        WorldPacket data;
        std::string message = std::string("WXLMS\t") + body;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }

    // --- the talent tree --------------------------------------------------------------------------
    // Every one of these runs with g_lock held, so a rank write and the tally that answers it cannot
    // interleave with another message from the same client.

    // D5 Aftershock: the client's boost key was released while a boost is still fading. Only a diver
    // that owns the talent gets to fold the burst into a dive; for anyone else the fade runs its course.
    // The check is on the rank, not on the message, so a forged 'X' buys nothing.
    void HandleAftershock(uint64 guid)
    {
        auto pit = g_progress.find(guid);
        if (pit == g_progress.end() || !pit->second.loaded)
            return;
        const Talents& tree = pit->second.tree;
        bool owned = false;
        for (int i = 0; i < kTalentCount; ++i)
            for (int k = 0; k < kTalents[i].nEffects; ++k)
                if (tree.ranks[i] && kTalents[i].effects[k].effect == E_Aftershock)
                    owned = true;
        if (!owned)
            return; // no ranked talent carries the Aftershock flag

        auto it = g_states.find(guid);
        if (it == g_states.end())
            return;
        PlayerState& st = it->second;
        if (!st.eligible || !st.boostDuration || st.motion != Motion::Dive)
            return; // only a live boost released straight into a dive

        CommitBoost(st, pit->second.eff);
        st.sendAccum = pit->second.eff.sendMs; // push the folded speed out at once
    }

    // Sends one "WXLTT" body. Same routing as the speed announce: the prefix rides at the front of a
    // LANG_ADDON whisper, which is the only prefix the client's chat layer parses.
    void SendTalentLine(Player* player, const std::string& body)
    {
        WorldPacket data;
        std::string message = std::string("WXLTT\t") + body;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }

    // The progress bar on its own: the partial point as a permille of the point being paid for, then
    // the yards that point still needs. This is the body that used to be the tally's tail, split out
    // so a bar tick does not drag every rank pair along with it -- the tail is on a ~500 ms cadence
    // for as long as the mount is banking yards, and the pairs are the bulk of a tally.
    void SendTalentBar(Player* player, uint32 permille, double need)
    {
        char body[64];
        std::snprintf(body, sizeof(body), "P:%u,%.0f", permille, need);
        SendTalentLine(player, body);
    }

    // The authoritative tally: lifetime points, points spent and unspent, then one id:rank pair per
    // talent the character owns, then the partial point as a permille and the yards the next point
    // still needs. Both tail fields are appended *after* the pairs on purpose: a client that scans the
    // pairs breaks out on a field it cannot read, so it keeps every rank and only loses the bar.
    // Only a rank or a point count can change here -- the bar moves on its own and rides SendTalentBar.
    void SendTalentTally(Player* player, Progress& p)
    {
        // One body carries the whole tree: one character per talent in kTalents order, "0"-"9" then "a"-"z"
        // (a linked talent reaches 35). A split tally would not reach the client: its listener keeps only the
        // last T/F body it hears, so every chunk but the last would be lost. The client checks the count.
        std::string ranks;
        ranks.reserve(kTalentCount);
        for (int i = 0; i < kTalentCount; ++i)
        {
            const int r = p.tree.ranks[i];
            ranks.push_back(r < 10 ? static_cast<char>('0' + r) : static_cast<char>('a' + r - 10));
        }

        char tail[64];
        const double need = p.points < g_talentCfg.maxPoints ? PointYards(p.points, p.tree) : 0.0;
        p.shownPartial = PartialPermille(p.points, p.yardPartial, p.tree);
        std::snprintf(tail, sizeof(tail), ",P%u,N%.0f", p.shownPartial, need);

        const std::string body = "T:" + std::to_string(p.points) + "," + std::to_string(p.Spent()) + "," +
                                 std::to_string(p.Unspent()) + "," + std::to_string(kTalentCount) + "," +
                                 ranks + tail;

        // The bar has just been drawn from this tally, so record it: without this the next tick would
        // see a point count it has not shown and answer a tally that only repeats what we just sent.
        p.shownPoints = p.points;
        SendTalentLine(player, body);
    }

    // A refusal is answered, never dropped, so the panel cannot drift out of step with the server.
    void SendTalentRefusal(Player* player, const char* reason)
    {
        SendTalentLine(player, std::string("F:") + reason);
    }

    // Validates and applies one rank. Returns nullptr on success, or the refusal reason (a caller-owned
    // literal) so the caller decides whether to answer each write or only the batch. One `F` per request
    // is enough: the batch is retried as a whole, so it only needs the first stumble, not every one.
    const char* ApplyTalentRank(Progress& p, int talentId)
    {
        const int index = IndexOfTalentId(talentId);
        if (index < 0)
            return "unknown";

        const TalentDef& t = kTalents[index];
        if (const char* why = CanRank(p.tree, index, p.Unspent()))
            return why;

        p.tree.ranks[index]++;
        p.Rebuild(g_cfg, g_speedCapPct);
        p.dirty = true;
        LOG_INFO("modules", "mod-mana-speedflying: learnt talent {} (rank {}/{})", t.id,
                 p.tree.ranks[index], t.maxRank);
        return nullptr;
    }

    // Every write runs CanRank (refusal order: max rank, one cap, a parent with a rank, unspent points),
    // so a forged message costs exactly what a real one would.
    void HandleTalentLearn(Player* player, uint64 guid, int talentId)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        if (const char* why = ApplyTalentRank(p, talentId))
        {
            SendTalentRefusal(player, why);
            return;
        }
        SendTalentTally(player, p);
    }

    // Unlock only checks parents when a point is bought, so anything that lost its last full-rank parent must go
    // too, or it could be kept for free while the refunded points are spent elsewhere. Repeats until stable: each
    // drop can orphan the next ring, so removing one talent breaks the whole chain ahead of it. Returns the ranks
    // dropped (the points handed back).
    int DropOrphans(Talents& t)
    {
        int dropped = 0;
        for (bool changed = true; changed;)
        {
            changed = false;
            for (int i = 0; i < kTalentCount; ++i)
            {
                if (!t.ranks[i] || kTalents[i].kind == KindCore)
                    continue;
                if (!ParentFull(t.ranks, i))
                {
                    dropped += t.ranks[i];
                    t.ranks[i] = 0;
                    changed = true;
                }
            }
        }
        return dropped;
    }

    // Respec: refund one tree ("b1".."b3", or a bare "1".."3": dive, glide, wings) or everything ("A").
    // Free unless Talents.RespecCost is set. A tree refund clears that tree's ordinary and linked talents, then
    // anything left with no ranked parent (so a kept bridge or big talent cannot outlive its path). The argument is
    // parsed strictly -- a malformed
    // one is refused, never defaulted, because "refund everything" is the one destructive path.
    void HandleTalentRefund(Player* player, uint64 guid, const std::string& what)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        int branch = -1; // -1 = every branch
        if (what == "A")
        {
            branch = -1;
        }
        else
        {
            // "b<N>" is what this client sends; a bare "<N>" is accepted so an older build of it keeps
            // working. Anything else -- empty, zero, non-numeric, out of range -- is refused.
            const char* digits = (what.size() >= 2 && what[0] == 'b') ? what.c_str() + 1 : what.c_str();
            if (digits[0] < '1' || digits[0] > '3' || digits[1] != '\0')
            {
                SendTalentRefusal(player, "bad-branch");
                return;
            }
            branch = digits[0] - '0' - 1;
        }

        // Check affordability once, before anything is refunded, so a half-applied respec is not a
        // state this module can be in.
        if (g_talentCfg.respecCost && !player->HasEnoughMoney(g_talentCfg.respecCost))
        {
            SendTalentRefusal(player, "no-money");
            return;
        }

        int refunded = 0;
        for (int i = 0; i < kTalentCount; ++i)
        {
            // A tree respec clears that tree's ordinary and linked talents. Bridges, the core and the big
            // talents stay (a cap is cleared only by "A"); "A" clears everything.
            if (branch >= 0 && (kTalents[i].tree != branch || kTalents[i].kind == KindBridge ||
                                kTalents[i].kind == KindCore || kTalents[i].kind == KindCap))
                continue;
            if (!p.tree.ranks[i])
                continue;
            p.tree.ranks[i] = 0;
            ++refunded;
        }

        refunded += DropOrphans(p.tree);
        if (!refunded)
        {
            SendTalentTally(player, p); // nothing to refund; still answer, so the panel redraws
            return;
        }

        if (g_talentCfg.respecCost)
            player->ModifyMoney(-static_cast<int32>(g_talentCfg.respecCost));
        p.Rebuild(g_cfg, g_speedCapPct);
        p.dirty = true;
        SendTalentTally(player, p);
    }

    // Take one rank back from talent `talentId` and, with it, every talent ahead of it that needed it (DropOrphans).
    // Free unless Talents.RespecCost is set, like a respec.
    void HandleTalentUnspend(Player* player, uint64 guid, int talentId)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        const int index = IndexOfTalentId(talentId);
        if (index < 0)
        {
            SendTalentRefusal(player, "bad-id");
            return;
        }
        if (p.tree.ranks[index] <= 0)
        {
            SendTalentTally(player, p); // nothing there; answer so the panel redraws
            return;
        }
        if (g_talentCfg.respecCost && !player->HasEnoughMoney(g_talentCfg.respecCost))
        {
            SendTalentRefusal(player, "no-money");
            return;
        }

        --p.tree.ranks[index];
        DropOrphans(p.tree);

        if (g_talentCfg.respecCost)
            player->ModifyMoney(-static_cast<int32>(g_talentCfg.respecCost));
        p.Rebuild(g_cfg, g_speedCapPct);
        p.dirty = true;
        SendTalentTally(player, p);
    }

    // Learn several ranks per message: one body for a whole Confirm instead of one W per rank and a
    // round trip each. Entries are `id:target` pairs and `target` is the *absolute* rank wanted, not a
    // delta: a resent batch is then the same request, so a dropped reply cannot double-spend. Ranks are
    // applied one at a time in the listed order and every gate is re-checked each time, so a legal batch
    // stays legal and an illegal one stops at the first refusal. One tally answers the whole batch, so
    // the panel sees its tree change once rather than counting down.
    void HandleTalentLearnBatch(Player* player, uint64 guid, const std::string& list)
    {
        auto it = g_progress.find(guid);
        if (it == g_progress.end() || !it->second.loaded)
        {
            SendTalentRefusal(player, "no-progress");
            return;
        }
        Progress& p = it->second;

        // The parsed batch, kept whole: the apply below is order-independent, so the entries are collected
        // first rather than acted on as they are read.
        constexpr int kBatchMax = 64;
        int ids[kBatchMax] = {};
        int tgt[kBatchMax] = {};
        int count          = 0;

        const char* cur = list.c_str();
        while (*cur)
        {
            char* end = nullptr;
            const long id = std::strtol(cur, &end, 10);
            if (end == cur || id < std::numeric_limits<int>::min() || id > std::numeric_limits<int>::max())
            {
                SendTalentRefusal(player, "bad-arg");
                return;
            }
            cur = end;
            if (*cur != ':')
            {
                SendTalentRefusal(player, "bad-arg");
                return;
            }
            const long target = std::strtol(cur + 1, &end, 10);
            // A rank cap is the only ceiling a batch entry needs; per-talent maxRank is enforced by the
            // apply itself. A forged body cannot ask for a million ranks, but nothing here trusts it
            // anyway -- ApplyTalentRank re-derives every gate from the live ranks.
            if (end == cur + 1 || target < 0 || target > kTalentCount)
            {
                SendTalentRefusal(player, "bad-arg");
                return;
            }
            cur = end;

            const int index = IndexOfTalentId(static_cast<int>(id));
            if (index < 0)
            {
                SendTalentRefusal(player, "unknown");
                return;
            }
            if (count >= kBatchMax)
            {
                SendTalentRefusal(player, "bad-arg");
                return;
            }
            ids[count]   = static_cast<int>(id);
            tgt[count]   = static_cast<int>(target);
            ++count;
            if (*cur == ',') ++cur;
            else if (*cur)
            {
                SendTalentRefusal(player, "bad-arg");
                return;
            }
        }

        // Apply in passes, not left to right. A batch is a *set* of ranks, not a script: a draft that buys
        // a bridge and a node the bridge opens has a dependency the list order cannot always express --
        // the bridge must be ranked before the node it opens, which may sit earlier in the list. Applying
        // in the sent order refused an otherwise legal draft. Each pass ranks every
        // entry whose gates are already met; the passes repeat until one ranks nothing, and the first gate
        // still refusing after that is the reason the panel shows. A refusal aborts nothing mid-pass, so a
        // partly satisfied draft still lands what it legally can.
        const char* why = nullptr;
        for (bool progress = true; progress; )
        {
            progress = false;
            for (int e = 0; e < count; ++e)
            {
                const int ei = IndexOfTalentId(ids[e]);
                if (ei < 0 || p.tree.ranks[ei] >= tgt[e]) continue;
                if (const char* r = ApplyTalentRank(p, ids[e]))
                {
                    if (!why) why = r;
                    continue;
                }
                progress = true;
                why      = nullptr; // the pass moved, so an earlier stumble may yet resolve
            }
        }
        if (why)
        {
            SendTalentRefusal(player, why);
            return;
        }
        // Always answer, even on an empty body, so the panel redraws and never holds a stale count.
        SendTalentTally(player, p);
    }

    // Parses a "WXLTT" body. Everything arriving here is a *request*: the server decides. A refusal is
    // an answer, never a silent drop, so the client panel cannot drift out of step.
    //   WXLTT\tW\t<talentId>   WXLTT\tB\t<id>:<n>,...   WXLTT\tR\t<b1|b2|b3|A>   WXLTT\tQ
    void HandleTalentMessage(Player* player, const std::string& msg)
    {
        const size_t op = 6;
        if (msg.size() <= op)
            return;

        // The 'Q' tally request only reads and is answered on foot too, so the mounted gate below
        // covers W and R alone -- an unmounted player still gets the panel's refresh.
        const uint64 guid = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> guard(g_lock);

        if (!g_talentCfg.enabled)
        {
            SendTalentRefusal(player, "disabled");
            return;
        }

        // Only mounted flight earns and only a mount spends: a respec while riding a ground mount is
        // still fine, but an on-foot character cannot touch the tree.
        if (msg[op] != 'Q' && !player->IsMounted())
        {
            SendTalentRefusal(player, "not-mounted");
            return;
        }

        switch (msg[op])
        {
            case 'W':
            {
                // A truncated argument, a missing separator or a number too large for an int would
                // otherwise be parsed out of whatever follows; the channel is untrusted input.
                if (msg.size() <= op + 2 || msg[op + 1] != '\t')
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                const char* arg = msg.c_str() + op + 2;
                char*       end = nullptr;
                const long  id  = std::strtol(arg, &end, 10);
                if (end == arg || *end != '\0' || id < std::numeric_limits<int>::min() ||
                    id > std::numeric_limits<int>::max())
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentLearn(player, guid, static_cast<int>(id));
                break;
            }
            case 'U':
            {
                if (msg.size() <= op + 2 || msg[op + 1] != '\t')
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                const char* arg = msg.c_str() + op + 2;
                char*       end = nullptr;
                const long  id  = std::strtol(arg, &end, 10);
                if (end == arg || *end != '\0' || id < 0 || id > 65535)
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentUnspend(player, guid, static_cast<int>(id));
                break;
            }
            case 'B':
                if (msg.size() <= op + 2 || msg[op + 1] != '\t')
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentLearnBatch(player, guid, msg.substr(op + 2));
                break;
            case 'R':
                if (msg.size() <= op + 1)
                {
                    SendTalentRefusal(player, "bad-arg");
                    break;
                }
                HandleTalentRefund(player, guid, msg.substr(op + 2));
                break;
            case 'Q':
            {
                auto it = g_progress.find(guid);
                if (it == g_progress.end() || !it->second.loaded)
                    SendTalentRefusal(player, "no-progress");
                else
                    SendTalentTally(player, it->second);
                break;
            }
            default:
                break; // an unknown op is a newer client's; leave it for a build that knows it
        }
    }

class ManaSpeedFlyingPlayer : public PlayerScript
{
public:
    ManaSpeedFlyingPlayer()
        : PlayerScript("ManaSpeedFlyingPlayer",
                       { PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN,
                         PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE }) {}

    // The client link. wxl-mana-speedflying senders are the only ones that matter; everything else on the
    // addon channel is left as the core received it. The channel is untrusted: a '1' link is only
    // applied to a flying player and a boost off the ground is dropped, but the server cannot tell a
    // real client from a forged message -- it only ever sets a flight speed.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/, uint32& lang, std::string& msg) override
    {
        if (!player || lang != LANG_ADDON)
            return;

        // The talent tree rides its own prefix, so the link parser below is left exactly as it was.
        if (msg.size() >= 6 && msg.compare(0, 6, "WXLTT\t") == 0)
        {
            HandleTalentMessage(player, msg);
            return;
        }

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
                    it->second.advFly    = false;
                }
                return;
            }

            PlayerState& st = g_states[guid];
            st.advFly = true;

            const uint32 now = getMSTime();
            // A client sends at most one message per ~200 ms; anything faster is a flood. Boost
            // messages are never throttled (each spends a stack and must be answered), so the
            // client's stack count cannot desync from a dropped one.
            // The flood guard exempts 'B' because each boost spends a stack and must be answered.
            if (state != 'B' && state != 'X' && st.lastMsgMs && now - st.lastMsgMs < 50)
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
            // Everything below reads this player's own effective config, so a talent actually moves
            // the speed instead of only being validated.
            const Config eff = EffectiveConfig(guid);

            if (state == 'B')
            {
                st.sinceLink = 0;
                if (player->IsFlying())
                    ActivateBoost(st, eff);
                // Answer every attempt with the authoritative count, spent or rejected, so a click the
                // server ignored cannot leave the client button one stack off until the slow resend.
                st.lastAnnounce = now;
                announce        = true;
            }
            else if (state == 'X')
            {
                // D5 Aftershock: the boost key came up while a boost is still live. Handled inline
                // (not via SetTarget) because it must run even when the state letter is unchanged.
                st.sinceLink = 0;
                HandleAftershock(guid);
                st.lastAnnounce = now;
                announce        = true;
            }
            else
            {
                switch (state)
                {
                    case 'C': SetTarget(st, eff, Motion::Climb); break;
                    case 'D': SetTarget(st, eff, Motion::Dive);  break;
                    case 'H': SetTarget(st, eff, Motion::Hover); break;
                    default:  SetTarget(st, eff, Motion::Level); break;
                }
            }
            if (announce)
            {
                // This announce carries the live ceiling too, so record it: otherwise the update tick
                // would see lastMaxStacks stale and send a duplicate for the same change.
                st.lastMaxStacks = eff.boostStacks;
                SendSpeedAnnounce(player, eff, st.stacks, st.stackAccum,
                                  st.motion == Motion::Dive ? eff.stackDiveMs : eff.stackMs);
            }
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

        // The progress row is read once, on login, outside this lock (see OnPlayerLogin). A player whose
        // row is somehow missing is treated as a fresh character rather than queried here: a blocking
        // database read under g_lock would stall every other player's link tick on this thread.
        auto pit = g_progress.find(guid);
        if (pit == g_progress.end())
        {
            // A bare Progress default-constructs eff from the Config member initializers, not from the
            // INI, so a tick arriving before OnPlayerLogin would silently drive the mount at the
            // shipped defaults. Build the placeholder from the live base instead.
            Progress fresh;
            fresh.eff = g_cfg;
            ApplyTalents(fresh.eff, fresh.tree, g_speedCapPct);
            pit = g_progress.emplace(guid, std::move(fresh)).first;
        }
        Progress& progress = pit->second;

        // Everything the update tick applies comes from the player's own effective config.
        const Config eff = EffectiveConfig(guid);

        // Talent points are earned here too. A relocated first position is adopted rather than paid,
        // so a login or a teleport cannot mint a point.
        if (g_talentCfg.enabled)
        {
            const bool earning = flying && player->IsMounted() && player->GetMountID() &&
                                 g_states.count(guid) && g_states[guid].eligible;
            const float x = player->GetPositionX();
            const float y = player->GetPositionY();
            const float z = player->GetPositionZ();
            if (earning)
            {
                if (progress.havePos)
                {
                    const float dx = x - progress.lastX;
                    const float dy = y - progress.lastY;
                    const float dz = z - progress.lastZ;
                    const float yards = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (yards < g_talentCfg.teleportYards)
                    {
                        // Diving pays best: the same yard counts more while heading down.
                        const Motion m = g_states[guid].motion;
                        // A dive's own slope, for D4: the server never sees which way the client looks.
                        if (m == Motion::Dive && yards > 1.0f)
                            g_states[guid].divePitch = static_cast<float>(
                                std::atan2(-dz, std::sqrt(dx * dx + dy * dy)) * 57.2957795);
                        // Dive Yield and Level Yield scale the yards credited in their state (eff is 1 + sum).
                        float weight = 1.0f;
                        if (m == Motion::Dive)
                            weight = g_talentCfg.diveAccrual * progress.tree.eff[E_DiveYield];
                        else if (m == Motion::Level || m == Motion::Climb)
                            weight = progress.tree.eff[E_LevelYield];
                        progress.yardPartial += static_cast<double>(yards * weight);
                        if (progress.yardPartial > 0.0)
                            progress.dirty = true; // the fraction survives a logout too
                        // The award cascade: one tick can cross several (increasingly longer) steps of the
                        // curve after a teleport-free burst of speed, so the loop re-reads the yard cost
                        // of the point it is about to pay. That cost is also the meter the panel reads as
                        // pointIndex/maxPoints, so no second counter has to be kept in step with it.
                        while (progress.yardPartial >= 1.0)
                        {
                            if (progress.points >= g_talentCfg.maxPoints)
                            {
                                progress.yardPartial = 0.0; // the tree is full: stop banking yards
                                break;
                            }
                            if (g_talentCfg.maxPointsPerMinute &&
                                progress.recentMs.size() >= g_talentCfg.maxPointsPerMinute)
                                break; // the rolling cap: an AFK orbit is not the whole economy
                            const double need = PointYards(progress.points, progress.tree);
                            if (progress.yardPartial < need)
                                break;
                            progress.yardPartial -= need;
                            progress.points++;
                            progress.recentMs.push_back(0);
                            progress.dirty = true;
                            LOG_INFO("modules", "mod-mana-speedflying: {} earned talent point {}",
                                     guid, progress.points);
                        }
                    }
                }
            }
            progress.lastX = x;
            progress.lastY = y;
            progress.lastZ = z;
            progress.havePos = earning;
            // The rolling minute: age every sample that paid a point and drop the ones that have
            // just left the window, so an unbroken flight keeps earning MaxPointsPerMinute a minute.
            for (uint32& age : progress.recentMs)
                age += p_time;
            while (!progress.recentMs.empty() && progress.recentMs.front() >= 60000)
                progress.recentMs.erase(progress.recentMs.begin());
        }

        auto it = g_states.find(guid);
        if (it == g_states.end())
            return;
        PlayerState& st = it->second;

        // Advance the link clock even while grounded, so a link that dies as the mount lands still
        // times out instead of freezing eligible and resuming stale on the next take-off.
        if (st.eligible && eff.timeoutMs)
        {
            st.sinceLink += p_time;
            if (st.sinceLink > eff.timeoutMs)
            {
                st.eligible  = false; // link went quiet: the client is gone, stop driving the mount
                st.announced = false; // force the speeds onto the next link
            }
        }

        if (!eff.enabled || !flying || !st.eligible)
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
            // A linked mount still on the ground charges, GroundStackSlowdown times slower than flight.
            // The link is trusted only while fresh, same timeout as flight.
            const bool groundCharge = eff.enabled && !flying && st.advFly && player->IsMounted() &&
                                      eff.stackMs && st.stacks < eff.boostStacks &&
                                      (!eff.timeoutMs || getMSTime() - st.lastMsgMs <= eff.timeoutMs);
            st.stackInterval = eff.stackMs * kGroundStackSlowdown;
            if (groundCharge)
            {
                st.stackAccum += p_time;
                if (st.stackAccum >= eff.stackMs * kGroundStackSlowdown)
                {
                    st.stackAccum = 0;
                    ++st.stacks;
                    SendSpeedAnnounce(player, eff, st.stacks, st.stackAccum, st.stackInterval);
                }
            }
            else
                st.stackAccum = 0; // not charging: do not resume half a stack on take-off
            st.boostStart    = 0.0f;
            st.boostOffset   = 0.0f;
            st.boostElapsed  = 0;
            st.boostDuration = 0;
            st.pendingCarry  = 0.0f;
            st.settling      = false; // grounded: a pending Skyward settle must not resume on take-off
            st.settleAccum   = 0;
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

        // G10's second phase: once the glide has reached the elevated rate, the mount drops to the plain
        // level speed over a fixed settle. Gated on the transition above having finished, or the settle
        // would race the very glide it is supposed to follow.
        if (st.settling && st.elapsed >= st.duration)
        {
            st.settleAccum += p_time;
            if (st.settleAccum >= eff.settleMs)
            {
                st.current     = eff.levelSpeed;
                st.settling    = false;
                st.settleAccum = 0;
            }
            else
                st.current = st.settleFrom +
                             (eff.levelSpeed - st.settleFrom) *
                                 (static_cast<float>(st.settleAccum) / static_cast<float>(eff.settleMs));
            // The settle owns the applied rate while it runs: pin the target to it so the transition
            // block above cannot pull the speed back to the elevated rate on the next tick.
            st.target = st.current;
        }

        // Boost stacks build on a timer: one every StackIntervalMs, or every StackDiveIntervalMs
        // while diving, capped at the configured stock. The timer only runs while the mount is
        // link-eligible, so a grounded or unlinked mount earns nothing.
        const int    prevStacks       = st.stacks;
        const uint32 stackIntervalNow = st.motion == Motion::Dive ? eff.stackDiveMs : eff.stackMs;
        if (st.stacks < eff.boostStacks)
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

        const float applied = std::min(eff.boostSpeed, st.current + st.boostOffset);
        // Settled once the base transition is done and any boost has fully faded, so the exact final
        // value is always pushed. Testing `current == target` instead would fire every tick during a
        // boost fade (the base is settled but the offset is not), defeating the SendMs throttle.
        const bool settled = (applied == st.target && st.boostDuration == 0);

        st.sendAccum += p_time;
        if (st.sendAccum >= eff.sendMs || settled)
        {
            st.sendAccum = 0;
            // Quantize before the compare: the blend moves by hundredths and a spline broadcast per
            // hundredth is pure churn. The client ramps between what it is told, so the wire value is
            // authority only -- a coarser step is invisible on the bar and much cheaper to broadcast.
            const float quantized = std::round(applied * 100.0f) * 0.01f;
            if (quantized != st.lastSent)
            {
                player->SetSpeed(MOVE_FLIGHT, quantized, true);
                st.lastSent = quantized;
            }
        }

        // A stack change is pushed at once, so the client button never lags the count the next boost
        // will actually spend. The ceiling counts as a change in its own right: a talent that raises it
        // (Twin Vault) leaves `stacks` where it was, so a count-only test never announced the new
        // maximum and the client kept drawing three orbs. Only a point count or a rank pair needs the
        // whole tally; the bar's own movement rides the 'P' body below.
        if (st.stacks != prevStacks || eff.boostStacks != st.lastMaxStacks)
        {
            st.lastMaxStacks = eff.boostStacks;
            SendSpeedAnnounce(player, eff, st.stacks, st.stackAccum, st.stackInterval);
        }

        // Flush the earned points on a timer, not on every award: a point is a row write, not a
        // per-tick event, and a crash between flushes costs only the partial point in memory.
        if (g_talentCfg.flushMs && (progress.accMs += p_time) >= g_talentCfg.flushMs)
        {
            progress.accMs = 0;
            g_flushWanted = true; // the write rides the world tick, never this lock
        }

        // The talent bar's fill. The bar has no other way to see the partial point move, so it is
        // announced on a coarse floor when the permille changes -- but it is the bar alone, two fields,
        // not the tally's rank pairs. A tally is for a rank or a point, and both already send one.
        if ((progress.progMs += p_time) >= 500)
        {
            progress.progMs = 0;
            const uint32 partial = PartialPermille(progress.points, progress.yardPartial, progress.tree);
            if (progress.points != progress.shownPoints)
            {
                // A point was paid on this tick: the count, the unspent total and the next point's
                // cost all moved, so that one gets a tally. It is a rare event -- the caps allow a
                // handful a minute -- so the pairs cost nothing over the bar ticks between them.
                SendTalentTally(player, progress);
            }
            else if (partial != progress.shownPartial)
            {
                progress.shownPartial = partial;
                const double need = progress.points < g_talentCfg.maxPoints
                                        ? PointYards(progress.points, progress.tree)
                                        : 0.0;
                SendTalentBar(player, partial, need);
            }
        }
    }

    // Read the character's points and ranks once, on the session's own thread. The two queries block,
    // which is why this is not done in OnPlayerUpdate: that runs under g_lock and a read there would
    // stall every other player's link tick behind it.
    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;
        const uint64 guid = player->GetGUID().GetRawValue();
        // Copy the published config before releasing the lock: LoadProgress builds an effective config
        // from it and does its blocking queries outside the lock, on this session's own thread.
        Config base;
        float capPct = 1600.0f;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            base   = g_cfg;
            capPct = g_speedCapPct;
        }
        Progress p;
        LoadProgress(guid, base, capPct, p);

        std::lock_guard<std::mutex> guard(g_lock);
        // A reconnect can win the race against a slow logout; an entry already there keeps its ranks.
        // An entry the update path minted as a placeholder carries no row, so it is replaced -- only
        // marking it loaded would let the next write-back overwrite the stored tree with zeros.
        auto it = g_progress.find(guid);
        if (it == g_progress.end())
            g_progress.emplace(guid, std::move(p));
        else if (!it->second.loaded)
            it->second = std::move(p);
    }

    // The link state and the progress map both go on logout: the state is volatile, the progress is
    // flushed first so a relog finds exactly the ranks and the points it earned.
    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(g_lock);
        const uint64 guid = player->GetGUID().GetRawValue();
        auto pit = g_progress.find(guid);
        if (pit != g_progress.end())
        {
            if (pit->second.dirty)
                WriteProgress(guid, pit->second);
            g_progress.erase(pit);
        }
        g_states.erase(guid);
    }

    // Character deletion takes the talent rows with it, so a recycled guid cannot inherit a tree.
    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        const uint64 raw = guid.GetRawValue();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_progress.erase(raw);
            g_states.erase(raw); // a recycled guid must not inherit the deleted one's live link state
        }
        CharacterDatabase.Execute("DELETE FROM `character_speedflying_talents` WHERE `guid` = {}",
                                   DbGuid(raw));
        CharacterDatabase.Execute("DELETE FROM `character_speedflying` WHERE `guid` = {}", DbGuid(raw));
    }
};

class ManaSpeedFlyingWorld : public WorldScript
{
public:
    ManaSpeedFlyingWorld()
        : WorldScript("ManaSpeedFlyingWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) {}

    // The timer flush, drained here rather than in OnPlayerUpdate: this tick holds no lock, so a slow
    // character database stalls only this write, not every other player's link tick behind g_lock.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (g_flushWanted.exchange(false))
            FlushAllProgressUnlocked();
    }

    // The shutdown flush the review asked for: without it a graceful restart loses every rank bought
    // and every yard flown since the last flush. Runs while the maps are still alive, hence the hook.
    void OnShutdown() override
    {
        std::lock_guard<std::mutex> guard(g_lock);
        FlushAllProgress(true);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Config cfg;
        cfg.enabled      = sConfigMgr->GetOption<bool>("ManaSpeedFlying.Enable", true);
        cfg.climbSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.ClimbSpeed", 2.2f);
        cfg.levelSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.LevelSpeed", 3.2f);
        cfg.diveSpeed    = sConfigMgr->GetOption<float>("ManaSpeedFlying.DiveSpeed", 6.0f);
        cfg.blendMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BlendMs", 5500);
        cfg.diveMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.DiveMs", 4000);
        cfg.glideMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.GlideMs", 18000);
        cfg.climbDecelMs = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.ClimbDecelMs", 3500);
        cfg.hoverMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.HoverMs", 500);
        cfg.sendMs       = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.SendMs", 100);
        cfg.timeoutMs    = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.TimeoutMs", 2500);
        cfg.boostAdd     = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostAdd", 4.0f);
        cfg.boostSpeed   = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostSpeed", 12.0f);
        cfg.boostFadeMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.BoostFadeMs", 4000);
        cfg.boostCarryPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostCarryPct", 0.50f);
        cfg.boostGlidePct = sConfigMgr->GetOption<float>("ManaSpeedFlying.BoostGlidePct", 0.70f);
        cfg.boostStacks  = sConfigMgr->GetOption<int>("ManaSpeedFlying.BoostMaxStacks", 3);
        cfg.stackMs      = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackIntervalMs", 9000);
        cfg.stackDiveMs  = sConfigMgr->GetOption<uint32>("ManaSpeedFlying.StackDiveIntervalMs", 3000);

        // The four talent keys. Talents.Enable gates earning and spending; the rest default so the
        // module runs with no talent key present at all.
        TalentConfig tc;
        tc.enabled        = sConfigMgr->GetOption<bool>("Talents.Enable", true);
        // The earning curve: the first point costs Talents.BaseYards, each one after it costs
        // Talents.YardStep more than the point before (1.00 = 100%, compounding), easing to Talents.YardStepLate over Talents.YardSoothe points.
        tc.baseYards      = sConfigMgr->GetOption<float>("Talents.BaseYards", 100.0f);
        tc.yardStep       = sConfigMgr->GetOption<float>("Talents.YardStep", 1.00f);
        tc.yardStepLate   = sConfigMgr->GetOption<float>("Talents.YardStepLate", 0.10f);
        tc.yardSoothe     = sConfigMgr->GetOption<uint32>("Talents.YardSoothe", 25);
        // The circular tree has 52 nodes and a 140-point lifetime cap.
        tc.maxPoints      = sConfigMgr->GetOption<uint32>("Talents.MaxPoints", 103);
        tc.maxPointsPerMinute = sConfigMgr->GetOption<uint32>("Talents.MaxPointsPerMinute", 4);
        tc.respecCost     = sConfigMgr->GetOption<uint32>("Talents.RespecCost", 0);
        tc.flushMs        = sConfigMgr->GetOption<uint32>("Talents.FlushMs", 20000);
        tc.teleportYards  = sConfigMgr->GetOption<float>("Talents.TeleportYards", 200.0f);
        tc.diveAccrual    = sConfigMgr->GetOption<float>("Talents.DiveAccrual", 1.5f);

        float speedCapPct = sConfigMgr->GetOption<float>("ManaSpeedFlying.SpeedCapPct", 1600.0f);
        speedCapPct = std::max(10.0f, speedCapPct);

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
        cfg.boostGlidePct = std::max(0.0f, std::min(cfg.boostGlidePct, 1.0f));
        if (!cfg.boostFadeMs) cfg.boostFadeMs = 1;
        if (!cfg.stackMs) cfg.stackMs = 1;
        if (!cfg.stackDiveMs) cfg.stackDiveMs = 1;
        if (!cfg.sendMs) cfg.sendMs = 1;
        if (tc.baseYards < 1.0f) tc.baseYards = 1.0f;
        // A negative or huge step would make the curve non-monotonic or overflow the double.
        tc.yardStep = std::max(0.0f, std::min(tc.yardStep, 10.0f));
        tc.yardStepLate = std::max(0.0f, std::min(tc.yardStepLate, 10.0f));
        if (!tc.maxPoints) tc.maxPoints = 140;
        tc.maxPoints = std::min(tc.maxPoints, 255u); // 140 is the shipped default; this only guards a typo
        if (tc.teleportYards < 50.0f) tc.teleportYards = 50.0f;
        tc.diveAccrual = std::max(0.0f, tc.diveAccrual);
        // A cost above int32's range would flip the sign of the ModifyMoney argument below.
        if (tc.respecCost > static_cast<uint32>(std::numeric_limits<int32>::max()))
            tc.respecCost = static_cast<uint32>(std::numeric_limits<int32>::max());

        // Publish the new config and drop every link under the lock readers take. The entries are kept
        // (only marked ineligible) so the update path can restore each mount's own speed: clearing the
        // map would leave the last blended rate applied with nothing left to repair it.
        std::lock_guard<std::mutex> guard(g_lock);
        g_cfg = cfg;
        g_talentCfg = tc;
        g_speedCapPct = speedCapPct;

        // Every live player's effective config is rebuilt from the new base, so a reload reaches
        // ranks that were bought against the old values without a relog.
        for (auto& kv : g_progress)
        {
            kv.second.eff = cfg;
            ApplyTalents(kv.second.eff, kv.second.tree, speedCapPct);
        }

        for (auto& kv : g_states)
        {
            kv.second.eligible  = false;
            kv.second.announced = false;
        }
    }
};
} // namespace

void AddSC_ManaSpeedFlying()
{
    new ManaSpeedFlyingPlayer();
    new ManaSpeedFlyingWorld();
}
