#include "autoattack.hpp"
#include <vector>
#include <unordered_map>
#include <stdarg.h> 
#include <cmath>    

// FIXED INCLUDES: Using the correct .hpp extension
#include "clif.hpp"
#include "unit.hpp"
#include "mob.hpp"
#include "itemdb.hpp"

#include "../common/mmo.hpp" // May contain PC_PERM_* and some job/status constants
#include "pc.hpp"     // For Job IDs (if needed) and PC_PERM_WARP_ANYWHERE
#include "skill.hpp"  // For Skill IDs (WZ_ENERGYCOAT, etc.)
#include "status.hpp" // For Status Change IDs (SC_AGIUP, SC_ADRENALINERUSH, etc.)
#include "map.hpp"    // For general map/game constants
#include "script.hpp"
#include "../common/strlib.hpp"

#define AA_DEBUG 1


// =====================================================================================
// === PRIVATE DATA (Static to this file) ===
// =====================================================================================

// Helper to read character variables from the script engine
static int get_aa_var(struct map_session_data* sd, const char* var_name)
{
    if (!sd || !var_name)
        return 0;

    // add_str converts the variable name string into a unique ID (integer)
    // pc_readglobalreg reads the value from the player's permanent variable storage
    return pc_readglobalreg(sd, add_str(var_name));
}

static int buildin_autoattack_count_mobs(struct block_list *bl, va_list ap)
{
    int *count = va_arg(ap, int *);
    (*count)++;
    return 0;
}

static int autoattack_count_nearby_mobs(struct map_session_data* sd, int radius)
{
    int count = 0;
    map_foreachinarea(
        buildin_autoattack_count_mobs,
        sd->bl.m,
        sd->bl.x - radius, sd->bl.y - radius,
        sd->bl.x + radius, sd->bl.y + radius,
        BL_MOB,
        &count
    );
    return count;
}

static void aa_debug(struct map_session_data* sd, const char* fmt, ...)
{
#if AA_DEBUG
    if (!sd || sd->fd <= 0) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    clif_displaymessage(sd->fd, buf);
#endif
}

static void autoattack_perform_teleport(struct map_session_data* sd) {
    if (!sd) return;

    // Read the NPC choice: 0 = Free (@jump), 1 = Fly Wing
    int tp_use_flywing = get_aa_var(sd, "AA_TP_USE_FLY_WING");
    int tp_use_jump = get_aa_var(sd, "AA_TP_USE_JUMP");

    if (tp_use_flywing == 1) {
        // Try to use a Fly Wing (Item ID: 601)
        int idx = pc_search_inventory(sd, 601);
        if (idx >= 0) {
            pc_useitem(sd, idx);
            return;
        }
    }

    // Default / Type 0: Free Teleport
    if (tp_use_jump == 1) {
        pc_randomwarp(sd, CLR_TELEPORT);
    }
}

// Global storage for per-player configurations
static std::unordered_map<int, autoattack_config> autoattack_configs;

// Default list of ASPD consumables to check/use
static const autoattack_item_entry AUTOATTACK_ITEMS[] = {
    { 657, AUTOITEM_CHECK_ASPD_GROUP, SC_NONE }, // Berserk Potion (Top Tier)
    { 656, AUTOITEM_CHECK_ASPD_GROUP, SC_NONE }, // Awakening Potion
    { 645, AUTOITEM_CHECK_ASPD_GROUP, SC_NONE }, // Concentration Potion (Lowest Tier)
};

// Default list of HP/SP consumables for auto-potting
static const autoattack_pot_entry AUTOATTACK_POTS[] = {
    { 504, 50, 90, false }, // White Potion (HP)
    { 505, 50, 90, true  }, // Blue Potion (SP)
    // Add other potions here in descending order of preference if needed
};

static const int AUTOATTACK_BB_MAX_MOBS = 5;

// =====================================================================================
// === PRIVATE FUNCTION PROTOTYPES (Static to this file) ===
// =====================================================================================

static const std::vector<t_itemid> get_aspd_pot_list(struct map_session_data* sd);
static const std::vector<autoattack_pot_entry> get_heal_pot_configs(struct map_session_data* sd);
static int buildin_autoattack_sub(struct block_list *bl,va_list ap);
static int buildin_autoattack_count_attackers(struct block_list *bl, va_list ap);
static int autoattack_count_attackers(struct map_session_data* sd, int radius);
static bool autoattack_has_aspd_potion(struct map_session_data* sd);
// autoattack_use_item_timer is REMOVED/NO LONGER NEEDED
static void autoattack_try_consumables(struct map_session_data* sd);
static void autoattack_try_autopots(struct map_session_data* sd);
static void autoattack_rebuff(struct map_session_data* sd);
static void autoattack_use_offensive_skill(struct map_session_data* sd, int mob_count);
static bool autoattack_motion(struct map_session_data* sd);


