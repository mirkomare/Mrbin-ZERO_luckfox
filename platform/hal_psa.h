#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSA_SUCCESS 0
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_KEY_TYPE_ECC_KEY_PAIR(fam) (0x7000u | (fam))
#define PSA_ECC_FAMILY_MONTGOMERY 0x41
#define PSA_ALG_ECDH 0x09020000
#define PSA_KEY_USAGE_DERIVE 0x4000
#define PSA_KEY_USAGE_EXPORT 0x0001

typedef int psa_status_t;
typedef uint32_t psa_key_id_t;

typedef struct {
    uint16_t type;
    uint16_t bits;
    uint32_t alg;
    uint32_t usage;
} psa_key_attributes_t;

psa_status_t psa_crypto_init(void);
void psa_set_key_type(psa_key_attributes_t *a, uint16_t t);
void psa_set_key_bits(psa_key_attributes_t *a, uint16_t b);
void psa_set_key_algorithm(psa_key_attributes_t *a, uint32_t alg);
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t u);
psa_status_t psa_generate_key(const psa_key_attributes_t *a, psa_key_id_t *id);
psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data, size_t len,
                            psa_key_id_t *id);
psa_status_t psa_export_key(psa_key_id_t id, uint8_t *data, size_t cap, size_t *olen);
psa_status_t psa_export_public_key(psa_key_id_t id, uint8_t *data, size_t cap, size_t *olen);
psa_status_t psa_raw_key_agreement(uint32_t alg, psa_key_id_t id, const uint8_t *peer,
                                   size_t peer_len, uint8_t *out, size_t cap, size_t *olen);
psa_status_t psa_destroy_key(psa_key_id_t id);

#ifdef __cplusplus
}
#endif
