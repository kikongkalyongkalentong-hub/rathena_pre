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

    t_tick now = gettick();

    // Standard animation/cast timers
    if (sd->ud.skilltimer > 0 || sd->ud.canact_tick > now || sd->canskill_tick > now)
        return;

    int skill_lv = 0;

    int weapon_type = sd->status.weapon;
    int shield_id = sd->status.shield;

    switch ((enum e_job)sd->status.class_) {
        case JOB_KNIGHT:
        case JOB_LORD_KNIGHT:
        case JOB_CRUSADER:
        case JOB_PALADIN:
        case JOB_SWORDMAN:
        {
            bool has_spear = (weapon_type == W_1HSPEAR || weapon_type == W_2HSPEAR);
            bool has_shield = (shield_id > 0);

            // --- Pressure (ID: 367) ---
            if (get_aa_var(sd, "AA_USE_SKILL_PRESSURE") > 0) {
                skill_lv = pc_checkskill(sd, 367);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(367, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 367, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Shield Chain (ID: 480) [Shield Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SHIELD_CHAIN") > 0 && has_shield) {
                skill_lv = pc_checkskill(sd, 480);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(480, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 480, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Grand Cross (ID: 254) [Multi-Mob Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_GRAND_CROSS") > 0) {
                skill_lv = pc_checkskill(sd, 254);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(254, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 254, skill_lv);
                        sd->canskill_tick = now + 1500; return; 
                    }
                }
            }

            // --- Shield Charge (ID: 250) [Shield Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SHIELD_CHARGE") > 0 && has_shield) {
                skill_lv = pc_checkskill(sd, 250);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(250, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 250, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Shield Boomerang (ID: 251) [Shield Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SHIELD_BOOMERANG") > 0 && has_shield) {
                skill_lv = pc_checkskill(sd, 251);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(251, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 251, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Holy Cross (ID: 253) ---
            if (get_aa_var(sd, "AA_USE_SKILL_HOLY_CROSS") > 0) {
                skill_lv = pc_checkskill(sd, 253);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(253, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 253, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Bowling Bash (ID: 62) ---
            if (get_aa_var(sd, "AA_USE_SKILL_BOWLING_BASH") > 0) {
                skill_lv = pc_checkskill(sd, 62);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(62, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 62, skill_lv);
                        sd->canskill_tick = now + 1000; return; 
                    }
                }
            }

            // --- Brandish Spear (ID: 57) [Spear Only] ---
            if (get_aa_var(sd, "AA_USE_SKILL_BRANDISH_SPEAR") > 0 && has_spear) {
                skill_lv = pc_checkskill(sd, 57);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(57, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 57, skill_lv);
                        sd->canskill_tick = now + 1200; return; 
                    }
                }
            }

            // --- Spear Stab (ID: 58) [Spear Only] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SPEAR_STAB") > 0 && has_spear) {
                skill_lv = pc_checkskill(sd, 58);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(58, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 58, skill_lv);
                        sd->canskill_tick = now + 1000; return; 
                    }
                }
            }

            // --- Magnum Break (ID: 7) ---
            if (get_aa_var(sd, "AA_USE_SKILL_MAGNUM_BREAK") > 0) {
                skill_lv = pc_checkskill(sd, 7);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(7, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 7, skill_lv);
                        sd->canskill_tick = now + 1000; return; 
                    }
                }
            }

            // --- Pierce (ID: 56) [Spear Only] ---
            if (get_aa_var(sd, "AA_USE_SKILL_PIERCE") > 0 && has_spear) {
                skill_lv = pc_checkskill(sd, 56);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(56, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 56, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Spear Boomerang (ID: 59) [Spear Only] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SPEAR_BOOMERANG") > 0 && has_spear) {
                skill_lv = pc_checkskill(sd, 59);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(59, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 59, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Bash (ID: 5) ---
            if (get_aa_var(sd, "AA_USE_SKILL_BASH") > 0) {
                skill_lv = pc_checkskill(sd, 5);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(5, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 5, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }
            break;
        }
        
        case JOB_ARCHER:
        case JOB_HUNTER:
        case JOB_SNIPER:
        case JOB_BARD:
        case JOB_CLOWN:
        case JOB_DANCER:
        case JOB_GYPSY:
        {
            bool has_bow = (weapon_type == W_BOW);
            bool has_falcon = (sd->sc.option & OPTION_FALCON);
            bool has_instrument = (weapon_type == W_MUSICAL);
            bool has_whip = (weapon_type == W_WHIP);

            // --- Arrow Vulcan (ID: 394) [Instrument or Whip Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_ARROW_VULCAN") > 0 && (has_instrument || has_whip)) {
                skill_lv = pc_checkskill(sd, 394);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(394, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 394, skill_lv);
                        sd->canskill_tick = now + 1500; return; // Long animation delay
                    }
                }
            }

            // --- Musical Strike (ID: 316) [Instrument Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_MUSICAL_STRIKE") > 0 && has_instrument) {
                skill_lv = pc_checkskill(sd, 316);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(316, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 316, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Throw Arrow (ID: 324) [Whip Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_THROW_ARROW") > 0 && has_whip) {
                skill_lv = pc_checkskill(sd, 324);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(324, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 324, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Sharp Shooting (ID: 382) [Multi-Mob Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_SHARP_SHOOTING") > 0 && has_bow) {
                skill_lv = pc_checkskill(sd, 382);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(382, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 382, skill_lv);
                        sd->canskill_tick = now + 1200; return; // Longer delay for animation
                    }
                }
            }

            // --- Falcon Assault (ID: 381) [Falcon Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_FALCON_ASSAULT") > 0 && has_falcon) {
                skill_lv = pc_checkskill(sd, 381);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(381, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 381, skill_lv);
                        sd->canskill_tick = now + 1000; return; 
                    }
                }
            }

            // --- Blitz Beat (ID: 129) [Falcon Req] ---
            if (get_aa_var(sd, "AA_USE_SKILL_BLITZ_BEAT") > 0 && has_falcon) {
                skill_lv = pc_checkskill(sd, 129);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(129, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 129, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Arrow Shower (ID: 47) [Multi-Mob Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_ARROW_SHOWER") > 0 && has_bow) {
                skill_lv = pc_checkskill(sd, 47);
                if (skill_lv > 0 && mob_count >= 2) {
                    int target_id = sd->ud.target;
                    // Area logic: check for target or nearby mobs
                    if (!target_id) {
                        map_foreachinarea(buildin_autoattack_sub, sd->bl.m, sd->bl.x - 2, sd->bl.y - 2, sd->bl.x + 2, sd->bl.y + 2, BL_MOB, &target_id);
                    }
                    if (target_id && sd->battle_status.sp >= skill_get_sp(47, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 47, skill_lv);
                        sd->canskill_tick = now + 800; return; 
                    }
                }
            }

            // --- Double Strafe (ID: 46) ---
            if (get_aa_var(sd, "AA_USE_SKILL_DOUBLE_STRAFE") > 0 && has_bow) {
                skill_lv = pc_checkskill(sd, 46);
                if (skill_lv > 0) {
                    int target_id = sd->ud.target;
                    if (target_id && sd->battle_status.sp >= skill_get_sp(46, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 46, skill_lv);
                        sd->canskill_tick = now + 600; return; 
                    }
                }
            }
            break;
        }
        
        case JOB_MERCHANT:
        case JOB_BLACKSMITH:
        case JOB_WHITESMITH:
        case JOB_ALCHEMIST:
        case JOB_CREATOR:
        {
            // --- Acid Demonstration (ID: 490) [Creator Only] ---
            // Requires: 1 Acid Bottle AND 1 Bottle Grenade
            if (get_aa_var(sd, "AA_USE_SKILL_ACID_DEMO") > 0) {
                skill_lv = pc_checkskill(sd, 490);
                if (skill_lv > 0) {
                    // Check for reagents in inventory
                    if (pc_search_inventory(sd, 7136) > 0 && pc_search_inventory(sd, 7135) > 0) {
                        int target_id = sd->ud.target;
                        if (target_id && sd->battle_status.sp >= skill_get_sp(490, skill_lv)) {
                            unit_skilluse_id(&sd->bl, target_id, 490, skill_lv);
                            sd->canskill_tick = now + 1200; return; 
                        }
                    }
                }
            }

            // --- Acid Terror (ID: 230) ---
            // Requires: 1 Acid Bottle
            if (get_aa_var(sd, "AA_USE_SKILL_ACID_TERROR") > 0) {
                skill_lv = pc_checkskill(sd, 230);
                if (skill_lv > 0) {
                    if (pc_search_inventory(sd, 7136) > 0) {
                        int target_id = sd->ud.target;
                        if (target_id && sd->battle_status.sp >= skill_get_sp(230, skill_lv)) {
                            unit_skilluse_id(&sd->bl, target_id, 230, skill_lv);
                            sd->canskill_tick = now + 1000; return; 
                        }
                    }
                }
            }

            // --- Mammonite (ID: 42) ---
            if (get_aa_var(sd, "AA_USE_SKILL_MAMMONITE") > 0) {
                skill_lv = pc_checkskill(sd, 42);
                // Check skill level, SP, and Zeny (Mammonite costs 100z * Level)
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(42, skill_lv) && sd->status.zeny >= (100 * skill_lv)) {
                    int target_id = sd->ud.target;
                    if (target_id) {
                        unit_skilluse_id(&sd->bl, target_id, 42, skill_lv);
                        sd->canskill_tick = now + 600; return; 
                    }
                }
            }
            break;
        }

        case JOB_MAGE:
        case JOB_WIZARD:
        case JOB_HIGH_WIZARD:
        case JOB_SAGE:
        case JOB_PROFESSOR:
        {
            int target_id = sd->ud.target;

            // --- The "Big Three" AoE (IDs: 89, 83, 85) ---
            int aoe_skills[] = { 89, 83, 85 }; // Storm Gust, Meteor Storm, LoV
            const char* aoe_vars[] = { "AA_USE_SKILL_STORM_GUST", "AA_USE_SKILL_METEOR_STORM", "AA_USE_SKILL_VERMILION" };
            
            if (mob_count >= 2) {
                for (int i = 0; i < 3; i++) {
                    if (get_aa_var(sd, aoe_vars[i]) > 0) {
                        skill_lv = pc_checkskill(sd, aoe_skills[i]);
                        if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(aoe_skills[i], skill_lv)) {
                            unit_skilluse_id(&sd->bl, target_id, aoe_skills[i], skill_lv);
                            sd->canskill_tick = now + 1500; return; 
                        }
                    }
                }
            }

            // --- Heaven's Drive (ID: 91) [AoE Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_HEAVEN_DRIVE") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 91);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(91, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 91, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // --- Jupitel Thunder (ID: 84) ---
            if (get_aa_var(sd, "AA_USE_SKILL_JUPITEL_THUNDER") > 0) {
                skill_lv = pc_checkskill(sd, 84);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(84, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 84, skill_lv);
                    sd->canskill_tick = now + 800; return; 
                }
            }

            // --- Spike (ID: 90) ---
            if (get_aa_var(sd, "AA_USE_SKILL_EARTH_SPIKE") > 0) {
                skill_lv = pc_checkskill(sd, 90);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(90, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 90, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // --- Water Ball (ID: 86) [Water Cell Check] ---
            if (get_aa_var(sd, "AA_USE_SKILL_WATER_BALL") > 0) {
                // Check if player is standing on water
                if (map_getcell(sd->bl.m, sd->bl.x, sd->bl.y, CELL_CHKWATER)) {
                    skill_lv = pc_checkskill(sd, 86);
                    if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(86, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 86, skill_lv);
                        sd->canskill_tick = now + 1200; return;
                    }
                }
            }

            // --- Thunder Storm (ID: 21) [AoE Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_THUNDER_STORM") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 21);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(21, skill_lv)) {
                    if (target_id) {
                        unit_skilluse_id(&sd->bl, target_id, 21, skill_lv);
                        sd->canskill_tick = now + 1200; return;
                    }
                }
            }

            // --- Fire Ball (ID: 17) [AoE Pattern] ---
            if (get_aa_var(sd, "AA_USE_SKILL_FIRE_BALL") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 17);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(17, skill_lv)) {
                    if (target_id) {
                        unit_skilluse_id(&sd->bl, target_id, 17, skill_lv);
                        sd->canskill_tick = now + 800; return;
                    }
                }
            }

            // --- Bolts & Single Target (Fire, Cold, Lightning, Soul, Napalm) ---
            // We check them in order of priority. 
            // Note: If multiple are ON, the first one found in the code will be cast.
            
            int mage_skills[] = { 19, 14, 20, 13, 11 }; // Fire Bolt, Cold Bolt, Lightning Bolt, Soul Strike, Napalm Beat
            const char* mage_vars[] = { "AA_USE_SKILL_FIRE_BOLT", "AA_USE_SKILL_COLD_BOLT", "AA_USE_SKILL_LIGHTNING_BOLT", "AA_USE_SKILL_SOUL_STRIKE", "AA_USE_SKILL_NAPALM_BEAT" };

            for (int i = 0; i < 5; i++) {
                if (get_aa_var(sd, mage_vars[i]) > 0) {
                    skill_lv = pc_checkskill(sd, mage_skills[i]);
                    if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(mage_skills[i], skill_lv)) {
                        if (target_id) {
                            unit_skilluse_id(&sd->bl, target_id, mage_skills[i], skill_lv);
                            sd->canskill_tick = now + 1000; return;
                        }
                    }
                }
            }
            break;
        }

        case JOB_PRIEST:
        case JOB_HIGH_PRIEST:
        case JOB_MONK:
        case JOB_CHAMPION:
        {
            int target_id = sd->ud.target;
            if (!target_id) break;

            // Magnus Exorcismus (Requires Blue Gemstone check if your server enforces it)
            if (get_aa_var(sd, "AA_USE_SKILL_MAGNUS") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 79);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(79, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 79, skill_lv);
                    sd->canskill_tick = now + 1500; return;
                }
            }

            // Turn Undead (Only if target is Undead element)
            if (get_aa_var(sd, "AA_USE_SKILL_TURN_UNDEAD") > 0) {
                struct block_list* target_bl = map_id2bl(target_id);
                // Check if target exists and if its element is Undead (6)
                if (target_bl && status_get_element(target_bl) == ELE_UNDEAD) {
                    skill_lv = pc_checkskill(sd, 77);
                    if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(77, skill_lv)) {
                        unit_skilluse_id(&sd->bl, target_id, 77, skill_lv);
                        sd->canskill_tick = now + 1000; return;
                    }
                }
            }
            break;
        }

        case JOB_ASSASSIN:
        case JOB_ASSASSIN_CROSS:
        case JOB_ROGUE:
        case JOB_STALKER:
        {
            int target_id = sd->ud.target;
            if (!target_id) break;

            // Double Strafe (Requires Bow)
            if (get_aa_var(sd, "AA_USE_SKILL_DOUBLE_STRAFE") > 0) {
                skill_lv = pc_checkskill(sd, 46);
                if (skill_lv > 0 && sd->status.weapon == W_BOW && sd->battle_status.sp >= skill_get_sp(46, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 46, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Meteor Assault (AoE - Logic: Use if 2 or more mobs are nearby)
            if (get_aa_var(sd, "AA_USE_SKILL_METEOR_ASSAULT") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 406);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(406, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 406, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Soul Breaker (Mid-Range)
            if (get_aa_var(sd, "AA_USE_SKILL_SOUL_BREAKER") > 0) {
                skill_lv = pc_checkskill(sd, 379);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(379, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 379, skill_lv);
                    sd->canskill_tick = now + 1200; return;
                }
            }

            // Sonic Blow (Requires Katar)
            if (get_aa_var(sd, "AA_USE_SKILL_SONIC_BLOW") > 0) {
                skill_lv = pc_checkskill(sd, 136);
                // Check if wearing a Katar before attempting
                if (skill_lv > 0 && sd->status.weapon == W_KATAR && sd->battle_status.sp >= skill_get_sp(136, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 136, skill_lv);
                    sd->canskill_tick = now + 2000; return; 
                }
            }
            break;
        }

        case JOB_SUPER_NOVICE:
        {
            int target_id = sd->ud.target;
            if (!target_id) break;

            // Fire Bolt (ID: 19)
            if (get_aa_var(sd, "AA_USE_SKILL_FIRE_BOLT") > 0) {
                skill_lv = pc_checkskill(sd, 19);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(19, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 19, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Cold Bolt (ID: 14)
            if (get_aa_var(sd, "AA_USE_SKILL_COLD_BOLT") > 0) {
                skill_lv = pc_checkskill(sd, 14);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(14, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 14, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Lightning Bolt (ID: 20)
            if (get_aa_var(sd, "AA_USE_SKILL_LIGHTNING_BOLT") > 0) {
                skill_lv = pc_checkskill(sd, 20);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(20, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 20, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Soul Strike (ID: 13)
            if (get_aa_var(sd, "AA_USE_SKILL_SOUL_STRIKE") > 0) {
                skill_lv = pc_checkskill(sd, 13);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(13, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 13, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Thunder Storm (ID: 21) - AoE (2+ mobs)
            if (get_aa_var(sd, "AA_USE_SKILL_THUNDER_STORM") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 21);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(21, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 21, skill_lv);
                    sd->canskill_tick = now + 1500; return;
                }
            }

            // Fire Ball (ID: 17) - AoE (2+ mobs)
            if (get_aa_var(sd, "AA_USE_SKILL_FIRE_BALL") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 17);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(17, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 17, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Napalm Beat (ID: 11) - AoE (2+ mobs)
            if (get_aa_var(sd, "AA_USE_SKILL_NAPALM_BEAT") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 11);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(11, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 11, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Magnum Break (ID: 7) - AoE (2+ mobs)
            if (get_aa_var(sd, "AA_USE_SKILL_MAGNUM_BREAK") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 7);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(7, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 7, skill_lv);
                    sd->canskill_tick = now + 1200; return;
                }
            }

            // Mammonite (ID: 42) - Requires 1000 Zeny
            if (get_aa_var(sd, "AA_USE_SKILL_MAMMONITE") > 0 && sd->status.zeny >= 1000) {
                skill_lv = pc_checkskill(sd, 42);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(42, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 42, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Bash (ID: 5)
            if (get_aa_var(sd, "AA_USE_SKILL_BASH") > 0) {
                skill_lv = pc_checkskill(sd, 5);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(5, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 5, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }
            break;
        }

        case JOB_NINJA:
        {
            int target_id = sd->ud.target;
            if (!target_id) break;

            // Dragon Fire Formation (ID: 536) - AoE
            if (get_aa_var(sd, "AA_USE_SKILL_DRAGON_FIRE") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 536);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(536, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 536, skill_lv);
                    sd->canskill_tick = now + 1500; return;
                }
            }

            // Crimson Fire Blossom (ID: 534)
            if (get_aa_var(sd, "AA_USE_SKILL_FIRE_BLOSSOM") > 0) {
                skill_lv = pc_checkskill(sd, 534);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(534, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 534, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Lightning Spear of Ice (ID: 537)
            if (get_aa_var(sd, "AA_USE_SKILL_SPEAR_ICE") > 0) {
                skill_lv = pc_checkskill(sd, 537);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(537, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 537, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Wind Blade (ID: 540)
            if (get_aa_var(sd, "AA_USE_SKILL_WIND_BLADE") > 0) {
                skill_lv = pc_checkskill(sd, 540);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(540, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 540, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // North Wind (ID: 542) - AoE
            if (get_aa_var(sd, "AA_USE_SKILL_NORTH_WIND") > 0 && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 542);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(542, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 542, skill_lv);
                    sd->canskill_tick = now + 1200; return;
                }
            }

            // Throw Huuma Shuriken (ID: 525) - Requires Huuma Shuriken weapon
            if (get_aa_var(sd, "AA_USE_SKILL_THROW_HUUMA") > 0 && sd->status.weapon == W_HUUMA) {
                skill_lv = pc_checkskill(sd, 525);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(525, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 525, skill_lv);
                    sd->canskill_tick = now + 1200; return;
                }
            }

            // Throw Kunai (ID: 524) - Requires Kunai ammo
            if (get_aa_var(sd, "AA_USE_SKILL_THROW_KUNAI") > 0) {
                skill_lv = pc_checkskill(sd, 524);
                // Source check for Kunai (Ammo type)
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(524, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 524, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Throw Shuriken (ID: 523)
            if (get_aa_var(sd, "AA_USE_SKILL_THROW_SHURIKEN") > 0) {
                skill_lv = pc_checkskill(sd, 523);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(523, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 523, skill_lv);
                    sd->canskill_tick = now + 500; return;
                }
            }

            // Throw Zeny (ID: 526)
            if (get_aa_var(sd, "AA_USE_SKILL_THROW_ZENY") > 0 && sd->status.zeny >= 1000) {
                skill_lv = pc_checkskill(sd, 526);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(526, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 526, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }
            break;
        }

        case JOB_GUNSLINGER:
        {
            int target_id = sd->ud.target;
            if (!target_id) break;

            // Rapid Shower (ID: 515) - Requires Pistol (Revolver)
            if (get_aa_var(sd, "AA_USE_SKILL_RAPID_SHOWER") > 0 && sd->status.weapon == W_REVOLVER) {
                skill_lv = pc_checkskill(sd, 515);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(515, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 515, skill_lv);
                    sd->canskill_tick = now + 800; return;
                }
            }

            // Tracking (ID: 512) - Requires Pistol or Rifle
            if (get_aa_var(sd, "AA_USE_SKILL_TRACKING") > 0 && (sd->status.weapon == W_REVOLVER || sd->status.weapon == W_RIFLE)) {
                skill_lv = pc_checkskill(sd, 512);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(512, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 512, skill_lv);
                    sd->canskill_tick = now + 1500; return; // Longer cast/delay
                }
            }

            // Piercing Shot (ID: 514) - Requires Rifle
            if (get_aa_var(sd, "AA_USE_SKILL_PIERCING_SHOT") > 0 && sd->status.weapon == W_RIFLE) {
                skill_lv = pc_checkskill(sd, 514);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(514, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 514, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Full Buster (ID: 519) - Requires Shotgun
            if (get_aa_var(sd, "AA_USE_SKILL_FULL_BUSTER") > 0 && sd->status.weapon == W_SHOTGUN) {
                skill_lv = pc_checkskill(sd, 519);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(519, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 519, skill_lv);
                    sd->canskill_tick = now + 2000; return; // High recoil delay
                }
            }

            // Dust (ID: 518) - Requires Shotgun
            if (get_aa_var(sd, "AA_USE_SKILL_DUST") > 0 && sd->status.weapon == W_SHOTGUN) {
                skill_lv = pc_checkskill(sd, 518);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(518, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 518, skill_lv);
                    sd->canskill_tick = now + 1000; return;
                }
            }

            // Spread Attack (ID: 520) - Requires Shotgun (AoE)
            if (get_aa_var(sd, "AA_USE_SKILL_SPREAD_ATTACK") > 0 && sd->status.weapon == W_SHOTGUN && mob_count >= 2) {
                skill_lv = pc_checkskill(sd, 520);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(520, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 520, skill_lv);
                    sd->canskill_tick = now + 1200; return;
                }
            }

            // Disarm (ID: 513) - Requires Rifle
            if (get_aa_var(sd, "AA_USE_SKILL_DISARM") > 0 && sd->status.weapon == W_RIFLE) {
                skill_lv = pc_checkskill(sd, 513);
                if (skill_lv > 0 && sd->battle_status.sp >= skill_get_sp(513, skill_lv)) {
                    unit_skilluse_id(&sd->bl, target_id, 513, skill_lv);
                    sd->canskill_tick = now + 1000; return;
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