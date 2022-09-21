#include "x509helper.h"
#include "mbedtls/oid.h"
#include "string.h"

size_t getOidByName(const mbedtls_x509_name *name, const char* target_short_name, char *value, size_t value_length) {
    const char* short_name = NULL;
    uint8_t found = 0;
    size_t retval = 0;

    while ((name != NULL) && !found) {
        // if there is no data for this name go to the next one
        if (!name->oid.p) {
            name = name->next;
            continue;
        }

        int ret = mbedtls_oid_get_attr_short_name(&name->oid, &short_name);
        if ((ret == 0) && (strcmp(short_name, target_short_name) == 0)) {
            found = 1;
        }

        if (found) {
            size_t bytes_to_write = (name->val.len >= value_length) ? value_length - 1 : name->val.len;

            for (size_t i = 0; i < bytes_to_write; i++) {
                char c = name->val.p[i];
                if (c < 32 || c == 127 || (c > 128 && c < 160)) {
                    value[i] = '?';
                } else {
                    value[i] = c;
                }
            }
            // null terminate
            value[bytes_to_write] = 0;
            retval = name->val.len;
        }
        name = name->next;
    }
    return retval;
}
