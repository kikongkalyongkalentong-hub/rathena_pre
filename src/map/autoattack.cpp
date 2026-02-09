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
static int get_aa_tick(struct map_session_data* sd, const char* var) {
    if (!sd) return 0;
    return pc_readglobalreg(sd, add_str(var));
}

static void set_aa_tick(struct map_session_data* sd, const char* var, int value) {
    if (!sd) return;
    pc_setglobalreg(sd, add_str(var), value);
}

static void autoattack_rebuff(struct map_session_data* sd)
{
    if (!sd) return;
    const t_tick now = gettick();

    if (sd->ud.skilltimer > 0 || sd->ud.attacktimer > 0 || 
        sd->ud.canact_tick > now || sd->ud.canmove_tick > now || DIFF_TICK(sd->canskill_tick, now) > 0)
        return;

    // Use a local variable to store the level of the skill we actually DECIDE to use
    int final_skill_lv = 0;

    if (!sd->sc.data[SC_CONCENTRATE]) {
        set_aa_tick(sd, "AA_IC_EXP", 0);
    }

    auto get_needed_buff = [&]() -> e_skill {
        int temp_lv = 0;
        
        auto can_cast = [&](int s_id, int sc_id, const char* var) -> bool {
            if (var != nullptr && get_aa_var(sd, var) <= 0) return false;
            temp_lv = pc_checkskill(sd, s_id);
            if (temp_lv <= 0) return false;
            if (sd->battle_status.sp < skill_get_sp(s_id, temp_lv)) return false;
            if (sd->sc.data[sc_id]) return false;
            
            final_skill_lv = temp_lv; // Lock in the level for the successful skill
            return true;
        };

        switch ((enum e_job)sd->status.class_) {
            case JOB_KNIGHT: case JOB_LORD_KNIGHT: case JOB_SWORDMAN: case JOB_CRUSADER: case JOB_PALADIN:
                if (can_cast(LK_AURABLADE, SC_AURABLADE, "AA_USE_BUFF_AURA_BLADE")) return (e_skill)LK_AURABLADE;
                if (can_cast(LK_CONCENTRATION, SC_CONCENTRATION, "AA_USE_BUFF_CONCENTRATION")) return (e_skill)LK_CONCENTRATION;
                if (sd->status.weapon == W_2HSWORD) {
                    if (can_cast(KN_TWOHANDQUICKEN, SC_TWOHANDQUICKEN, "AA_USE_BUFF_2H_QUICKEN")) return (e_skill)KN_TWOHANDQUICKEN;
                    if (can_cast(LK_PARRYING, SC_PARRYING, "AA_USE_BUFF_PARRYING")) return (e_skill)LK_PARRYING;
                }
                if (can_cast(SM_ENDURE, SC_ENDURE, "AA_USE_BUFF_ENDURE")) return (e_skill)SM_ENDURE;
                if (can_cast(LK_TENSIONRELAX, SC_TENSIONRELAX, "AA_USE_BUFF_RELAX") && sd->battle_status.hp < (sd->battle_status.max_hp / 2)) return (e_skill)LK_TENSIONRELAX;
                if (can_cast(LK_BERSERK, SC_BERSERK, "AA_USE_BUFF_BERSERK") && sd->battle_status.hp < (sd->battle_status.max_hp / 5)) return (e_skill)LK_BERSERK;
                if (sd->status.shield > 0) {
                    if (can_cast(CR_AUTOGUARD, SC_AUTOGUARD, "AA_USE_BUFF_GUARD")) return (e_skill)CR_AUTOGUARD;
                    if (can_cast(CR_REFLECTSHIELD, SC_REFLECTSHIELD, "AA_USE_BUFF_REFLECT")) return (e_skill)CR_REFLECTSHIELD;
                    if (can_cast(CR_DEFENDER, SC_DEFENDER, "AA_USE_BUFF_DEFENDER")) return (e_skill)CR_DEFENDER;
                }
                if ((sd->status.weapon == W_1HSPEAR || sd->status.weapon == W_2HSPEAR) && can_cast(CR_SPEARQUICKEN, SC_SPEARQUICKEN, "AA_USE_BUFF_SPEAR_QUICKEN")) return (e_skill)CR_SPEARQUICKEN;
                if (can_cast(CR_PROVIDENCE, SC_PROVIDENCE, "AA_USE_BUFF_PROVIDENCE")) return (e_skill)CR_PROVIDENCE;
                break;

            case JOB_ARCHER: case JOB_HUNTER: case JOB_SNIPER: case JOB_BARD: case JOB_DANCER: case JOB_CLOWN: case JOB_GYPSY:
            {
                if (can_cast(SN_SIGHT, SC_TRUESIGHT, "AA_USE_BUFF_TRUE_SIGHT")) return (e_skill)SN_SIGHT;
                if (can_cast(SN_WINDWALK, SC_WINDWALK, "AA_USE_BUFF_WIND_WALK")) return (e_skill)SN_WINDWALK;
                if (get_aa_var(sd, "AA_USE_BUFF_IMPROVE_CON") > 0) {
                    int ic_lv = pc_checkskill(sd, 45); 
                    if (ic_lv > 0) {
                        // 1. Check if the status is actually on the player
                        bool has_icon = (sd->sc.data[SC_CONCENTRATE] != NULL);

                        if (has_icon) {
                            // Keep the timer synced while the buff is active
                            set_aa_tick(sd, "AA_IC_EXP", now + 240000); 
                            return (e_skill)0; 
                        }

                        // 2. If icon is missing (Death/Dispel/Expired), ignore the old timer
                        if (sd->battle_status.sp >= skill_get_sp(45, ic_lv)) {
                            
                            // Check for performance lock (Bards/Dancers)
                            // Note: SC_DANCING usually covers both Songs and Dances
                            if (sd->sc.data[SC_DANCING]) {
                                return (e_skill)0;
                            }

                            printf("[AA_DEBUG] Bard IC: Icon missing. Re-casting.\n");
                            
                            // Reset the timer since we are actually re-casting now
                            set_aa_tick(sd, "AA_IC_EXP", 0);

                            final_skill_lv = ic_lv;
                            return (e_skill)45;
                        }
                    }
                }
                if (can_cast(321, SC_MARIONETTE, "AA_USE_BUFF_MARIONETTE")) return (e_skill)321;
                if (sd->sc.data[SC_DANCING]) {
                    if (can_cast(311, SC_LONGING, "AA_USE_BUFF_LONGING")) return (e_skill)311;
                }
                if (sd->status.weapon == W_WHIP && !sd->sc.data[SC_DANCING]) {
                    if (can_cast(401, SC_SERVICE4U, "AA_USE_BUFF_SERVICE")) return (e_skill)401;
                    if (can_cast(403, SC_HUMMING, "AA_USE_BUFF_HUMMING")) return (e_skill)403;
                    if (can_cast(404, SC_DONTFORGETME, "AA_USE_BUFF_KISS")) return (e_skill)404;
                }
                if (sd->status.weapon == W_MUSICAL && !sd->sc.data[SC_DANCING]) {
                    if (can_cast(312, SC_POEMBRAGI, "AA_USE_BUFF_BRAGI")) return (e_skill)312;
                    if (can_cast(313, SC_ASSNCROS, "AA_USE_BUFF_ASSASSIN")) return (e_skill)313;
                }
                break;
            }

            case JOB_MERCHANT: case JOB_BLACKSMITH: case JOB_WHITESMITH: case JOB_ALCHEMIST: case JOB_CREATOR:
            {
                if (can_cast(111, SC_ADRENALINE, "AA_USE_BUFF_ADRENALINE")) return (e_skill)111;
                if (can_cast(112, SC_WEAPONPERFECTION, "AA_USE_BUFF_PERFECTION")) return (e_skill)112;
                if (get_aa_var(sd, "AA_USE_BUFF_MAXIMIZE") > 0) {
                    if (pc_checkskill(sd, 114) > 0 && !sd->sc.data[SC_MAXIMIZEPOWER]) {
                        final_skill_lv = pc_checkskill(sd, 114);
                        return (e_skill)114;
                    }
                }
                if (!sd->sc.data[SC_MAXOVERTHRUST]) {
                    if (can_cast(113, SC_OVERTHRUST, "AA_USE_BUFF_OVERTHRUST")) return (e_skill)113;
                }
                if (can_cast(387, SC_CARTBOOST, "AA_USE_BUFF_CARTBOOST")) return (e_skill)387;
                if (can_cast(384, SC_MELTDOWN, "AA_USE_BUFF_MELTDOWN")) return (e_skill)384;
                if (can_cast(486, SC_MAXOVERTHRUST, "AA_USE_BUFF_OVERTHRUSTMAX")) return (e_skill)486;
                if (can_cast(479, SC_CP_ARMOR, "AA_USE_BUFF_FCP")) return (e_skill)479;
                if (!sd->sc.data[SC_CP_ARMOR]) {
                    if (can_cast(234, SC_CP_WEAPON, "AA_USE_BUFF_CP_WEAPON")) return (e_skill)234;
                    if (can_cast(236, SC_CP_ARMOR, "AA_USE_BUFF_CP_ARMOR")) return (e_skill)236;
                    if (can_cast(235, SC_CP_SHIELD, "AA_USE_BUFF_CP_SHIELD")) return (e_skill)235;
                    if (can_cast(237, SC_CP_HELM, "AA_USE_BUFF_CP_HELM")) return (e_skill)237;
                }
                break;
            }

            case JOB_MAGE: case JOB_WIZARD: case JOB_HIGH_WIZARD: case JOB_SAGE: case JOB_PROFESSOR:
            {
                if (can_cast(157, SC_ENERGYCOAT, "AA_USE_BUFF_ECOAT")) return (e_skill)157;
                if (can_cast(482, SC_DOUBLECAST, "AA_USE_BUFF_DOUBLECAST")) return (e_skill)482;
                if (can_cast(403, SC_MEMORIZE, "AA_USE_BUFF_MEMORIZE")) return (e_skill)403;
                if (!sd->sc.data[SC_FIREWEAPON] && !sd->sc.data[SC_WATERWEAPON] && !sd->sc.data[SC_WINDWEAPON] && !sd->sc.data[SC_EARTHWEAPON]) {
                    if (can_cast(280, SC_FIREWEAPON, "AA_USE_BUFF_FIRE_WPN")) return (e_skill)280;
                    if (can_cast(281, SC_WATERWEAPON, "AA_USE_BUFF_WATER_WPN")) return (e_skill)281;
                    if (can_cast(282, SC_WINDWEAPON, "AA_USE_BUFF_WIND_WPN")) return (e_skill)282;
                    if (can_cast(283, SC_EARTHWEAPON, "AA_USE_BUFF_EARTH_WPN")) return (e_skill)283;
                }
                if (get_aa_var(sd, "AA_USE_BUFF_INDULGE") > 0) {
                    int indulge_lv = pc_checkskill(sd, 373);
                    if (indulge_lv > 0 && sd->battle_status.hp > (sd->battle_status.max_hp * 8 / 10) && 
                        sd->battle_status.sp < (sd->battle_status.max_sp * 3 / 10)) {
                        final_skill_lv = indulge_lv;
                        return (e_skill)373;
                    }
                }

                break;
            }

            case JOB_ACOLYTE: case JOB_PRIEST: case JOB_HIGH_PRIEST: case JOB_MONK: case JOB_CHAMPION:
            {
                if (can_cast(34, SC_BLESSING, "AA_USE_BUFF_BLESSING")) return (e_skill)34;
                if (can_cast(29, SC_INCREASEAGI, "AA_USE_BUFF_INCAGI")) return (e_skill)29;
                if (can_cast(33, SC_ANGELUS, "AA_USE_BUFF_ANGELUS")) return (e_skill)33;
                if (can_cast(361, SC_ASSUMPTIO, "AA_USE_BUFF_ASSUMPTIO")) return (e_skill)361;
                if (!sd->sc.data[SC_ASSUMPTIO]) {
                    if (can_cast(73, SC_KYRIE, "AA_USE_BUFF_KYRIE")) return (e_skill)73;
                }
                if (can_cast(74, SC_MAGNIFICAT, "AA_USE_BUFF_MAGNIFICAT")) return (e_skill)74;
                if (can_cast(75, SC_GLORIA, "AA_USE_BUFF_GLORIA")) return (e_skill)75;
                if (can_cast(66, SC_IMPOSITIO, "AA_USE_BUFF_IMPOSITIO")) return (e_skill)66;

                break;
            }

            case JOB_THIEF: case JOB_ASSASSIN: case JOB_ASSASSIN_CROSS:
            case JOB_ROGUE: case JOB_STALKER:
            {
                if (can_cast(378, SC_EDP, "AA_USE_BUFF_EDP")) return (e_skill)378;
                if (!sd->sc.data[SC_EDP]) {
                    if (can_cast(138, SC_ENCPOISON, "AA_USE_BUFF_ENCPOISON")) return (e_skill)138;
                }
                if (can_cast(139, SC_POISONREACT, "AA_USE_BUFF_POISONREACT")) return (e_skill)139;
                if (pc_checkskill(sd, 219) > 0) {
                    if (can_cast(475, SC_PRESERVE, "AA_USE_BUFF_PRESERVE")) return (e_skill)475;
                }
                if (can_cast(471, SC_REJECTSWORD, "AA_USE_BUFF_REJECTSWORD")) return (e_skill)471;

                break;
            }

            case JOB_SUPER_NOVICE:
            {
                if (can_cast(34, SC_BLESSING, "AA_USE_BUFF_BLESSING")) return (e_skill)34;
                if (can_cast(29, SC_INCREASEAGI, "AA_USE_BUFF_INCAGI")) return (e_skill)29;
                if (can_cast(33, SC_ANGELUS, "AA_USE_BUFF_ANGELUS")) return (e_skill)33;
                if (can_cast(8, SC_ENDURE, "AA_USE_BUFF_ENDURE")) return (e_skill)8;
                if (get_aa_var(sd, "AA_USE_BUFF_IMPROVE_CON") > 0) {
                    int ic_lv = pc_checkskill(sd, 45); 
                    if (ic_lv > 0) {
                        if (sd->sc.data[SC_CONCENTRATE]) {
                            break;
                        }
                        int expire_at = get_aa_tick(sd, "AA_IC_EXP");
                        int time_left = expire_at - now;
                        if (time_left > 3600000 || time_left < -3600000) {
                            expire_at = 0;
                        }
                        if (expire_at > 0 && now < (expire_at - 2000)) {
                            break; 
                        }
                        if (sd->battle_status.sp >= skill_get_sp(45, ic_lv)) {
                            final_skill_lv = ic_lv;
                            return (e_skill)45;
                        }
                    }
                }
                if (can_cast(157, SC_ENERGYCOAT, "AA_USE_BUFF_ECOAT")) return (e_skill)157;

                break;
            }

            case JOB_GUNSLINGER:
            {
                if (can_cast(503, (sc_type)160, "AA_USE_BUFF_ACCURACY")) return (e_skill)503;
                if (sd->status.weapon == W_GATLING) {
                    if (can_cast(509, (sc_type)163, "AA_USE_BUFF_GATLING")) return (e_skill)509;
                }
                if (sd->status.weapon == W_REVOLVER) {
                    if (can_cast(510, (sc_type)164, "AA_USE_BUFF_MADNESS")) return (e_skill)510;
                }
                break;
            }

            case JOB_NINJA:
            {
                if (can_cast(516, (sc_type)167, "AA_USE_BUFF_CICADA")) return (e_skill)516;   
                if (sd->sc.data[165]) {
                    if (can_cast(527, (sc_type)173, "AA_USE_BUFF_MIRROR")) return (e_skill)527;
                }
                if (can_cast(512, (sc_type)165, "AA_USE_BUFF_NINJA_AURA")) return (e_skill)512;

                break;
            }

            case JOB_TAEKWON: case JOB_STAR_GLADIATOR: case JOB_SOUL_LINKER:
            {
                if (can_cast(411, (sc_type)115, "AA_USE_BUFF_RUNNING")) return (e_skill)411;
                if (can_cast(462, (sc_type)148, "AA_USE_BUFF_LINK")) return (e_skill)462;
                if (can_cast(436, (sc_type)131, "AA_USE_BUFF_PROTECTION")) return (e_skill)436;

                break;
            }

            default: break;
        }
        return (e_skill)0; 
    };

    e_skill skill_id = get_needed_buff();
    
    if (skill_id != (e_skill)0) {
        if (final_skill_lv <= 0) final_skill_lv = 1;

        // Execute the skill
        unit_skilluse_id(&sd->bl, sd->bl.id, (uint16)skill_id, (uint16)final_skill_lv);
        
        // Handling for Improve Concentration (Skill ID 45)
        if (skill_id == (e_skill)45) {
            int duration = skill_get_time(45, final_skill_lv);
            
            // FIX: If duration is in seconds (e.g. 240), convert to milliseconds (240000)
            if (duration > 0 && duration < 5000) {
                duration *= 1000;
            }

            set_aa_tick(sd, "AA_IC_EXP", (int)(now + duration));
            sd->canskill_tick = now + 2000; // Delay next buff check
        } else {
            // Standard delay for all other buffs
            sd->canskill_tick = now + 500; 
        }
    }
}

