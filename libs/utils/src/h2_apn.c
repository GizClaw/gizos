#include "h2_apn.h"

#include <string.h>

const char *h2_apn_from_imsi(const char *imsi) {
    if (imsi == NULL) {
        return "internet";
    }
    if (strncmp(imsi, "46000", 5) == 0 ||
        strncmp(imsi, "46002", 5) == 0 ||
        strncmp(imsi, "46004", 5) == 0 ||
        strncmp(imsi, "46007", 5) == 0 ||
        strncmp(imsi, "46008", 5) == 0) {
        return "cmnet";
    }
    if (strncmp(imsi, "46001", 5) == 0 ||
        strncmp(imsi, "46006", 5) == 0 ||
        strncmp(imsi, "46009", 5) == 0) {
        return "3gnet";
    }
    if (strncmp(imsi, "46003", 5) == 0 ||
        strncmp(imsi, "46005", 5) == 0 ||
        strncmp(imsi, "46011", 5) == 0) {
        return "ctnet";
    }
    return "internet";
}