// =====================================================================================
// === CONFIGURATION ACCESSORS ===
// =====================================================================================

autoattack_config& get_autoattack_config(struct map_session_data* sd) {
    auto& config = autoattack_configs[sd->bl.id];
    if (config.max_mobs_before_tp == 0) {
        config.max_mobs_before_tp = AUTOATTACK_MAX_MOBS_DEFAULT;
        config.idle_cycles_before_tp = AUTOATTACK_MISS_CYCLES_DEFAULT;
        config.use_aspd_pots = true;
        config.use_heal_pots = true;
    }
    return config;
}

static const std::vector<t_itemid> get_aspd_pot_list(struct map_session_data* sd) {
    auto& config = get_autoattack_config(sd);
    if (!config.aspd_pot_ids.empty())
        return config.aspd_pot_ids;
    
    std::vector<t_itemid> defaults;
    for (const auto& entry : AUTOATTACK_ITEMS) {
        if (entry.check_type == AUTOITEM_CHECK_ASPD_GROUP && entry.item_id > 0)
            defaults.push_back(entry.item_id);
    }
    return defaults;
}

static const std::vector<autoattack_pot_entry> get_heal_pot_configs(struct map_session_data* sd) {
    auto& config = get_autoattack_config(sd);
    if (!config.heal_pot_configs.empty())
        return config.heal_pot_configs;
    
    std::vector<autoattack_pot_entry> defaults;
    for (const auto& pot : AUTOATTACK_POTS) {
        if (pot.item_id > 0)
            defaults.push_back(pot);
    }
    return defaults;
}

// =====================================================================================
// === TARGETING AND UTILITY FUNCTIONS ===
// =====================================================================================

static int buildin_autoattack_sub(struct block_list *bl,va_list ap)
{
    int *target_id=va_arg(ap,int *);
    *target_id = bl->id;
    return 1; // Stop iteration (found first mob)
}

static int buildin_autoattack_count_attackers(struct block_list *bl, va_list ap)
{
    struct map_session_data *sd = va_arg(ap, struct map_session_data *);
    int *count = va_arg(ap, int *);
    struct mob_data *md = (struct mob_data *)bl;
    if (md && md->target_id == sd->bl.id)
        (*count)++;
    return 0; // Continue iteration
}

static int autoattack_count_attackers(struct map_session_data* sd, int radius)
{
    int count = 0;
    map_foreachinarea(buildin_autoattack_count_attackers, sd->bl.m, sd->bl.x - radius, sd->bl.y - radius, sd->bl.x + radius, sd->bl.y + radius, BL_MOB, sd, &count);
    return count;
}

static bool autoattack_has_aspd_potion(struct map_session_data* sd)
{
    return sd->sc.data[SC_ASPDPOTION0] || sd->sc.data[SC_ASPDPOTION1] || sd->sc.data[SC_ASPDPOTION2] || sd->sc.data[SC_ASPDPOTION3];
}

// =====================================================================================
// === CONSUMABLE LOGIC (FIXED: NO DELAY) ===
// =====================================================================================

/**
 * @brief Attempts to use ASPD-boosting consumables if not currently buffed.
 */
static void autoattack_try_consumables(struct map_session_data* sd)
{
    if (!sd) return;

    // --- 1. ASPD Potions ---
    int aspd_id = get_aa_var(sd, "AA_ASPD_ITEM");
    if (aspd_id > 0 && !autoattack_has_aspd_potion(sd)) {
        int idx = pc_search_inventory(sd, aspd_id);
        if (idx >= 0) pc_useitem(sd, idx);
    }

    // --- 2. Battle Manual ---
    if (get_aa_var(sd, "AA_USE_BATTLE_MANUAL") > 0 && !sd->sc.data[SC_EXPBOOST]) {
        int idx = pc_search_inventory(sd, 12263); 
        if (idx >= 0) pc_useitem(sd, idx);
    }

    // --- 3. Bubble Gum ---
    if (get_aa_var(sd, "AA_USE_BUBBLE_GUM") > 0 && !sd->sc.data[SC_ITEMBOOST]) {
        int idx = pc_search_inventory(sd, 12210);
        if (idx >= 0) pc_useitem(sd, idx);
    }

    // --- 4. LV10 Agil Scroll ---
    if (get_aa_var(sd, "AA_USE_AGI_SCROLL") > 0 && !sd->sc.data[SC_ITEMBOOST]) {
        int idx = pc_search_inventory(sd, 12216);
        if (idx >= 0) pc_useitem(sd, idx);
    }

    // --- 5. LV10 Blessing Scroll ---
    if (get_aa_var(sd, "AA_USE_BLESS_SCROLL") > 0 && !sd->sc.data[SC_ITEMBOOST]) {
        int idx = pc_search_inventory(sd, 12215);
        if (idx >= 0) pc_useitem(sd, idx);
    }
}

