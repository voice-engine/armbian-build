#!/bin/sh
# Composite USB gadget for the NanoPi NEO Air (Allwinner H3, musb-hdrc UDC).
#   ffs.adb  : FunctionFS for adbd -> "adb devices" on the PC
#   uac2.usb0: UAC2 audio function -> 4ch/48k/S32_LE USB microphone for the
#              PC (fed from the ac108 array by usb-audio-bridge.service)
# NOTE: never unbind/rebind the musb-sunxi driver on this kernel
# (oops/hang); this script only ever touches configfs.

GADGET=/sys/kernel/config/usb_gadget/g1
FFS=/dev/usb-ffs/adb

start_gadget() {
    modprobe libcomposite 2>/dev/null || true
    modprobe usb_f_uac2 2>/dev/null || true
    # usb_f_fs is built-in on this kernel (CONFIG_USB_CONFIGFS_F_FS=y)
    if ! mountpoint -q /sys/kernel/config; then
        mount -t configfs configfs /sys/kernel/config
    fi
    if [ -d "$GADGET" ]; then
        return 0
    fi

    udc=""
    i=0
    while [ "$i" -lt 15 ]; do
        udc=$(ls /sys/class/udc 2>/dev/null | head -n 1)
        [ -n "$udc" ] && break
        sleep 1
        i=$((i + 1))
    done
    if [ -z "$udc" ]; then
        echo "usb-gadget: no UDC found" >&2
        exit 1
    fi

    mkdir -p "$GADGET"
    cd "$GADGET" || exit 1
    echo 0x18d1 > idVendor  # Google (matches android udev rules)
    echo 0x0100 > idProduct

    mkdir -p strings/0x409
    echo "Armbian"        > strings/0x409/manufacturer
    echo "NanoPi NEO Air" > strings/0x409/product

    mkdir -p configs/g1.1/strings/0x409
    echo "adb + UAC2 audio" > configs/g1.1/strings/0x409/configuration

    mkdir -p functions/ffs.adb functions/uac2.usb0

    # UAC2: capture-only 4-channel microphone, 48 kHz / S32_LE, no
    # playback endpoint, no mixer controls (keeps host side simple).
    ( cd functions/uac2.usb0
      echo 0xF    > c_chmask
      echo 48000  > c_srate
      echo 4      > c_ssize
      echo 0      > c_mute_present
      echo 0      > c_volume_present
      echo 0      > p_chmask
      echo 16     > req_number )

    ln -s functions/ffs.adb   configs/g1.1/ffs.adb
    ln -s functions/uac2.usb0 configs/g1.1/uac2.usb0

    # functionfs mount must exist before adbd starts (it writes the USB
    # descriptors through ep0)
    mkdir -p "$FFS"
    mountpoint -q "$FFS" || mount -t functionfs adb "$FFS"

    # Debian's adbd; its own gadget hooks are disabled via drop-in, the
    # lifecycle is fully ours. Started before the UDC bind so descriptors
    # land before the host enumerates.
    systemctl start --no-block adbd.service 2>/dev/null || true

    i=0
    while [ "$i" -lt 15 ] && [ ! -e "$FFS/ep1" ]; do
        sleep 1
        i=$((i + 1))
    done
    if [ ! -e "$FFS/ep1" ]; then
        echo "usb-gadget: adbd did not create ffs endpoints" >&2
    fi

    echo "$udc" > UDC
}

stop_gadget() {
    if [ ! -d "$GADGET" ]; then
        return 0
    fi
    systemctl stop adbd.service 2>/dev/null || true
    cd "$GADGET" || return 0
    echo "" > UDC 2>/dev/null || true
    rm -f configs/g1.1/ffs.adb configs/g1.1/uac2.usb0 2>/dev/null || true
    umount "$FFS" 2>/dev/null || true
    rmdir "$FFS" 2>/dev/null || true
    rmdir functions/ffs.adb functions/uac2.usb0 2>/dev/null || true
    rmdir configs/g1.1/strings/0x409 configs/g1.1 2>/dev/null || true
    rmdir strings/0x409 2>/dev/null || true
    cd /
    rmdir "$GADGET" 2>/dev/null || true
}

case "$1" in
    start) start_gadget ;;
    stop)  stop_gadget ;;
    *)     echo "usage: $0 {start|stop}" >&2; exit 1 ;;
esac
