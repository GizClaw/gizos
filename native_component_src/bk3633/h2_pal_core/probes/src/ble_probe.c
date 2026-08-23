#include "rwip_config.h"

#include "attm.h"
#include "gapm_task.h"
#include "gattc_task.h"

#include <stddef.h>

_Static_assert(
    offsetof(struct gapm_activity_created_ind, actv_idx) == 0u,
    "BK3633 activity-created correlation layout changed");
_Static_assert(
    sizeof(((struct gapm_set_adv_data_cmd *)0)->actv_idx) == 1u,
    "BK3633 advertising-data activity identity changed");
_Static_assert(
    sizeof(((struct gapm_activity_stopped_ind *)0)->actv_idx) == 1u,
    "BK3633 activity-stopped correlation layout changed");
_Static_assert(
    sizeof(((struct gattc_cmp_evt *)0)->seq_num) == 2u,
    "BK3633 GATT completion sequence layout changed");

int main(void) {
    (void)sizeof(&attm_svc_create_db);
    (void)sizeof(&attm_svc_create_db_128);
    return 0;
}
