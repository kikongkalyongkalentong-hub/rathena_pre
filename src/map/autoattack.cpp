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


// =====================================================================================
// === PRIVATE DATA (Static to this file) ===
// =====================================================================================

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

static const int AUTOATTACK_TP_MAX_MOBS = 3;

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
    auto& config = get_autoattack_config(sd);
    if (!config.use_aspd_pots)
        return;

    if (autoattack_has_aspd_potion(sd))
        return;

    auto pot_list = get_aspd_pot_list(sd);
    if (pot_list.empty())
        return;

    // Use the highest-tier available potion immediately
    for (t_itemid item_id : pot_list) {
        if (item_id <= 0)
            continue;
        
        int idx = pc_search_inventory(sd, item_id);
        if (idx >= 0) {
            pc_useitem(sd, idx);
            break; // Used one pot, we are done
        }
    }
}

/**
 * @brief Attempts to use HP/SP healing potions based on configured thresholds.
 */
static void autoattack_try_autopots(struct map_session_data* sd)
{
    auto& config = get_autoattack_config(sd);
    if (!config.use_heal_pots)
        return;

    // Prevent spamming by checking the global item cooldown (canuseitem tick)
    if (gettick() < sd->canskill_tick)
        return; 

    static std::unordered_map<int, bool> hp_healing;
    static std::unordered_map<int, bool> sp_healing;

    auto pct = [](uint32 cur, uint32 max){
        return max == 0 ? 100 : (int)((cur * 100ULL) / max);
    };

    int hp = pct(sd->battle_status.hp, sd->battle_status.max_hp);
    int sp = pct(sd->battle_status.sp, sd->battle_status.max_sp);

    auto pots = get_heal_pot_configs(sd);
    bool used_potion = false;

    for (const auto& pot : pots) {
        if (pot.item_id <= 0)
            continue;

        bool is_sp = pot.is_sp;
        int cur_pct = is_sp ? sp : hp;
        
        auto& healing = is_sp ? sp_healing[sd->bl.id] : hp_healing[sd->bl.id];

        // 1. Check if healing should start or stop
        if (cur_pct <= pot.trigger_pct)
            healing = true;
        else if (cur_pct >= pot.target_pct)
            healing = false;

        if (!healing)
            continue;

        int idx = pc_search_inventory(sd, pot.item_id);
        if (idx < 0)
            continue;

        // Use the pot (pc_useitem applies the cooldown if successful)
        if (pc_useitem(sd, idx)) {
            used_potion = true;
            break; // Stop and wait for the cooldown/next cycle
        }
    }
    
    // Safety: If critically low but couldn't use a pot (e.g., ran out), reset state.
    if (!used_potion && (hp < 10 || sp < 10)) {
         hp_healing[sd->bl.id] = false;
         sp_healing[sd->bl.id] = false;
    }
}

// =====================================================================================
// === AUTO-BUFF (REBUFF) LOGIC (Complete Pre-Renewal) ===
// =====================================================================================

