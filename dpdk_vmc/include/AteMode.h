#ifndef ATE_MODE_H
#define ATE_MODE_H

#include <stdbool.h>

/**
 * @brief Interactive ATE mode selection
 *
 * Asks the user if they want ATE test mode.
 * No Cumulus reconfiguration is done (VMC initial config is sufficient).
 */
void ate_mode_selection(void);

/**
 * @brief Check if ATE test mode is enabled
 * @return true if ATE mode was selected by user
 */
bool ate_mode_enabled(void);

#endif // ATE_MODE_H
