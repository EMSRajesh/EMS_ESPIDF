/*
 * ems_profile.h - firmware activation of the generated configuration
 * (DP-WP4 4A, Design 5.3 step 4, milestone D7).
 *
 * Before any polling starts, the firmware re-checks the compiled-in tables:
 * CRC, build target and classification (V-13), table limits (V-15), and the
 * structural rules marked "FW" in Design 5.4 (V-01, V-02, V-05..V-10, V-12).
 * If anything fails, nothing is polled (DR-03).
 *
 * The host tool already applies all rules before generation; this second
 * check catches hand-edited or corrupted generated files and a configuration
 * built for the wrong target.
 *
 * Pure C, no ESP-IDF dependency; host-tested in test/host/test_ems_profile_activate.c.
 */
#ifndef EMS_PROFILE_H
#define EMS_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "ems_config_types.h"
#include "ems_register_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMS_ACT_OK = 0,
    EMS_ACT_NULL,             /* V-01 missing table                               */
    EMS_ACT_CRC,              /* stored CRC does not match the tables             */
    EMS_ACT_TARGET,           /* V-13 target / classification vs build            */
    EMS_ACT_LIMITS,           /* V-15 table sizes                                 */
    EMS_ACT_REFERENCE,        /* V-03 profile->map, device->profile/bus index      */
    EMS_ACT_MAP,              /* V-02/V-05..V-09 (ems_regmap_validate)            */
    EMS_ACT_DICTIONARY,       /* V-04 parameter not in dictionary / used twice     */
    EMS_ACT_BUS,              /* V-10 bus settings                                 */
    EMS_ACT_DEVICE,           /* V-10 slave ID, timeout, retries, probe group ...  */
    EMS_ACT_DUPLICATE_SLAVE,  /* V-10 slave ID used twice on one bus               */
    EMS_ACT_RATIO             /* V-12 METER_REPORTS_PRIMARY with CT/PT != 1       */
} ems_activation_status_t;

typedef struct {
    ems_activation_status_t status;
    ems_map_check_t map_check;  /* for EMS_ACT_MAP                                 */
    const char *rule;           /* "V-06", "CRC", ... ; never NULL                  */
    uint32_t crc_stored;
    uint32_t crc_computed;
    uint8_t  index;             /* map, profile, bus or device index of the error   */
    uint8_t  sub_index;         /* group/mapping index inside a map, if relevant    */
    bool     sim_build;
} ems_activation_report_t;

/*
 * Check a configuration build for this firmware. sim_build is true when the
 * firmware was built with CONFIG_MYEMS_CONFIG_TARGET_SIM. report may be NULL.
 */
ems_activation_status_t ems_profile_activate(const ems_config_build_t *build, bool sim_build,
                                             ems_activation_report_t *report);

/* "OK", "CRC", "TARGET", ... ; never NULL. */
const char *ems_activation_status_name(ems_activation_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* EMS_PROFILE_H */
