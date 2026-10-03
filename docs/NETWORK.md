# Native networking and HTML browser

ArkOS 0.11.0 contains an original polling network driver and a bounded IPv4/HTTP client. The guest sends real Ethernet frames through Intel 82540EM DMA rings. It does not use a Linux kernel, a Linux network stack, host socket syscalls, or a host API that fetches URLs on its behalf. QEMU's user network supplies the virtual Ethernet peer, DHCP server and NAT.

## Implemented boundary

- PCI ID `8086:100e` (Intel 82540EM, QEMU `e1000`), firmware-assigned MMIO BARs, bus mastering, 64 RX and 32 TX descriptors. No interrupt-driven NIC path, checksum offload, Wi-Fi or other NIC models are claimed. Physical 82540EM hardware has not been tested.
- Ethernet II, ARP replies/cache, IPv4 checksums and ICMP echo replies. IPv4 fragments are dropped; there is no IPv6.
- DHCP discovery, request, bounded retries, renewal, rebinding and expiry. Production DNS/address configuration comes from DHCP.
- UDP with checksum validation; DNS A queries, compressed names and bounded CNAME following. Truncated DNS replies requiring TCP fallback produce an explicit error.
- One TCP connection with sequence/ACK validation, MSS negotiation, duplicate/overlap handling, retransmission and timeout bounds. The client uses one outstanding segment and a fixed receive window. It is intentionally small: no general sockets API, out-of-order queue, SACK, window scaling, full congestion-control implementation, server listeners.
- One HTTP/1.x GET, literal IPv4 or DNS host, optional port, 30-second total request deadline. Response framing supports Content-Length, chunked transfer encoding and connection close. Body capacity is exactly 16 KiB plus a terminating NUL. Header/trailer capacity is 4 KiB. Conflicting framing, oversized responses, compressed content and protocol upgrades fail explicitly. HTTP error statuses remain available; redirects are not automatically followed.

The independent Ring 3 **Ark Web** reader parses a bounded HTML subset: UTF-8 text, headings, paragraphs, lists, entities, image alt text and clickable links. Script/style/head content is hidden. Relative paths, query references, an eight-entry history and wheel scrolling are supported. Ctrl+L selects the address; Enter or Visit issues a request. HTTPS uses the guest TLS adapter. CSS, JavaScript, image decoding, forms and downloads are not supported. The document and request state are cleared when the desktop session changes.

There is no host IME companion or host fetching bridge. Pinyin runs locally in the Notes process. The browser starts with UI only; its first HTTP request requires trusted consent. The kernel supplies the process identity and enforces NETWORK on every request. Revocation cancels the active owner and denies subsequent calls. The reader has no SYSTEM or FILES capability. This remains a bounded HTML reader, not a complete browser engine.

The ring-3 browser calls the public `ARK_SYS_NETWORK` service in `include/ark_api.h`. The kernel retains the response buffer, copies bounded reads into validated user memory, and ties request ownership to the initiating process. `include/net.h` is the internal kernel interface; IPv4 integers use dotted order (`10.0.2.15 == 0x0a00020f`). `net_poll()` performs at most 32 receive-descriptor deliveries per invocation.

## Reproduce

Launch with a real virtual adapter, for example:

```sh
qemu-system-x86_64 ... -netdev user,id=net0 -device e1000,netdev=net0,romfile=
```

`romfile=` disables optional PXE ROM loading; it does not disable the adapter. The guest normally receives `10.0.2.15/24`, gateway `10.0.2.2`, resolver `10.0.2.3`. A controlled HTTP server on host loopback port 8080 can be reached at `http://10.0.2.2:8080/`. Network behavior depends on host routing/firewall and the server; the default URL does not imply a bundled web server or automatic Internet access.

```sh
make check-net-host
python3 tests/tls_vm_test.py
python3 tests/https_public_vm_test.py
```

The hosted test runs ASan/UBSan over incremental HTTP framing, URL limits, DNS compression, TCP checksum/sequence/duplicate handling, completed-response preservation, offline deadlines, and 12,000 malformed byte sequences. These are parser tests, not evidence of hardware networking.

The current TLS VM test builds an isolated diagnostic kernel and ISO, starts controlled TLS/HTTP and DNS fixtures, and boots both BIOS and UEFI with QEMU TCG and e1000/slirp. It verifies actual DHCP, ARP, UDP DNS, TCP, encrypted responses, certificate failures, altered records and subsequent plain HTTP recovery. Ethernet captures and results are in `build/test-tls`. The separate public test uses the production browser and its public roots. Both tests use independent project-created disks.

