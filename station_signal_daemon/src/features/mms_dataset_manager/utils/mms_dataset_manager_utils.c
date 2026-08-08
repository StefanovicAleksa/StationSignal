#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"

char*
MmsDatasetManagerUtils_safeStringDup(const char* s) {
    return s ? strdup(s) : NULL;
}
