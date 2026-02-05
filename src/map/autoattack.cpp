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

/**
 * Helper function to handle repetitive skill checks
 * Returns true if the skill was successfully cast
 **/
static bool aa_can_use(struct map_session_data* sd, int target_id, int skill_id, int delay, const char* var_name, int mob_count_req = 0, int current_mobs = 0) 
{
    // 1. Check if the toggle is ON in the NPC script
    if (get_aa_var(sd, var_name) <= 0) return false;

    // 2. Check if it's an AoE skill and if enough mobs are present
    if (mob_count_req > 0 && current_mobs < mob_count_req) return false;

    // 3. Check if the player actually has the skill learned
    int skill_lv = pc_checkskill(sd, skill_id);
    if (skill_lv <= 0) return false;

    // 4. Check if the player has enough SP
    if (sd->battle_status.sp < skill_get_sp(skill_id, skill_lv)) return false;

    // 5. Cast the skill
    unit_skilluse_id(&sd->bl, target_id, skill_id, skill_lv);
    sd->canskill_tick = gettick() + delay;
    return true;
}

static void autoattack_use_offensive_skill(struct map_session_data* sd, int mob_count)
{
    if (!sd) return;

    t_tick now = gettick();
    if (sd->ud.skilltimer > 0 || sd->ud.canact_tick > now || sd->canskill_tick > now)
        return;

    int target_id = sd->ud.target;
    int weapon = sd->status.weapon;
    bool has_shield = (sd->status.shield > 0);

    switch ((enum e_job)sd->status.class_) {
        
        case JOB_KNIGHT: case JOB_LORD_KNIGHT: case JOB_CRUSADER: case JOB_PALADIN: case JOB_SWORDMAN:
        {
            bool has_spear = (weapon == W_1HSPEAR || weapon == W_2HSPEAR);
            if (has_spear && aa_can_use(sd, target_id, 397, 1200, "AA_USE_SKILL_SPIRAL_PIERCE")) return;
            if (aa_can_use(sd, target_id, 398, 800, "AA_USE_SKILL_HEAD_CRUSH")) return;
            if (aa_can_use(sd, target_id, 399, 800, "AA_USE_SKILL_JOINT_BEAT")) return;
            if (aa_can_use(sd, target_id, 367, 800, "AA_USE_SKILL_PRESSURE")) return;
            if (has_shield && aa_can_use(sd, target_id, 480, 800, "AA_USE_SKILL_SHIELD_CHAIN")) return;
            if (aa_can_use(sd, target_id, 254, 1500, "AA_USE_SKILL_GRAND_CROSS", 2, mob_count)) return;
            if (has_shield && aa_can_use(sd, target_id, 250, 800, "AA_USE_SKILL_SHIELD_CHARGE")) return;
            if (aa_can_use(sd, target_id, 62, 1000, "AA_USE_SKILL_BOWLING_BASH", 2, mob_count)) return;
            if (has_spear && aa_can_use(sd, target_id, 56, 800, "AA_USE_SKILL_PIERCE")) return;
            if (aa_can_use(sd, target_id, 5, 800, "AA_USE_SKILL_BASH")) return;
            break;
        }

        case JOB_ARCHER: case JOB_HUNTER: case JOB_SNIPER: case JOB_BARD: case JOB_CLOWN: case JOB_DANCER: case JOB_GYPSY:
        {
            bool has_falcon = (sd->sc.option & OPTION_FALCON);
            if ((weapon == W_MUSICAL || weapon == W_WHIP) && aa_can_use(sd, target_id, 394, 1500, "AA_USE_SKILL_ARROW_VULCAN")) return;
            if (has_falcon && aa_can_use(sd, target_id, 381, 1000, "AA_USE_SKILL_FALCON_ASSAULT")) return;
            if (weapon == W_BOW && aa_can_use(sd, target_id, 382, 1200, "AA_USE_SKILL_SHARP_SHOOTING", 2, mob_count)) return;
            if (weapon == W_BOW && aa_can_use(sd, target_id, 46, 600, "AA_USE_SKILL_DOUBLE_STRAFE")) return;
            break;
        }

        case JOB_MAGE: case JOB_WIZARD: case JOB_HIGH_WIZARD: case JOB_SAGE: case JOB_PROFESSOR:
        {
            // AoE Big Three
            if (aa_can_use(sd, target_id, 89, 1500, "AA_USE_SKILL_STORM_GUST", 2, mob_count)) return;
            if (aa_can_use(sd, target_id, 83, 1500, "AA_USE_SKILL_METEOR_STORM", 2, mob_count)) return;
            if (aa_can_use(sd, target_id, 85, 1500, "AA_USE_SKILL_VERMILION", 2, mob_count)) return;
            
            // Standard Bolts
            if (aa_can_use(sd, target_id, 19, 1000, "AA_USE_SKILL_FIRE_BOLT")) return;
            if (aa_can_use(sd, target_id, 14, 1000, "AA_USE_SKILL_COLD_BOLT")) return;
            if (aa_can_use(sd, target_id, 20, 1000, "AA_USE_SKILL_LIGHTNING_BOLT")) return;
            break;
        }

        case JOB_ASSASSIN: case JOB_ASSASSIN_CROSS: case JOB_ROGUE: case JOB_STALKER:
        {
            if (aa_can_use(sd, target_id, 406, 1000, "AA_USE_SKILL_METEOR_ASSAULT", 2, mob_count)) return;
            if (aa_can_use(sd, target_id, 379, 1200, "AA_USE_SKILL_SOUL_BREAKER")) return;
            if (weapon == W_KATAR && aa_can_use(sd, target_id, 136, 2000, "AA_USE_SKILL_SONIC_BLOW")) return;
            break;
        }

        case JOB_SUPER_NOVICE:
        {
            if (aa_can_use(sd, target_id, 19, 1000, "AA_USE_SKILL_FIRE_BOLT")) return;
            if (aa_can_use(sd, target_id, 14, 1000, "AA_USE_SKILL_COLD_BOLT")) return;
            if (aa_can_use(sd, target_id, 21, 1500, "AA_USE_SKILL_THUNDER_STORM", 2, mob_count)) return;
            if (sd->status.zeny >= 1000 && aa_can_use(sd, target_id, 42, 800, "AA_USE_SKILL_MAMMONITE")) return;
            if (aa_can_use(sd, target_id, 5, 800, "AA_USE_SKILL_BASH")) return;
            break;
        }

        case JOB_NINJA:
        {
            if (aa_can_use(sd, target_id, 536, 1500, "AA_USE_SKILL_DRAGON_FIRE", 2, mob_count)) return;
            if (aa_can_use(sd, target_id, 534, 1000, "AA_USE_SKILL_FIRE_BLOSSOM")) return;
            if (weapon == W_HUUMA && aa_can_use(sd, target_id, 525, 1200, "AA_USE_SKILL_THROW_HUUMA")) return;
            if (aa_can_use(sd, target_id, 524, 800, "AA_USE_SKILL_THROW_KUNAI")) return;
            break;
        }

        case JOB_GUNSLINGER:
        {
            if (weapon == W_REVOLVER && aa_can_use(sd, target_id, 515, 800, "AA_USE_SKILL_RAPID_SHOWER")) return;
            if (weapon == W_SHOTGUN && aa_can_use(sd, target_id, 519, 2000, "AA_USE_SKILL_FULL_BUSTER")) return;
            if (weapon == W_SHOTGUN && aa_can_use(sd, target_id, 520, 1200, "AA_USE_SKILL_SPREAD_ATTACK", 2, mob_count)) return;
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