// Crypto de l'enveloppe H1 sur la carte : mbedTLS, SHA-256 materiel du C6
// (CONFIG_MBEDTLS_HARDWARE_SHA). Les tests hote fournissent la leur
// (tools/host_tests/test_h1.cpp, CommonCrypto).
//
// Appels depuis la tache loop seulement (transport reseau, 'json cle') : un
// seul contexte HMAC, prepare une fois (mbedtls_md_setup alloue), puis remis
// a zero par chaque mbedtls_md_hmac_starts.
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "h1_proto.h"

namespace h1 {

static mbedtls_md_context_t sCtx;
static bool sReady = false;

static bool ready() {
  if (sReady) return true;
  mbedtls_md_init(&sCtx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&sCtx, info, 1) != 0) {
    mbedtls_md_free(&sCtx);
    return false;  // nouvel essai au prochain appel
  }
  sReady = true;
  return true;
}

bool hmacSha256(const uint8_t *key, size_t keyLen, const Part *parts, size_t nParts, uint8_t out[32]) {
  if (!ready() || mbedtls_md_hmac_starts(&sCtx, key, keyLen) != 0) return false;
  for (size_t i = 0; i < nParts; i++)
    if (parts[i].n && mbedtls_md_hmac_update(&sCtx, (const unsigned char *)parts[i].p, parts[i].n) != 0)
      return false;
  return mbedtls_md_hmac_finish(&sCtx, out) == 0;
}

bool sha256(const void *p, size_t n, uint8_t out[32]) {
  return mbedtls_sha256((const unsigned char *)p, n, out, 0) == 0;
}

}  // namespace h1
