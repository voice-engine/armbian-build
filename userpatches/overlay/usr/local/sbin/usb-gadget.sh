#!/bin/sh
# Composite USB gadget for the NanoPi NEO Air (Allwinner H3, musb-hdrc UDC).
#  - ffs.adb  : FunctionFS for adbd -> adb over USB
#  - uac2.usb0: USB Audio Class 2 -> 4ch/48k/S32 USB 麦克风（配 usb-audio-bridge 桥接 ac108）
# 可选回滚: USE_ACM=1 恢复旧的 CDC 串口控制台（acm.GS0 -> /dev/ttyGS0）
# NOTE: never unbind/rebind musb-sunxi on this kernel (oops/hang); this script
# only ever touches configfs.
GADGET=/sys/kernel/config/usb_gadget/g1
FFS=/dev/usb-ffs/adb

start_gadget() {
    modprobe libcomposite 2>/dev/null || true
    modprobe usb_f_uac2 2>/dev/null || true
    if [ "$USE_ACM" = "1" ]; then
        modprobe usb_f_acm 2>/dev/null || true
    fi
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
    echo "adb + UAC2 (4ch mic)" > configs/g1.1/strings/0x409/configuration

    # --- UAC2: 4ch USB 麦克风（设备侧 playback = 主机录到的数据）---
    # musb 限制: 与其他功能共存时 iso maxpacket <=512B（字母序 acm/ffs 先绑走大端点）
    #  -> 4ch*S32*48k=768B 不可行; 4ch*S16*48k=384B / *16k=128B 可行
    mkdir -p functions/uac2.usb0
    # 方向映射(实测): p_* = 主机录制(USB 麦克风)方向, 设备侧从 UAC2Gadget 的
    # playback PCM 喂数据; c_* = 主机播放->设备。故: p_=4ch 麦克风, c_=关闭。
    ( cd functions/uac2.usb0 || exit 1
      echo 0xF         > p_chmask         # 4 通道 USB 麦克风
      echo 48000,16000 > p_srate          # 双速率（桥接默认 48k）
      echo 2           > p_ssize          # S16_LE（musb 512B 限制, S32@48k 不可行）
      echo 0           > p_mute_present
      echo 0           > p_volume_present
      echo 0           > c_chmask         # 不做主机->设备回放
      echo 16          > req_number )
    ln -s functions/uac2.usb0 configs/g1.1/uac2.usb0

    mkdir -p functions/ffs.adb
    ln -s functions/ffs.adb configs/g1.1/ffs.adb

    if [ "$USE_ACM" = "1" ]; then
        mkdir -p functions/acm.GS0
        ln -s functions/acm.GS0 configs/g1.1/acm.GS0
    fi

    # functionfs mount must exist before adbd starts (it writes the USB
    # descriptors through ep0)
    mkdir -p "$FFS"
    mountpoint -q "$FFS" || mount -t functionfs adb "$FFS"

    # Debian's adbd; its own gadget hooks are disabled via drop-in, the
    # lifecycle is fully ours. Started before the UDC bind so descriptors land
    # before the host enumerates.
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
    rm -f configs/g1.1/acm.GS0 configs/g1.1/ffs.adb configs/g1.1/uac2.usb0 2>/dev/null || true
    umount "$FFS" 2>/dev/null || true
    rmdir "$FFS" 2>/dev/null || true
    rmdir functions/acm.GS0 functions/ffs.adb functions/uac2.usb0 2>/dev/null || true
    rmdir configs/g1.1/strings/0x409 configs/g1.1 2>/dev/null || true
    rmdir strings/0x409 2>/dev/null || true
    cd /
    rmdir "$GADGET" 2>/dev/null || true
}

case "$1" in
    start) start_gadget ;;
    stop)  stop_gadget ;;
    *)     echo "usage: $0 {start|stop} [USE_ACM=1]" >&2; exit 1 ;;
esac