static void autoattack_rebuff(struct map_session_data* sd)
{
    if (!sd)
        return;

    const t_tick now = gettick();

    // ===================
    // UNIVERSAL FAILSAFE
    // ===================

    // still casting a skill?
    if (sd->ud.skilltimer > 0)
        return;

    // still attacking?
    if (sd->ud.attacktimer > 0)
        return;

    // global skill delay?
    if (sd->ud.canact_tick > now)
        return;

    // movement delay?
    if (sd->ud.canmove_tick > now)
        return;

    // Now safe to cast buffs
    int skill_lv = 0;
    t_tick offset = 0;

    switch ((enum e_job)sd->status.class_) {
        
        case JOB_SWORDMAN:
        case JOB_KNIGHT:
        case JOB_LORD_KNIGHT:
        {
            // Endure
            skill_lv = pc_checkskill(sd, SM_ENDURE);
            if (skill_lv > 0 && !sd->sc.data[SC_ENDURE]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, SM_ENDURE, skill_lv, now + offset, 0); offset += 50; 
            }
            bool is_two_hand_equipped = (sd->status.weapon == W_2HSWORD || sd->status.weapon == W_2HSPEAR);
            // Two-Hand Quicken
            skill_lv = pc_checkskill(sd, KN_TWOHANDQUICKEN);
            if (skill_lv > 0 && !sd->sc.data[SC_TWOHANDQUICKEN] && is_two_hand_equipped) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, KN_TWOHANDQUICKEN, skill_lv, now + offset, 0); offset += 50; 
            }
            // Parrying (LK)
            skill_lv = pc_checkskill(sd, LK_PARRYING);
            if (skill_lv > 0 && !sd->sc.data[SC_PARRYING] && is_two_hand_equipped) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, LK_PARRYING, skill_lv, now + offset, 0); offset += 50; 
            }
            // Aura Blade (LK)
            skill_lv = pc_checkskill(sd, LK_AURABLADE);
            if (skill_lv > 0 && !sd->sc.data[SC_AURABLADE]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, LK_AURABLADE, skill_lv, now + offset, 0); offset += 50; 
            }
            // Concentration (LK)
            skill_lv = pc_checkskill(sd, LK_CONCENTRATION);
            if (skill_lv > 0 && !sd->sc.data[SC_CONCENTRATION]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, LK_CONCENTRATION, skill_lv, now + offset, 0); offset += 50; 
            }
            break;
        }
        
        case JOB_MAGE:
        case JOB_WIZARD:
        case JOB_HIGH_WIZARD:
        {
            // Energy Coat
            skill_lv = pc_checkskill(sd, EF_ENERGYCOAT);
            if (skill_lv > 0 && !sd->sc.data[SC_ENERGYCOAT] && sd->battle_status.hp > sd->battle_status.max_hp / 10) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, EF_ENERGYCOAT, skill_lv, now + offset, 0); offset += 50; 
            }
            break;
        }

        case JOB_ACOLYTE:
        case JOB_PRIEST:
        case JOB_HIGH_PRIEST:
        case JOB_MONK:
        {
            // Blessing
            skill_lv = pc_checkskill(sd, AL_BLESSING);
            if (skill_lv > 0 && !sd->sc.data[SC_BLESSING]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, AL_BLESSING, skill_lv, now + offset, 0); offset += 50; 
            }
            // Increase Agility
            skill_lv = pc_checkskill(sd, AL_INCAGI);
            if (skill_lv > 0 && !sd->sc.data[SC_INCREASEAGI]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, AL_INCAGI, skill_lv, now + offset, 0); offset += 50; 
            }
            break;
        }
        
        case JOB_MERCHANT:
        case JOB_BLACKSMITH:
        case JOB_WHITESMITH:
        {
            // Adrenaline Rush
            skill_lv = pc_checkskill(sd, BS_ADRENALINE);
            if (skill_lv > 0 && !sd->sc.data[SC_ADRENALINE]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, BS_ADRENALINE, skill_lv, now + offset, 0); offset += 50; 
            }
            // Weapon Perfection
            skill_lv = pc_checkskill(sd, BS_WEAPONPERFECT);
            if (skill_lv > 0 && !sd->sc.data[SC_WEAPONPERFECTION]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, BS_WEAPONPERFECT, skill_lv, now + offset, 0); offset += 50; 
            }
            // Power Maximize (WS)
            skill_lv = pc_checkskill(sd, BS_MAXIMIZE);
            if (skill_lv > 0 && !sd->sc.data[SC_MAXIMIZEPOWER]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, BS_MAXIMIZE, skill_lv, now + offset, 0); offset += 50; 
            }
            break;
        }

        case JOB_THIEF:
        case JOB_ASSASSIN:
        case JOB_ASSASSIN_CROSS:
        {
            skill_lv = pc_checkskill(sd, ASC_EDP);

            // Check if EDP is needed AND the status is NOT already active
            if (skill_lv > 0 && !sd->sc.data[SC_EDP]) {
                
                // STEP 1: USE POISON BOTTLE (IF NOT USED ALREADY)
                int idx = pc_search_inventory(sd, 7001); // Item ID for Poison Bottle
                
                // CRITICAL FIX: We must check for the current action status.
                // If the player is already moving or attacking due to the item delay, skip this.
                if (sd->ud.attacktimer > 0 || sd->ud.skilltimer > 0 || sd->ud.canact_tick > now) {
                    break; // Skip item use if the player is busy
                }
                
                if (idx >= 0) {
                    // This attempts to use the item, triggering its action delay.
                    if (pc_useitem(sd, idx)) {
                        // SUCCESS: Item was used. NOW WE MUST EXIT FOR THIS CYCLE.
                        // The item use action/delay will block the rest of the rebuffs and skill casts.
                        // We rely on the next rebuff loop (Cycle 2) to cast the skill.
                        break; 
                    }
                }
                
                // STEP 2: CAST EDP (If we reached here, the item was not successfully used 
                // in this cycle, but we try to cast the skill if it's the right moment).

                // Only cast the skill if the item was used in a previous cycle, or the timing is right.
                // Since we don't have a specific flag, we just let the logic continue:
                skill_castend_nodamage_id(&sd->bl, &sd->bl, ASC_EDP, skill_lv, now + offset, 0); 
                offset += 50;
            }
            break;
        }
        
        case JOB_ARCHER:
        case JOB_HUNTER:
        case JOB_SNIPER:
        {
            // Improve Concentration
            skill_lv = pc_checkskill(sd, AC_CONCENTRATION);
            if (skill_lv > 0 && !sd->sc.data[SC_CONCENTRATION]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, AC_CONCENTRATION, skill_lv, now + offset, 0); offset += 50; 
            }
            // True Sight (SN)
            skill_lv = pc_checkskill(sd, SN_SIGHT);
            if (skill_lv > 0 && !sd->sc.data[SC_TRUESIGHT]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, SN_SIGHT, skill_lv, now + offset, 0); offset += 50; 
            }
            // Wind Walk
            skill_lv = pc_checkskill(sd, SN_WINDWALK);
            if (skill_lv > 0 && !sd->sc.data[SC_WINDWALK]) {
                skill_castend_nodamage_id(&sd->bl, &sd->bl, SN_WINDWALK, skill_lv, now + offset, 0); offset += 50; 
            }
            break;
        }
        
        default:
            break;
    }
}

