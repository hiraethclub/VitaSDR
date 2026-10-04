/* Bundled CA trust anchors for TLS certificate verification (OpenWebRX over
 * wss). Generated into ca_bundle.c from the host CA store by tools/gen_ca.c;
 * regenerate rather than hand-editing. */
#ifndef VITASDR_CA_BUNDLE_H
#define VITASDR_CA_BUNDLE_H

#include "bearssl.h"

extern const br_x509_trust_anchor VITASDR_TAs[];
extern const size_t VITASDR_TAs_NUM;

#endif /* VITASDR_CA_BUNDLE_H */
