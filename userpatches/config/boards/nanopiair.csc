# NanoPi Air overrides for the NanoAir custom image.
# This file is sourced AFTER the official config/boards/nanopiair.csc,
# so only the variables that change are defined here.
#
# USB is a configfs composite gadget (adb + UAC2 audio), driven by
# /usr/local/sbin/usb-gadget.sh; g_serial / ttyGS0 are not used.

MODULES=""
SERIALCON="ttyS0"
