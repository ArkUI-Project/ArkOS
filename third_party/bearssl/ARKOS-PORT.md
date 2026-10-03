# BearSSL 0.6 for ArkOS

Upstream: https://www.bearssl.org/bearssl-0.6.tar.gz
Pinned archive SHA-256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`.
Unmodified upstream sources and the MIT LICENSE.txt are retained. net.mk compiles
portable crypto inside the x86 guest; OS entropy/time helpers, AES-NI, SSE2 and
Power8 upstream backends are disabled. Kernel crypto never touches user FPU state.
The freestanding build undefines the host's `_FORTIFY_SOURCE`, so memory routines
resolve to ArkOS's guest runtime rather than glibc `*_chk` entry points.

Original ArkOS modules kernel/tls.c and kernel/random.c adapt the engine to the
native E1000/TCP parser, CPUID-checked RDRAND and UTC RTC. There is no TLS host
service. The production client requires valid chain, host and certificate time;
it uses TLS 1.2 ECDHE with AES-GCM or ChaCha20-Poly1305. TLS 1.3, OCSP/CRL,
client certificates and session persistence are not implemented.

Public anchors come from the fixed curl/Mozilla 2025-02-25 snapshot:
https://curl.se/ca/cacert-2025-02-25.pem
Snapshot file SHA-256: `50a6277ec69113f00c5fd45f09e8b97a4b3e32daa35d3a95ab30137a55386cef`.
Its 150 roots are converted by the vendored brssl tool into include/tls_anchors.h.
The snapshot records its Mozilla NSS source and extraction date in its header;
Mozilla NSS certificate data is distributed under MPL-2.0. This is a pinned
snapshot, not a claim of a current or continuously updated trust store.

To regenerate offline (host tool used only at build time):

    make -C third_party/bearssl BUILD=../../build/bear-host tools
    build/bear-host/brssl ta third_party/bearssl/cacert-2025-02-25.pem > include/tls_anchors.h

The diagnostic tests/tls_vm_test.py generates an ephemeral private CA and links a
separate test kernel. It validates encrypted real-DMA requests, hostname, expiry,
untrusted issuer and altered AEAD records on BIOS/UEFI. Neither the private CA nor
its keys are included in the production trust store or distribution.
