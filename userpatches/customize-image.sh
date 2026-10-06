#!/bin/bash
# NanoAir image customization. Runs inside the target-image chroot.
# Args: RELEASE LINUXFAMILY BOARD BUILD_DESKTOP ARCH
set -e
RELEASE="$1"; LINUXFAMILY="$2"; BOARD="$3"; BUILD_DESKTOP="$4"; ARCH="$5"

[ "$BOARD" = nanopiair ] || exit 0

export DEBIAN_FRONTEND=noninteractive

# 1) overlay files into place (userpatches/overlay bind-mounted at /tmp/overlay;
#    the "build" subdir holds sources compiled below and is excluded here)
rsync -a --exclude=build /tmp/overlay/ /
chmod 755 /usr/local/sbin/usb-gadget.sh

# 2) runtime packages: alsaloop (USB-mic bridge) + adbd (Debian package)
apt-get -qq update
apt-get -y install --no-install-recommends alsa-utils adbd

# 3) build + install the ALSA tag-decode ioplug (4ch/48k|16k capture from
#    the micgen wire stream). Sources are read-only under /tmp/overlay.
apt-get -y install --no-install-recommends gcc make pkg-config libasound2-dev
cp -r /tmp/overlay/build /tmp/ac108-plugin
make -C /tmp/ac108-plugin -f plugin-Makefile
install -m 644 /tmp/ac108-plugin/libasound_module_pcm_ac108.so \
    /usr/lib/$(gcc --print-multiarch)/alsa-lib/
apt-get -y purge gcc make pkg-config libasound2-dev
apt-get -y autoremove

# 4) services: gadget (adb + UAC2) and the ac108->UAC2 bridge.
#    adbd itself is started by usb-gadget.sh once ffs is up (drop-in in overlay).
systemctl enable usb-gadget.service usb-audio-bridge.service

# 5) user + passwordless sudo (adb root shell is the primary access path)
useradd -m -s /bin/bash i 2>/dev/null || true
echo "i ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/i
chmod 440 /etc/sudoers.d/i

# 6) hold kernel packages: an apt upgrade would replace the DT and drop the
#    in-tree ac108_init module. Upgrades happen by rebuilding this image.
apt-mark hold linux-image-current-sunxi linux-dtb-current-sunxi 2>/dev/null || true

echo "nanoair: customize-image done"
