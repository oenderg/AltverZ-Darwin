/* Windows: the two libuuid calls the driver makes. windows.h has a uuid_t of its own (a GUID), so this one is renamed. */
#pragma once
typedef unsigned char tl_uuid_t[16];
#define uuid_t tl_uuid_t
void uuid_generate_random(tl_uuid_t out);
void uuid_unparse_lower(const tl_uuid_t in, char *out);
