#include "x509helper.h"
#include "mbedtls/oid.h"
#include <cstring>

void getOidByName(const mbedtls_x509_name *name, const char* target_short_name, std::string &value) {
    const char* short_name = nullptr;
    bool found = false;

    while ((name != nullptr) && !found) {
        // if there is no data for this name go to the next one
        if (!name->oid.p) {
            name = name->next;
            continue;
        }

        int ret = mbedtls_oid_get_attr_short_name(&name->oid, &short_name);
        if ((ret == 0) && (strcmp(short_name, target_short_name) == 0)) {
            found = true;
        }

        if (found) {
            for (size_t i = 0; i < name->val.len; i++) {
                char c = name->val.p[i];
                if (c < 32 || c == 127 || (c > 128 && c < 160)) {
                    value.push_back('?');
                } else {
                    value.push_back(c);
                }
            }
        }
        name = name->next;
    }
}
