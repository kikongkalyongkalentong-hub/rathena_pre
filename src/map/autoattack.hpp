#ifndef MAP_AUTOATTACK_HPP
#define MAP_AUTOATTACK_HPP

#include <vector>
#include <unordered_map>
// FIXED INCLUDES: Using the correct .hpp extension AND the correct relative path
#include "../common/cbasetypes.hpp" 
#include "../common/timer.hpp"
#include "../map/pc.hpp"
#include "../map/map.hpp"
#include "../map/status.hpp" // Necessary for enum sc_type

// =====================================================================================
// === CORE CONFIGURATION CONSTANTS ===
// =====================================================================================

// Search radius (in cells) for finding nearby mobs.
static const int AUTOATTACK_RADIUS              = 9;
// Default cycles without a target before initiating a random teleport.
static const int AUTOATTACK_MISS_CYCLES_DEFAULT = 2;
// Default maximum hostile mobs in range before initiating a random teleport.
static const int AUTOATTACK_MAX_MOBS_DEFAULT    = 15;

// =====================================================================================
// === DATA STRUCTURES ===
// =====================================================================================

/**
 * @brief Configuration for a single auto-healing potion.
 */
struct autoattack_pot_entry {
    t_itemid item_id;   // The item ID of the consumable potion.
    int trigger_pct;    // Percentage (HP/SP) at which healing should START.
    int target_pct;     // Percentage (HP/SP) at which healing should STOP.
    bool is_sp;         // True if the potion heals SP, False if it heals HP.
};

/**
 * @brief Per-player auto-attack configuration (stores user-specific settings).
 */
struct autoattack_config {
    bool enabled = false;                           // Toggles the overall auto-attack functionality.
    bool use_aspd_pots = true;                      // Flag to enable/disable auto-casting ASPD buff potions.
    bool use_heal_pots = true;                      // Flag to enable/disable auto-casting HP/SP heal potions.
    int max_mobs_before_tp = AUTOATTACK_MAX_MOBS_DEFAULT; // Mob count limit before teleport.
    int idle_cycles_before_tp = AUTOATTACK_MISS_CYCLES_DEFAULT; // Idle cycle limit before teleport.
    std::vector<t_itemid> aspd_pot_ids;             // Custom list of ASPD potion Item IDs.
    std::vector<autoattack_pot_entry> heal_pot_configs; // Custom list of HP/SP potion configurations.
};

/**
 * @brief Helper enum for defining how to check if a buff item should be used.
 */
enum e_autoitem_check {
    AUTOITEM_CHECK_SC,        // Check if a specific status change (SC) is active.
    AUTOITEM_CHECK_ASPD_GROUP // Check if *any* ASPD potion SC is active.
};

/**
 * @brief Defines the properties of an auto-attack item.
 */
struct autoattack_item_entry {
    t_itemid item_id;
    e_autoitem_check check_type;
    enum sc_type sc_if_sc;
};


// =====================================================================================
// === PUBLIC FUNCTION PROTOTYPES ===
// =====================================================================================

/**
 * @brief The main timer function that drives the auto-attack loop (runs every 2s).
 */
int autoattack_timer(int tid, t_tick tick, int id, intptr_t data);

/**
 * @brief Retrieves or initializes the autoattack_config struct for a player.
 */
autoattack_config& get_autoattack_config(struct map_session_data* sd);

#endif // MAP_AUTOATTACK_HPP