#pragma once

/*
 * Asks every running Hyprland, of any user, to draw the island above its lock screen. A config
 * reload drops the rule, so call it again before each scan.
 */
void hyprland_allow_above_lock(void);