/**
 * @brief Attempts to use HP/SP healing potions based on NPC thresholds.
 */
static void autoattack_try_autopots(struct map_session_data* sd)
{
    if (!sd) return;
    
    // Global item cooldown check
    if (gettick() < sd->canskill_tick)
        return;

    auto pct = [](uint32 cur, uint32 max){
        return max == 0 ? 100 : (int)((cur * 100ULL) / max);
    };

    int cur_hp = pct(sd->battle_status.hp, sd->battle_status.max_hp);
    int cur_sp = pct(sd->battle_status.sp, sd->battle_status.max_sp);

    // --- HP LOGIC ---
    int hp_id  = get_aa_var(sd, "AA_HP_ITEM");
    int hp_threshold = get_aa_var(sd, "AA_HP_THRESHOLD");

    static std::unordered_map<int, bool> is_healing_hp;

    if (hp_id > 0 && hp_threshold > 0) {
        if (cur_hp <= hp_threshold) is_healing_hp[sd->bl.id] = true;
        else if (cur_hp > hp_threshold) is_healing_hp[sd->bl.id] = false;

        if (is_healing_hp[sd->bl.id]) {
            int idx = pc_search_inventory(sd, hp_id);
            if (idx >= 0) pc_useitem(sd, idx);
            else is_healing_hp[sd->bl.id] = false; // Out of pots
        }
    }

    // --- SP LOGIC ---
    int sp_id  = get_aa_var(sd, "AA_SP_ITEM");
    int sp_threshold = get_aa_var(sd, "AA_SP_THRESHOLD");

    static std::unordered_map<int, bool> is_healing_sp;

    if (sp_id > 0 && sp_threshold > 0) {
        if (cur_sp <= sp_threshold) is_healing_sp[sd->bl.id] = true;
        else if (cur_sp > sp_threshold) is_healing_sp[sd->bl.id] = false;

        if (is_healing_sp[sd->bl.id]) {
            int idx = pc_search_inventory(sd, sp_id);
            if (idx >= 0) pc_useitem(sd, idx);
            else is_healing_sp[sd->bl.id] = false; // Out of pots
        }
    }
}

// =====================================================================================
// === AUTO-BUFF (REBUFF) LOGIC (Complete Pre-Renewal) ===
// =====================================================================================

