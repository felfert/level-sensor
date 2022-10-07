#pragma once

#include<string>
#include "mbedtls/x509_crt.h"

extern void getOidByName(const mbedtls_x509_name *dn, const char* target_short_name, std::string &value);