Only the isolated TLS diagnostic defines `NET_DNS_PORT=55353` and points its resolver at the controlled host fixture `10.0.2.2`. These are real guest Ethernet/UDP DNS packets. Production retains DHCP-provided DNS on standard port 53. The historical plain-HTTP diagnostic and its results remain separate.

## Chromium status and port gap

Chromium is **not installed or ported** in this release. Ark Web contains no Chromium, Blink or V8. Renaming it would not supply those capabilities.

Chromium's Ozone interface provides a useful window/input/display integration boundary: a real ArkOS port would need platform window/event handling, surface allocation and presentation, screen/cursor support, and browser/GPU-process communication. This is only part of an OS port. ArkOS would also need a suitable C/C++ runtime and OS abstraction layer, substantially broader virtual-memory/thread/process/IPC primitives, networking and TLS integration, fonts/text/shaping and filesystem support, and a sandbox/security design for untrusted web content. These are porting gaps inferred from the current ArkOS implementation and Chromium's platform documents, not features delivered here.

A future Chromium port needs a native OS abstraction layer; existing Linux Chromium binaries and their sandbox cannot run against the current ArkOS ABI.

## Primary references

- Intel-authored *8254x Family Gigabit Ethernet Controllers Software Developer's Manual*, university-hosted copy: <https://tc.gts3.org/cs3210/2016/spring/r/hardware/8254x_GBe_SDM.pdf>. Register and legacy descriptor formats informed the original driver; no driver source was copied.
- ARP RFC 826: <https://www.rfc-editor.org/rfc/rfc826.html>
- IPv4 RFC 791: <https://www.rfc-editor.org/rfc/rfc791.html>
- UDP RFC 768: <https://www.rfc-editor.org/rfc/rfc768.html>
- DHCP RFC 2131 / options RFC 2132: <https://www.rfc-editor.org/rfc/rfc2131.html>, <https://www.rfc-editor.org/rfc/rfc2132.html>
- DNS RFC 1035: <https://www.rfc-editor.org/rfc/rfc1035.html>
- TCP RFC 9293: <https://www.rfc-editor.org/rfc/rfc9293.html>
- HTTP/1.1 message syntax RFC 9112: <https://www.rfc-editor.org/rfc/rfc9112.html>
- QEMU network-device documentation: <https://www.qemu.org/docs/master/system/devices/net.html>
- Chromium Ozone: <https://chromium.googlesource.com/chromium/src/+/main/docs/ozone_overview.md>
- Chromium Linux build instructions: <https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md>
- Chromium Linux sandbox: <https://chromium.googlesource.com/chromium/src/+/main/sandbox/linux/README.md>

Protocol references retained with the implementation. This implementation does not claim full RFC conformance or production-browser security.

## Native TLS 1.2

BearSSL0.6 (MIT) runs in kernel/tls.c inside the target guest. Six ECDHE suites use AES128/256-GCM or ChaCha20-Poly1305 with RSA/ECDSA authentication. X509 checks the chain against150 pinned public roots, server name and UTC validity; RSA keys must be at least2048 bits. RDRAND entropy is required, checked through CPUID and bounded hardware retries. QEMU uses `-cpu max`; absence/failure or invalid system date produces an error, never a predictable seed or certificate bypass.

Trust data is the fixed curl/Mozilla snapshot of2025-02-25. Sources, archive/hash, licenses and offline regeneration are in third_party/bearssl/ARKOS-PORT.md. The adapter has no TLS1.3, OCSP/CRL, mutual authentication, session persistence or general sockets API. Existing16 KiB HTTP body and4 KiB headers, single-request PID ownership and30-second deadline still apply.

`tests/tls_vm_test.py` builds a separate diagnostic with an ephemeral private CA and DNS port55353. BIOS/UEFI exercise real encrypted E1000 traffic, exact decrypted bytes, hostname/expiry/untrusted issuer failures, altered AES-GCM record rejection and subsequent plain HTTP recovery. Packet captures confirm the protected body does not appear as plaintext in TLS TCP payloads. Public production roots are verified separately by the actual browser visiting https://example.com/; results are retained with each test run’s logs and payload hashes.

Wi-Fi scanning, authentication and association have no device driver target or test hardware in the present QEMU setup. The system reports the absent wireless device; its E1000 path remains wired networking. No wireless support is claimed.
