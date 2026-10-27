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

**Update for current images:** devmfc's kernels now carry a built-in
MAE0621A driver, so on boots where the chip reports its real ID the link
comes up by itself. The problem this repo exists for is the *other* boot —
when the chip misreports, the built-in driver does not match and the kernel
falls back to the generic driver, producing the DMA-reset failure above
(exactly what was observed before the out-of-tree module was added). See
[Which driver binds](#which-driver-binds).

## The fix

Three parts, all included here:

1. **`meson-sc2-tox3-gbit.dtb`** — a corrected device tree (originally
   built by [AbraxasDMG](https://github.com/devmfc/debian-on-amlogic/discussions/25))
   that properly describes the PHY node with reset GPIOs and IRQ.
   **`meson-sc2-tox3-gbit.dts`** is that blob's source, recovered by
   decompiling it — see [Rebuilding the DTB](#rebuilding-the-dtb).
2. **The MAE0621A driver that devmfc's kernel already ships built in.**
   Current devmfc kernels compile `drivers/net/phy/maxio` into the kernel
   (it registers as `MAE0621A-Q2C` / `MAE0621A!B-Q3C(I)` and prints
   `MAXIO_PHY_VER: v1.8.1.4`). Whenever the chip reports its real PHY ID,
   that driver binds by itself and no module is needed at all — see
   [Which driver binds](#which-driver-binds).
3. **`maxio.c`** — an out-of-tree PHY driver (based on the one shipped in
   [CoreELEC's linux-amlogic](https://github.com/CoreELEC/linux-amlogic/blob/5.15.153_202501/drivers/net/phy/maxio.c)),
   kept as the **fallback** for boots where the chip misreports its ID and
   the built-in driver therefore does not match. Patched to build against
   modern kernels (6.18+): removed `eee_broken_modes` (dropped from
   `struct phy_device`) and updated `match_phy_device`'s signature for the
   current `phy_driver` API. It is installed but not force-loaded;
   [`maxio-rescue.service`](maxio-rescue.service) loads it only when `eth0`
   comes up without a carrier.

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
> That said, this is a small out-of-tree kernel driver
> put together by one user for one specific box, not a reviewed upstream
> submission — there may be edge cases or bugs. Changes that alter timing
> (RGMII delay clearing, PHY reset polling) are deliberately left out until
> someone can validate them on hardware. Issues and PRs welcome.

## Requirements

- A TOX3 box running devmfc/debian-on-amlogic, kernel 6.18.x
- The matching `linux-headers-<kernel-version>` package installed
  (download from the [devmfc releases page](https://github.com/devmfc/debian-on-amlogic/releases)
  and `dpkg -i` it before running the script)
- **Last known to work on devmfc kernel `6.18.59-meson64`** (build `20261026`).
  On newer images the built-in driver is normally sufficient on its own, so
  this script and the module are likely not needed there at all — they are
  kept as the fallback for the misreported-ID case, see
  [Which driver binds](#which-driver-binds).

## Which driver binds

The chip does not report its ID consistently: on some boots MDIO returns the
real `0x7b744411`, on others the broken `0xffff4411` that started this whole
project. Two drivers compete for the PHY and the first match wins:

| PHY ID read at boot | driver that binds | result |
|---|---|---|
| `0x7b744411` | devmfc's built-in driver (registered first) | link up, **no module loaded** |
| `0xffff4411` | only `maxio.c` matches, through its fallback | link up, fallback module loaded |

So on a healthy boot the out-of-tree module simply sits installed and
unloaded (and the kernel is not tainted by it), and it earns its keep only
on the boots where the built-in driver cannot match. That is why the module
is kept rather than deleted.

`maxio-rescue.service` automates the hand-over: at boot it waits up to 30 s
for the `eth0` carrier and, if the carrier never appears, loads the module
and bounces `eth0` so the PHY is re-probed. It prints `eth0 carrier present -
built-in MAE0621A driver is working` and exits when nothing is wrong.

## Install

```bash
# 1. Install matching kernel headers (skip if already installed)
dpkg -i linux-headers-<your-kernel-version>_arm64.deb

# 2. Copy the DTB into place for your kernel version
cp meson-sc2-tox3-gbit.dtb /boot/dtb-<your-kernel-version>/amlogic/

# 3. Point boot.config at it (the sed only rewrites an existing box=
#    line, so check the result)
sed -i 's/^box=.*/box=tox3_gigabit/' /boot/boot.config
grep '^box=' /boot/boot.config     # must print: box=tox3_gigabit
# (create /boot/box-config/box-tox3_gigabit.config if it doesn't already
#  exist on your image — see box-tox3_gigabit.config in this repo)

# 4. Put the driver source where the script expects it
mkdir -p /usr/src/maxio-1.0
cp maxio.c dkms.conf Makefile /usr/src/maxio-1.0/
# keep maxio-rescue.sh and maxio-rescue.service in the same directory as
# install-maxio.sh — the script installs them as the systemd unit

# 5. Reboot onto the new DTB, then run the install script
reboot
# after reboot:
chmod +x install-maxio.sh
sudo ./install-maxio.sh
```

The script is idempotent — safe to re-run any time. It builds and installs
the DKMS module and the rescue unit, but deliberately **does not load the
module**: the built-in driver owns the link, the module is only staged as a
fallback. It must be re-run after every kernel update (new
`linux-image`/`linux-headers` from devmfc), since the DKMS module is tied to
the exact kernel version.

## After a kernel update

Order matters: the `linux-image` postinst runs DKMS autoinstall and fails on
the missing `modpost` in devmfc's headers (leaving the package half-configured,
which in turn blocks `apt`). The script fixes the headers for the new kernel,
so run it before finishing the package configuration:

```bash
# 1. Install the new kernel + headers (step 1 may report an error — see above)
dpkg -i linux-image-<new-version>_arm64.deb linux-headers-<new-version>_arm64.deb

# 2. Heal the new headers and build the fallback module for the new kernel
KVER=<new-version>-meson64 sudo -E ./install-maxio.sh

# 3. Let dpkg finish configuring the image package
sudo dpkg --configure -a

# 4. Copy the DTB into the new kernel's directory
cp /boot/dtb-<old-version>/amlogic/meson-sc2-tox3-gbit.dtb \
   /boot/dtb-<new-version>/amlogic/

# 5. Reboot onto the new kernel
reboot
```

The script removes the DKMS build for the *previous* kernel on the way, so
if you still need to boot that one, rebuild it with
`KVER=<old-version>-meson64 sudo -E ./install-maxio.sh`.

## Verifying it worked

```bash
dmesg | grep -i mae0621
# normal boot — built-in driver, no module needed:
#   meson8b-dwmac fdc00000.ethernet eth0: PHY [mdio_mux-0.0:00] driver [MAE0621A-Q2C Gigabit Ethernet] (irq=POLL)
# fallback boot — chip misreported its ID, our module took over:
#   meson8b-dwmac fdc00000.ethernet eth0: PHY [mdio_mux-0.0:00] driver [MAE0621A Gigabit Ethernet] (irq=POLL)

journalctl -u maxio-rescue.service -b --no-pager --output cat
# eth0 carrier present - built-in MAE0621A driver is working

ip addr show eth0
ethtool eth0 | grep -i "speed\|link"
# Speed: 1000Mb/s
# Link detected: yes
```

Both driver names above are correct outcomes — what matters is `1000Mb/s`
and a stable link.

> **Gotcha:** `modinfo maxio` (by name) reports `description: Maxio PHY driver`
> — that is devmfc's **built-in** driver from `modules.builtin.modinfo`, not
> this out-of-tree module. For the same reason `modprobe -r maxio` refuses to
> unload anything with "Module maxio is builtin". To inspect the module you
> installed, pass the full path:
> `modinfo /lib/modules/$(uname -r)/updates/dkms/maxio.ko`.

## Rebuilding the DTB

There is no upstream `.dts` for this box, so `meson-sc2-tox3-gbit.dts` was
recovered by decompiling the shipped `.dtb`. The conversion was checked to
be lossless against an independent device-tree parser, but the `.dtb` in
this repo stays the authoritative artifact until a rebuilt blob has passed
a boot test on real hardware:

```bash
dtc -I dts -O dtb -o meson-sc2-tox3-gbit.dtb meson-sc2-tox3-gbit.dts
```

One detail worth knowing before editing: the `ethernet-phy@0` node in the
current blob carries **no** `compatible` string. Binding therefore happens
purely on the PHY ID: the built-in driver matches the real `0x7b744411`,
while `maxio.c` also accepts the broken `0xffff4411` — the fallback this
project exists for. Adding `compatible = "ethernet-phy-id7b74.4411"` would
let that fallback be narrowed, but only after a boot test.

## Credits

- DTB and initial CoreELEC-driver-based build: [AbraxasDMG](https://github.com/devmfc/debian-on-amlogic/discussions/25)
- Original Maxio PHY driver: [Stane1983 / CoreELEC](https://github.com/CoreELEC/linux-amlogic/pull/15)
- Kernel build system: [devmfc/debian-on-amlogic](https://github.com/devmfc/debian-on-amlogic)

## License

GPL-2.0-only — see [LICENSE](LICENSE). `maxio.c` is a derivative of GPL-2.0
Linux kernel code.
