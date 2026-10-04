# Vendored BearSSL

Upstream: https://www.bearssl.org/  (git://git.bearssl.org/BearSSL)
Vendored commit: 7bea48e
Imported: src/ and inc/ only (the library); upstream tools/, test/, samples/
and build scripts are not included.

Used by VitaSDR for TLS (OpenWebRX over wss://). Built as a static library by
the top-level CMakeLists.txt. BearSSL is MIT-licensed; see LICENSE.txt.

Do not hand-edit these files. The CA trust-anchor table in
src/core/ca_bundle.c is generated from a PEM bundle by tools/gen_ca.c.
