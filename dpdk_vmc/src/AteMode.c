#include "AteMode.h"
#include <stdio.h>

static bool g_ate_mode = false;

static bool ask_question(const char *question) {
    char response;
    int c;

    while (1) {
        printf("%s [y/n]: ", question);
        fflush(stdout);

        response = getchar();

        // Clear rest of line
        while ((c = getchar()) != '\n' && c != EOF);

        if (response == 'y' || response == 'Y') {
            return true;
        } else if (response == 'n' || response == 'N') {
            return false;
        }

        printf("Invalid input! Please enter 'y' or 'n'.\n");
    }
}

void ate_mode_selection(void) {
    printf("=== ATE Test Mode Selection ===\n\n");

    while (1) {
        if (ask_question("Do you want to continue in ATE test mode?")) {
            printf("\n[ATE] ATE test mode selected.\n");

            // No Cumulus ATE reconfiguration needed for VMC -
            // the initial Cumulus config from configureSequence is sufficient.

            // Ask for ATE test cables - max 3 attempts before returning to ATE selection
            int cable_retry_count = 0;
            bool cables_installed = false;

            while (cable_retry_count < 3) {
                if (ask_question("Are the ATE test mode cables installed?")) {
                    cables_installed = true;
                    break;
                }
                cable_retry_count++;
                if (cable_retry_count < 3) {
                    printf("\nPlease install the ATE test mode cables and try again.\n\n");
                }
            }

            if (cables_installed) {
                g_ate_mode = true;
                printf("[ATE] ATE test mode enabled.\n\n");
                return;
            }

            printf("\n[ATE] Cable installation declined 3 times. Returning to ATE mode selection.\n\n");
            // Loop continues - asks ATE mode question again
        } else {
            printf("Continuing in normal test mode.\n\n");

            // Ask for unit test cables - max 3 attempts before returning to ATE selection
            int unit_cable_retry = 0;
            bool unit_cables_installed = false;

            while (unit_cable_retry < 3) {
                if (ask_question("Are the unit test cables installed?")) {
                    unit_cables_installed = true;
                    break;
                }
                unit_cable_retry++;
                if (unit_cable_retry < 3) {
                    printf("\nPlease install the unit test cables and try again.\n\n");
                }
            }

            if (unit_cables_installed) {
                return;
            }

            printf("\n[Unit] Cable installation declined 3 times. Returning to ATE mode selection.\n\n");
            // Loop continues - asks ATE mode question again
        }
    }
}

bool ate_mode_enabled(void) {
    return g_ate_mode;
}
