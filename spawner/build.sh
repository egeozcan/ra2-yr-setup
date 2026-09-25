#!/bin/bash
# build.sh - compile yspawn.dll with 32-bit mingw inside a podman container.
# The first run builds the container image (Fedora + mingw32-gcc, about 1 GB).
cd "$(dirname "$0")" || exit 1
IMAGE=localhost/ra2-mingw:latest
if ! podman image exists "$IMAGE"; then
  podman run --name ra2mingw-build registry.fedoraproject.org/fedora:44 \
    dnf -y -q install mingw32-gcc mingw32-binutils || exit 1
  podman commit -q ra2mingw-build "$IMAGE" && podman rm ra2mingw-build >/dev/null || exit 1
fi
exec podman run --rm -v "$PWD":/src:Z -w /src "$IMAGE" \
  i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -mincoming-stack-boundary=2 \
  -s -o yspawn.dll yspawn.c -Wl,--kill-at