// =====================================================================================
// === AUTO-OFFENSIVE SKILL LOGIC (LK: Bowling Bash) ===
// =====================================================================================

static void autoattack_use_offensive_skill(struct map_session_data* sd, int mob_count)
{
    if (!sd)
        return;

    t_tick now = gettick();

    // ===================
    // UNIVERSAL FAILSAFE
    // ===================

    // casting a skill?
    // if (sd->ud.skilltimer > 0)
    //     return;

    // attack animation?
    // if (sd->ud.attacktimer > 0)
    //     return;

    // global skill delay?
    // if (sd->ud.canact_tick > now)
    //     return;

    // movement delay?
    // if (sd->ud.canmove_tick > now)
    //     return;

    // skill cooldown (if used)
    // if (sd->canskill_tick > now)
    //     return;

    // ===================
    // OFFENSIVE LOGIC
    // ===================

    int skill_lv = 0;

    switch ((enum e_job)sd->status.class_) {
        
        case JOB_KNIGHT:
        case JOB_LORD_KNIGHT:
        {
            // Use Bowling Bash if mob count is between 3 and 5 (as requested)
            // if (mob_count >= 3 && mob_count <= AUTOATTACK_BB_MAX_MOBS)
            // {
                if (skill_lv > 0)
                {
                    // *** Clear the skill/item cooldown flag ***
                    sd->canskill_tick = 0;

                    // Use the positional skill function for AoE cast on self
                    skill_castend_pos(sd->bl.x, sd->bl.y, KN_BOWLINGBASH, skill_lv);

                    unit_stop_attack(&sd->bl); // Stop current action
                    clif_displaymessage(sd->fd, "Auto-Skill: Using Bowling Bash!");
                    return; 
                }
            // }
            break;
        }
        
        // Add other classes here (e.g., Hunter AoE traps)

        default:
            break;
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
 * @brief The main timer function that drives the auto-attack loop. (PUBLIC)
 */
int autoattack_timer(int tid, t_tick tick, int id, intptr_t data)
{
    struct map_session_data *sd=NULL;
    static std::unordered_map<int,int> autoattack_notarget_ticks;
    
    sd=map_id2sd(id);
    if(sd==NULL) return 0;
    
    // Safety check: Disable if player is dead
    if (pc_isdead(sd)) { 
        sd->sc.option &= ~OPTION_AUTOATTACK;
        autoattack_notarget_ticks.erase(id);
        clif_changeoption(&sd->bl);
        return 0;
    }
    
    if(sd->sc.option & OPTION_AUTOATTACK) // Check if the auto-attack flag is ON
    {
        // 1. Run support features (Buffs/Pots)
        autoattack_rebuff(sd);
        autoattack_try_consumables(sd);
        autoattack_try_autopots(sd);

        // 2. Check current mob count
        auto& config = get_autoattack_config(sd);
        int mob_count = autoattack_count_attackers(sd, AUTOATTACK_RADIUS);

        // --- TEMPORARY PRIORITY LOGIC START (Normal Attack or Teleport) ---

        // PRIORITY 1: HIGH MOB COUNT (3+): Teleport Fail-Safe
        // Uses the new threshold of 3.
        if (mob_count >= AUTOATTACK_TP_MAX_MOBS) {
            bool can_teleport = !(map_getmapflag(sd->bl.m, MF_NOTELEPORT));
            
            // Check if player is NOT currently casting a skill and can teleport
            if (can_teleport && !pc_isdead(sd) && sd->canskill_tick < gettick())
            {
                pc_randomwarp(sd, CLR_TELEPORT);
                clif_displaymessage(sd->fd, "Auto-Defense: Mob count (3+) detected. Teleporting.");

                autoattack_notarget_ticks.erase(sd->bl.id);
                // Reschedule and EXIT 
                add_timer(gettick() + 2000, autoattack_timer, sd->bl.id, 0);
                return 0; 
            }
        } 
        
        // PRIORITY 2: ALL REMAINING CASES (Mob Count 0-2): Normal Attack or Search
        // Since we have no 'else if', if mob_count is 0, 1, or 2, we fall straight here.
        {
            // Try to find a target for normal attack, or continue movement/search
            bool found = autoattack_motion(sd); 
            
            if (found)
                autoattack_notarget_ticks.erase(sd->bl.id); // Target found, reset idle counter
            else {
                // Check idle cycles to trigger teleport if wandering too long
                int &miss = autoattack_notarget_ticks[sd->bl.id];
                int idle_cycles = config.idle_cycles_before_tp;
                
                if (++miss >= idle_cycles) {
                    bool can_teleport = !(map_getmapflag(sd->bl.m, MF_NOTELEPORT));
                    if (can_teleport && !pc_isdead(sd))
                        pc_randomwarp(sd, CLR_TELEPORT);
                    miss = 0; // Reset counter after teleport
                }
            }
        }
        // --- TEMPORARY PRIORITY LOGIC END ---
        
        // Schedule next cycle
        add_timer(gettick()+2000,autoattack_timer,sd->bl.id,0);
    } else
        autoattack_notarget_ticks.erase(id); // Feature is OFF
    
    return 0;
}