static void autoattack_rebuff(struct map_session_data* sd)
{
    if (!sd) return;
    const t_tick now = gettick();

    // FAILSAFES: Don't interrupt existing actions
    if (sd->ud.skilltimer > 0 || sd->ud.attacktimer > 0 || 
        sd->ud.canact_tick > now || sd->ud.canmove_tick > now)
        return;

    int skill_lv = 0;

    // Explicitly returning e_skill to keep the compiler happy
    auto get_needed_buff = [&]() -> e_skill {
        
        if ((skill_lv = pc_checkskill(sd, AL_BLESSING)) > 0 && !sd->sc.data[SC_BLESSING]) return (e_skill)AL_BLESSING;
        if ((skill_lv = pc_checkskill(sd, AL_INCAGI)) > 0 && !sd->sc.data[SC_INCREASEAGI]) return (e_skill)AL_INCAGI;

        switch ((enum e_job)sd->status.class_) {
            case JOB_LORD_KNIGHT:
                if ((skill_lv = pc_checkskill(sd, LK_AURABLADE)) > 0 && !sd->sc.data[SC_AURABLADE]) return (e_skill)LK_AURABLADE;
                if ((skill_lv = pc_checkskill(sd, LK_CONCENTRATION)) > 0 && !sd->sc.data[SC_CONCENTRATION]) return (e_skill)LK_CONCENTRATION;
                // fallthrough
            case JOB_KNIGHT:
                if ((skill_lv = pc_checkskill(sd, KN_TWOHANDQUICKEN)) > 0 && !sd->sc.data[SC_TWOHANDQUICKEN]) {
                    if (sd->status.weapon == W_2HSWORD || sd->status.weapon == W_2HSPEAR) return (e_skill)KN_TWOHANDQUICKEN;
                }
                break;

            case JOB_ASSASSIN_CROSS:
                // This call now respects the skill_db requirements (Item: Poison Bottle + SP cost)
                if ((skill_lv = pc_checkskill(sd, ASC_EDP)) > 0 && !sd->sc.data[SC_EDP]) {
                    int bottle_idx = pc_search_inventory(sd, 678);
                    if (bottle_idx >= 0) {
                        return (e_skill)ASC_EDP;
                    }
                }
                break;

            case JOB_SNIPER:
                if ((skill_lv = pc_checkskill(sd, SN_SIGHT)) > 0 && !sd->sc.data[SC_TRUESIGHT]) return (e_skill)SN_SIGHT;
                if ((skill_lv = pc_checkskill(sd, SN_WINDWALK)) > 0 && !sd->sc.data[SC_WINDWALK]) return (e_skill)SN_WINDWALK;
                break;
            
            case JOB_WHITESMITH:
                if ((skill_lv = pc_checkskill(sd, BS_ADRENALINE)) > 0 && !sd->sc.data[SC_ADRENALINE]) return (e_skill)BS_ADRENALINE;
                if ((skill_lv = pc_checkskill(sd, BS_WEAPONPERFECT)) > 0 && !sd->sc.data[SC_WEAPONPERFECTION]) return (e_skill)BS_WEAPONPERFECT;
                break;

            default: break;
        }
        return (e_skill)0; 
    };

    e_skill skill_id = get_needed_buff();
    
    // Check if a skill was actually selected
    if (skill_id != (e_skill)0) {
        // MATCHING YOUR SIGNATURE: src, target_id, skill_id, skill_lv
        unit_skilluse_id(&sd->bl, sd->bl.id, (uint16)skill_id, (uint16)skill_lv);
    }
}

// =====================================================================================
// === AUTO-OFFENSIVE SKILL LOGIC ===
// =====================================================================================

static void autoattack_use_offensive_skill(struct map_session_data* sd, int mob_count)
{
    if (!sd) return;

    if (get_aa_var(sd, "AA_USE_SKILLS") == 0)
        return;

    t_tick now = gettick();

    if (sd->ud.skilltimer > 0 || sd->ud.canact_tick > now || sd->canskill_tick > now)
        return;

    int skill_lv = 0;

    switch ((enum e_job)sd->status.class_) {
        case JOB_KNIGHT:
        case JOB_LORD_KNIGHT:
        {
            skill_lv = pc_checkskill(sd, KN_BOWLINGBASH);

            if (mob_count >= 3 && mob_count <= AUTOATTACK_BB_MAX_MOBS)
            {
                if (skill_lv > 0)
                {
                    sd->canskill_tick = 0;
                    
                    // Try the 4-argument version: (src, x, y, skill_id)
                    unit_skilluse_pos(&sd->bl, sd->bl.x, sd->bl.y, KN_BOWLINGBASH, skill_lv);

                    unit_stop_attack(&sd->bl); 
                    clif_displaymessage(sd->fd, "Auto-Skill: Bowling Bash!");
                    return; 
                }
            }
            break;
        }
        default: break;
    }
}

// =====================================================================================
// === CORE AUTOATTACK LOOP FUNCTIONS ===
// =====================================================================================

static bool autoattack_motion(struct map_session_data* sd)
{
    int i, target_id;
    
    // 1. Search for a target mob
    for(i=0;i<=AUTOATTACK_RADIUS;i++)
    {
        target_id=0;
        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x-i, sd->bl.y-i, sd->bl.x+i, sd->bl.y+i, BL_MOB, &target_id);
        
        if(target_id)
        {
            // Found a target: initiate attack motion
            unit_attack(&sd->bl,target_id,1);
            return true;
        }
    }
    
    // 2. No target found: initiate random short movement
    if(!target_id)
    {
        int dx = (rand()%2==0?-1:1)*(rand()%10);
        int dy = (rand()%2==0?-1:1)*(rand()%10);
        unit_walktoxy(&sd->bl, sd->bl.x + dx, sd->bl.y + dy, 0);
    }
    
    return false;
}

