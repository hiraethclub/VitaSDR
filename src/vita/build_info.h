/* Build identity, supplied by CMake via -D defines. Fallback defaults keep the
 * native build and any direct compile working. */
#ifndef VITASDR_BUILD_INFO_H
#define VITASDR_BUILD_INFO_H

#ifndef VITASDR_DATA_DIR
#define VITASDR_DATA_DIR "ux0:data/vitasdr"
#endif

#ifndef VITASDR_BUILD_REV
#define VITASDR_BUILD_REV "dev"
#endif

#ifndef VITASDR_APP_LABEL
#define VITASDR_APP_LABEL "VitaSDR"
#endif

#ifndef VITASDR_VERSION
#define VITASDR_VERSION "0.3.0"
#endif

#ifndef VITASDR_REV_LABEL
#define VITASDR_REV_LABEL "dev"
#endif

#endif /* VITASDR_BUILD_INFO_H */