// =====================================================================================
// === AUTO-OFFENSIVE SKILL LOGIC ===
// =====================================================================================

/**
 * Helper function to handle repetitive skill checks
 * Returns true if the skill was successfully cast
 **/
static bool aa_can_use_offensive_skill(struct map_session_data* sd, int target_id, int skill_id, int delay, const char* var_name, int mob_count_req = 0, int current_mobs = 0) 
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

    // --- FIX FOR SELF-CENTRIC SKILLS (Magnum Break, Grand Cross, etc.) ---
    // If the skill is Magnum Break (7) or Grand Cross (254), 
    // we MUST target the player (sd->bl.id), not the monster.
    int final_target_id = target_id;
    if (skill_id == 7 || skill_id == 254 || skill_id == 406) { // 406 is Meteor Assault
        final_target_id = sd->bl.id;
    }

    // 5. Cast the skill using the corrected target
    unit_skilluse_id(&sd->bl, final_target_id, skill_id, skill_lv);
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
            if (has_spear && aa_can_use_offensive_skill(sd, target_id, 397, 1200, "AA_USE_SKILL_SPIRAL_PIERCE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 398, 800, "AA_USE_SKILL_HEAD_CRUSH")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 399, 800, "AA_USE_SKILL_JOINT_BEAT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 367, 800, "AA_USE_SKILL_PRESSURE")) return;
            if (has_shield && aa_can_use_offensive_skill(sd, target_id, 480, 800, "AA_USE_SKILL_SHIELD_CHAIN")) return;
            if (aa_can_use_offensive_skill(sd, sd->bl.id, 254, 1500, "AA_USE_SKILL_GRAND_CROSS", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 253, 800, "AA_USE_SKILL_HOLY_CROSS")) return;
            if (has_shield && aa_can_use_offensive_skill(sd, target_id, 251, 800, "AA_USE_SKILL_SHIELD_BOOMERANG")) return;
            if (has_shield && aa_can_use_offensive_skill(sd, target_id, 250, 800, "AA_USE_SKILL_SHIELD_CHARGE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 62, 1000, "AA_USE_SKILL_BOWLING_BASH", 2, mob_count)) return;
            if (has_spear && aa_can_use_offensive_skill(sd, target_id, 57, 1000, "AA_USE_SKILL_BRANDISH_SPEAR", 2, mob_count)) return;
            if (has_spear && aa_can_use_offensive_skill(sd, target_id, 59, 800, "AA_USE_SKILL_SPEAR_BOOMERANG")) return;
            if (has_spear && aa_can_use_offensive_skill(sd, target_id, 58, 800, "AA_USE_SKILL_SPEAR_STAB")) return;
            if (has_spear && aa_can_use_offensive_skill(sd, target_id, 56, 800, "AA_USE_SKILL_PIERCE")) return;
            if (aa_can_use_offensive_skill(sd, sd->bl.id, 7, 1000, "AA_USE_SKILL_MAGNUM_BREAK", 1, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 5, 800, "AA_USE_SKILL_BASH")) return;

            break;
        }

        case JOB_ARCHER: case JOB_HUNTER: case JOB_SNIPER: case JOB_BARD: case JOB_CLOWN: case JOB_DANCER: case JOB_GYPSY:
        {
            bool has_falcon = (sd->sc.option & OPTION_FALCON);
            if ((weapon == W_MUSICAL || weapon == W_WHIP) && aa_can_use_offensive_skill(sd, target_id, 394, 1500, "AA_USE_SKILL_ARROW_VULCAN")) return;
            if (has_falcon && aa_can_use_offensive_skill(sd, target_id, 381, 1000, "AA_USE_SKILL_FALCON_ASSAULT")) return;
            if (weapon == W_BOW && aa_can_use_offensive_skill(sd, target_id, 382, 1200, "AA_USE_SKILL_SHARP_SHOOTING", 2, mob_count)) return;
            if (weapon == W_MUSICAL && aa_can_use_offensive_skill(sd, target_id, 316, 800, "AA_USE_SKILL_MUSICAL_STRIKE")) return;
            if (weapon == W_WHIP && aa_can_use_offensive_skill(sd, target_id, 324, 800, "AA_USE_SKILL_THROW_ARROW")) return;
            if (has_falcon && aa_can_use_offensive_skill(sd, target_id, 129, 1000, "AA_USE_SKILL_BLITZ_BEAT", 2, mob_count)) return;
            if (weapon == W_BOW && aa_can_use_offensive_skill(sd, target_id, 47, 800, "AA_USE_SKILL_ARROW_SHOWER", 2, mob_count)) return;
            if (weapon == W_BOW && aa_can_use_offensive_skill(sd, target_id, 46, 600, "AA_USE_SKILL_DOUBLE_STRAFE")) return;

            break;
        }

        case JOB_MAGE: case JOB_WIZARD: case JOB_HIGH_WIZARD: case JOB_SAGE: case JOB_PROFESSOR:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 89, 1500, "AA_USE_SKILL_STORM_GUST", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 83, 1500, "AA_USE_SKILL_METEOR_STORM", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 85, 1500, "AA_USE_SKILL_VERMILION", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 300, 1200, "AA_USE_SKILL_NAPALM_VULCAN")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 91, 1200, "AA_USE_SKILL_HEAVEN_DRIVE", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 90, 1000, "AA_USE_SKILL_EARTH_SPIKE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 84, 1000, "AA_USE_SKILL_JUPITEL_THUNDER")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 86, 1000, "AA_USE_SKILL_WATER_BALL")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 21, 1200, "AA_USE_SKILL_THUNDER_STORM", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 17, 800, "AA_USE_SKILL_FIRE_BALL", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 19, 1000, "AA_USE_SKILL_FIRE_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 14, 1000, "AA_USE_SKILL_COLD_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 20, 1000, "AA_USE_SKILL_LIGHTNING_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 13, 800, "AA_USE_SKILL_SOUL_STRIKE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 11, 800, "AA_USE_SKILL_NAPALM_BEAT", 2, mob_count)) return;

            break;
        }

        case JOB_ASSASSIN: case JOB_ASSASSIN_CROSS: case JOB_ROGUE: case JOB_STALKER:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 406, 1000, "AA_USE_SKILL_METEOR_ASSAULT", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 379, 1200, "AA_USE_SKILL_SOUL_BREAKER")) return;
            if (weapon == W_KATAR && aa_can_use_offensive_skill(sd, target_id, 136, 2000, "AA_USE_SKILL_SONIC_BLOW")) return;
            if (weapon == W_BOW && aa_can_use_offensive_skill(sd, target_id, 46, 600, "AA_USE_SKILL_DOUBLE_STRAFE")) return;

            break;
        }

        case JOB_ACOLYTE: case JOB_PRIEST: case JOB_HIGH_PRIEST:
        case JOB_MONK: case JOB_CHAMPION:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 79, 2000, "AA_USE_SKILL_MAGNUS", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 77, 1000, "AA_USE_SKILL_TURN_UNDEAD")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 15, 800, "AA_USE_SKILL_HOLY_LIGHT")) return;

            break;
        }

        case JOB_MERCHANT: case JOB_BLACKSMITH: case JOB_WHITESMITH:
        case JOB_ALCHEMIST: case JOB_CREATOR:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 490, 1000, "AA_USE_SKILL_ACID_DEMO")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 408, 600, "AA_USE_SKILL_CART_TERM")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 230, 800, "AA_USE_SKILL_ACID_TERROR")) return;
            if (sd->status.zeny >= 1000 && aa_can_use_offensive_skill(sd, target_id, 42, 600, "AA_USE_SKILL_MAMMONITE")) return;

            break;
        }

        case JOB_SUPER_NOVICE:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 21, 1500, "AA_USE_SKILL_THUNDER_STORM", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 17, 1000, "AA_USE_SKILL_FIRE_BALL", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 19, 1000, "AA_USE_SKILL_FIRE_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 14, 1000, "AA_USE_SKILL_COLD_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 20, 1000, "AA_USE_SKILL_LIGHTNING_BOLT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 13, 800, "AA_USE_SKILL_SOUL_STRIKE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 11, 800, "AA_USE_SKILL_NAPALM_BEAT", 2, mob_count)) return;
            if (sd->status.zeny >= 1000 && aa_can_use_offensive_skill(sd, target_id, 42, 800, "AA_USE_SKILL_MAMMONITE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 7, 1000, "AA_USE_SKILL_MAGNUM_BREAK", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 5, 800, "AA_USE_SKILL_BASH")) return;
            
            break;
        }

        case JOB_NINJA:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 536, 1500, "AA_USE_SKILL_DRAGON_FIRE", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 542, 1200, "AA_USE_SKILL_NORTH_WIND", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 534, 1000, "AA_USE_SKILL_FIRE_BLOSSOM")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 537, 1000, "AA_USE_SKILL_SPEAR_ICE")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 540, 1000, "AA_USE_SKILL_WIND_BLADE")) return;
            if (weapon == W_HUUMA && aa_can_use_offensive_skill(sd, target_id, 525, 1200, "AA_USE_SKILL_THROW_HUUMA")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 524, 800, "AA_USE_SKILL_THROW_KUNAI")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 523, 600, "AA_USE_SKILL_THROW_SHURIKEN")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 528, 800, "AA_USE_SKILL_MIST_SLASH")) return;
            if (sd->status.zeny >= 500 && aa_can_use_offensive_skill(sd, target_id, 526, 1000, "AA_USE_SKILL_THROW_ZENY")) return;

            break;
        }

        case JOB_GUNSLINGER:
        {
            if (weapon == W_REVOLVER && aa_can_use_offensive_skill(sd, target_id, 515, 800, "AA_USE_SKILL_RAPID_SHOWER")) return;
            if (weapon == W_SHOTGUN && aa_can_use_offensive_skill(sd, target_id, 519, 2000, "AA_USE_SKILL_FULL_BUSTER")) return;
            if (weapon == W_SHOTGUN && aa_can_use_offensive_skill(sd, target_id, 520, 1200, "AA_USE_SKILL_SPREAD_ATTACK", 2, mob_count)) return;
            if (aa_can_use_offensive_skill(sd, target_id, 512, 1500, "AA_USE_SKILL_TRACKING")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 514, 1000, "AA_USE_SKILL_PIERCING_SHOT")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 518, 800, "AA_USE_SKILL_DUST")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 513, 800, "AA_USE_SKILL_DISARM")) return;
            
            break;
        }

        case JOB_TAEKWON: case JOB_STAR_GLADIATOR: case JOB_SOUL_LINKER:
        {
            if (aa_can_use_offensive_skill(sd, target_id, 456, 1000, "AA_USE_SKILL_ESMA")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 455, 800, "AA_USE_SKILL_ESTUN")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 454, 800, "AA_USE_SKILL_ESTIN")) return;
            if (aa_can_use_offensive_skill(sd, target_id, 414, 800, "AA_USE_SKILL_FLYING_KICK")) return;

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