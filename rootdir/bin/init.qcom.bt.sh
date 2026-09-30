#!/vendor/bin/sh
# hciattach service body. libbt-vendor sets bluetooth.hciattach=true and polls
# bluetooth.status; hci_qcomm_init configures the WCNSS Bluetooth core over the
# SMD transport and exits 0 on success. LE power class 2 matches the CM13
# default for this device.
setprop bluetooth.status off
if /vendor/bin/hci_qcomm_init -e -P 1 > /dev/null; then
    setprop bluetooth.status on
else
    exit 1
fi