/**
 * @brief The main timer function.
 */
int autoattack_timer(int tid, t_tick tick, int id, intptr_t data)
{
    struct map_session_data *sd = map_id2sd(id);
    static std::unordered_map<int, int> autoattack_notarget_ticks;
    
    if (sd == NULL) return 0;
    
    if (pc_isdead(sd)) { 
        sd->sc.option &= ~OPTION_AUTOATTACK;
        autoattack_notarget_ticks.erase(id);
        clif_changeoption(&sd->bl);
        return 0;
    }

    if (sd->sc.option & OPTION_AUTOATTACK)
    {
        // ---------------------------------------------------------
        // [UPDATED] RESTING / SITTING LOGIC (Single Value Mode)
        // ---------------------------------------------------------
        // ---------------------------------------------------------
        int sit_hp_trigger = get_aa_var(sd, "AA_REST_HP");
        int sit_sp_trigger = get_aa_var(sd, "AA_REST_SP");

        if (sit_hp_trigger > 0 || sit_sp_trigger > 0) {
            auto pct = [](uint32 cur, uint32 max){
                return max == 0 ? 100 : (int)((cur * 100ULL) / max);
            };

            int cur_hp = pct(sd->battle_status.hp, sd->battle_status.max_hp);
            int cur_sp = pct(sd->battle_status.sp, sd->battle_status.max_sp);

            // Logic: If standing, check if we need to SIT
            if (!pc_issit(sd)) { 
                bool low_hp = (sit_hp_trigger > 0 && cur_hp < sit_hp_trigger);
                bool low_sp = (sit_sp_trigger > 0 && cur_sp < sit_sp_trigger);

                if (low_hp || low_sp) {
                    unit_stop_attack(&sd->bl); 
                    pc_setsit(sd, 1);
                    clif_sitting(&sd->bl);
                    clif_status_change(&sd->bl, 12, 1, 0, 0, 0, 0);
                    add_timer(gettick() + 1000, autoattack_timer, sd->bl.id, 0);
                    return 0; 
                }
            } 
            // Logic: If sitting, check if we can STAND
            else { 
                // Standing logic: trigger + 5% buffer to prevent sit/stand loops
                bool healthy_hp = (sit_hp_trigger == 0 || cur_hp >= (sit_hp_trigger + 5));
                bool healthy_sp = (sit_sp_trigger == 0 || cur_sp >= (sit_sp_trigger + 5));

                if (healthy_hp && healthy_sp) {
                    pc_setsit(sd, 0);
                    clif_standing(&sd->bl);
                    clif_status_change(&sd->bl, 12, 0, 0, 0, 0, 0);
                } else {
                    add_timer(gettick() + 1000, autoattack_timer, sd->bl.id, 0);
                    return 0;
                }
            }
        }
        // ---------------------------------------------------------

        // 1. Support & Potion Variable Checking
        autoattack_rebuff(sd);
        autoattack_try_consumables(sd);
        autoattack_try_autopots(sd);

        int mob_count = autoattack_count_nearby_mobs(sd, AUTOATTACK_RADIUS);

        int tp_threshold = get_aa_var(sd, "AA_TP_MOBCOUNT");

        if (tp_threshold > 0 && mob_count >= tp_threshold) {
            autoattack_perform_teleport(sd);
            clif_displaymessage(sd->fd, "Auto-Defense: Teleporting (Mob density too high).");
            autoattack_notarget_ticks.erase(sd->bl.id);
            add_timer(gettick() + 500, autoattack_timer, sd->bl.id, 0);
            return 0;
        }
        
        // Priority 2: Offensive Skills (Bowling Bash)
        autoattack_use_offensive_skill(sd, mob_count);

        // Priority 3: Normal Attack/Motion
        bool found = autoattack_motion(sd); 
        
        if (found) {
            autoattack_notarget_ticks.erase(sd->bl.id);
        } else {
            int &miss = autoattack_notarget_ticks[sd->bl.id];
            auto& config = get_autoattack_config(sd);
            if (++miss >= config.idle_cycles_before_tp) {
                if (!(map_getmapflag(sd->bl.m, MF_NOTELEPORT)))
                    autoattack_perform_teleport(sd);
                miss = 0;
            }
        }
        
        add_timer(gettick() + 500, autoattack_timer, sd->bl.id, 0);
    } else {
        autoattack_notarget_ticks.erase(id);
    }
    
    return 0;
}