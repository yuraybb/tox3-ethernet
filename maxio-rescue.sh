#!/bin/sh
# maxio-rescue.sh — safety net for the TOX3 gigabit fix.
#
# The devmfc kernel ships a built-in Maxio MAE0621A PHY driver that binds
# on its own, so the out-of-tree module is normally NOT loaded. But the chip
# sometimes misreports its PHY ID as 0xffff4411; on those boots the built-in
# driver does not match and eth0 never gets a carrier (the original
# "Failed to reset the dma" problem).
#
# This script waits for the carrier to appear, and only if it never does it
# loads the out-of-tree module and bounces eth0 so the PHY is re-probed.
# In other words: one driver in the normal case, the fallback only when the
# built-in could not do the job.
set -u

MOD="/lib/modules/$(uname -r)/updates/dkms/maxio.ko"
NETDEV=/sys/class/net/eth0

for _ in $(seq 1 30); do
    if [ "$(cat "${NETDEV}/operstate" 2>/dev/null)" = "up" ]; then
        echo "eth0 carrier present - built-in MAE0621A driver is working"
        exit 0
    fi
    sleep 1
done

echo "no carrier on eth0 after 30s - loading rescue module ${MOD}" >&2
if [ ! -f "$MOD" ]; then
    echo "rescue module missing: ${MOD}" >&2
    exit 1
fi
insmod "$MOD" || { echo "insmod ${MOD} failed" >&2; exit 1; }
ip link set eth0 down
sleep 2
ip link set eth0 up
echo "rescue module loaded, eth0 bounced"
