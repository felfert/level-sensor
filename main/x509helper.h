#pragma once

#include "mbedtls/x509_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

extern size_t getOidByName(const mbedtls_x509_name *dn, const char* target_short_name,
        char *value, size_t value_length);
#ifdef __cplusplus
}
#endif
