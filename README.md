# tox3-ethernet

Fix for non-working gigabit Ethernet on **TOX3** Amlogic SC2 (S905X4-class)
TV boxes running [devmfc/debian-on-amlogic](https://github.com/devmfc/debian-on-amlogic).

## The problem

TOX3 boxes are recognized by devmfc as `vontar,x4` and boot using
`meson-sc2-vontarx4-gbit.dtb`. That DTB has no `compatible` string for the
onboard PHY, so the kernel falls back to the generic PHY driver — which
fails DMA initialization entirely:

```
meson8b-dwmac fdc00000.ethernet eth0: Failed to reset the dma
meson8b-dwmac fdc00000.ethernet eth0: stmmac_hw_setup: DMA engine initialization failed
```

The actual PHY chip on TOX3 is a **Maxio MAE0621A**, not the Realtek
RTL8211F the board silkscreen might suggest. It also misreports its own ID
over MDIO (`0xffff4411` instead of the real `0x7b744411`), so even with the
right `compatible` string, mainline Linux still can't match it — there is
no upstream driver for this PHY (see [this LKML thread](https://lkml.rescloud.iu.edu/2505.0/03415.html)
where an Amlogic kernel developer confirms mainline has no driver for it).

## The fix

Two parts, both included here:

1. **`meson-sc2-tox3-gbit.dtb`** — a corrected device tree (originally
   built by [AbraxasDMG](https://github.com/devmfc/debian-on-amlogic/discussions/25))
   that properly describes the PHY node with reset GPIOs and IRQ.
2. **`maxio.c`** — an out-of-tree PHY driver (based on the one shipped in
   [CoreELEC's linux-amlogic](https://github.com/CoreELEC/linux-amlogic/blob/5.15.153_202501/drivers/net/phy/maxio.c)),
   patched to build against modern kernels (6.18+): removed
   `eee_broken_modes` (dropped from `struct phy_device`) and updated
   `match_phy_device`'s signature for the current `phy_driver` API.

Building this against devmfc's `linux-headers` package hits a chain of
missing build machinery — no `scripts/mod/modpost`, no
`arch/arm64/tools/gen-cpucaps.awk`, a `cpucaps` list missing the
`ARM64_WORKAROUND_4193714` entry devmfc's kernel actually has, and a
`UTS_RELEASE` in the headers package that's missing the `-meson64` suffix
(which silently produces a module with the wrong vermagic). The
[`install-maxio.sh`](install-maxio.sh) script works around all of this.

> **Note:** the `maxio.c` patches and `install-maxio.sh` were put together
> with AI assistance (debugging the build chain step by step, patching the
> driver against the current kernel API), then verified working end-to-end
> on a real TOX3 box (clean rebuild + reboot + automatic interface bring-up).
> That said, this is a small out-of-tree kernel driver put together by one
> user for one specific box, not a reviewed upstream submission — there may
> be edge cases or bugs. Issues and PRs welcome.

## Requirements

- A TOX3 box running devmfc/debian-on-amlogic, kernel 6.18.x
- The matching `linux-headers-<kernel-version>` package installed
  (download from the [devmfc releases page](https://github.com/devmfc/debian-on-amlogic/releases)
  and `dpkg -i` it before running the script)

## Install

```bash
# 1. Install matching kernel headers (skip if already installed)
dpkg -i linux-headers-<your-kernel-version>_arm64.deb

# 2. Copy the DTB into place for your kernel version
cp meson-sc2-tox3-gbit.dtb /boot/dtb-<your-kernel-version>/amlogic/

# 3. Point boot.config at it
sed -i 's/^box=.*/box=tox3_gigabit/' /boot/boot.config
# (create /boot/box-config/box-tox3_gigabit.config if it doesn't already
#  exist on your image — see box-tox3_gigabit.config in this repo)

# 4. Put the driver source where the script expects it
mkdir -p /usr/src/maxio-1.0
cp maxio.c dkms.conf Makefile /usr/src/maxio-1.0/

# 5. Reboot onto the new DTB, then run the install script
reboot
# after reboot:
chmod +x install-maxio.sh
./install-maxio.sh
```

The script is idempotent — safe to re-run any time, and **must be re-run
after every kernel update** (new `linux-image`/`linux-headers` from
devmfc), since the DKMS module is tied to the exact kernel version.

## After a kernel update

```bash
dpkg -i linux-image-<new-version>_arm64.deb
dpkg -i linux-headers-<new-version>_arm64.deb
cp /boot/dtb-<old-version>/amlogic/meson-sc2-tox3-gbit.dtb /boot/dtb-<new-version>/amlogic/
reboot
./install-maxio.sh
```

## Verifying it worked

```bash
dmesg | grep -i mae0621
# meson8b-dwmac fdc00000.ethernet eth0: PHY [mdio_mux-0.0:00] driver [MAE0621A Gigabit Ethernet] (irq=POLL)

ip addr show eth0
ethtool eth0 | grep -i "speed\|link"
# Speed: 1000Mb/s
# Link detected: yes
```

## Credits

- DTB and initial CoreELEC-driver-based build: [AbraxasDMG](https://github.com/devmfc/debian-on-amlogic/discussions/25)
- Original Maxio PHY driver: [Stane1983 / CoreELEC](https://github.com/CoreELEC/linux-amlogic/pull/15)
- Kernel build system: [devmfc/debian-on-amlogic](https://github.com/devmfc/debian-on-amlogic)

## License

GPL-2.0 — see [LICENSE](LICENSE). `maxio.c` is a derivative of GPL-2.0
Linux kernel code.
