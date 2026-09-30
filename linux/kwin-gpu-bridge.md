# KWin virtual rendering bridge

The native Linux package preloads `libvibeshine-kwin-gpu.so` into the KDE
desktop and login compositors. Only `/usr/bin/kwin_wayland` enables its hooks;
other processes pass through unchanged and compositor children do not inherit
the preload.

The bridge gives a Vibeshine faux KMS device a GBM allocator opened on a real
NVIDIA render node. The original KMS fd remains the output's scanout endpoint.
Physical DRM devices, libdrm identities, enumeration order and KWin's choice of
seat primary are unchanged. KWin and EGL use their normal per-output GPU import
path. This also avoids advertising the virtual card as a second copy of an
NVIDIA PCI device to unrelated GBM/EGL consumers inside the compositor.

With one NVIDIA render device, association is automatic. With multiple NVIDIA
devices, set a stable PCI identity explicitly for each intended KWin service:

```ini
# /etc/systemd/user/plasma-kwin_wayland.service.d/90-virtual-renderer.conf
[Service]
Environment=VIBESHINE_KWIN_RENDER_PCI=0000:01:00.0
```

The login compositor uses `plasma-login-kwin_wayland.service.d` with the same
setting. These settings take effect when the corresponding compositor next
starts. No monitor or dummy plug is required by the bridge. An unrelated seat
GPU, including a VM's Virtio console, retains its own GBM device.

`VIBESHINE_KWIN_RENDER_PCI` selects **virtual-output rendering**. Vibeshine's
`adapter_name` selects **capture/encoding** and must not implicitly change the
compositor's seat primary. To use the same GPU for both, resolve the chosen
render node's `/sys/class/drm/renderD*/device` to its PCI identity and configure
the virtual renderer deliberately. Raw `renderD*` numbering can change after
GPU or VM display-device changes; the bridge rechecks PCI identity after open
and refuses a different device. It does not read service-private machine state
or mutate a running compositor's environment.

Invalid, missing or ambiguous renderer selection leaves the virtual node's
original GBM path intact; physical outputs are unaffected. Such a fallback does
not establish that private output publication or streaming will work. Likewise,
a successful association log establishes only the allocator route: validate
console output, KScreen hotplug, client frames and requested cadence on the
actual GPU topology. The bridge is built from the pinned libvirtualdisplay
source as part of the native Linux package; updating only source without the
packaged library does not update a running compositor.
