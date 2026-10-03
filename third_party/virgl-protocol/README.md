# VirGL command definitions

These unmodified MIT-licensed headers come from UTM's virglrenderer fork,
commit `71a67414013f120c158729da7f56f29b55bf4f6c`.

Upstream: https://github.com/utmapp/virglrenderer/tree/71a67414013f120c158729da7f56f29b55bf4f6c

Source archive SHA-256:
`a66489e5f60d3b7a24f97f81a8b0a56ff19c3b57eee9e15bfcaea5dcc9393b88`.

ArkOS's PCI transport, resource lifetime management and bounded command encoder
are original code in `kernel/virtio_gpu.c`. The guest does not use Linux, DRM
or Mesa. Fixed TGSI shaders are generated offline from the checked-in source
in `scripts/build-glass-shaders.py`; normal builds need no downloads.
