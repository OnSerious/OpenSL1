#!/bin/sh
# Removes the OpenSL1 CoreAudio driver. Usage: sudo ./uninstall.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo: sudo $0" >&2
    exit 1
fi
rm -rf /Library/Audio/Plug-Ins/HAL/OpenSL1.driver
pkgutil --forget org.opensl1.driver >/dev/null 2>&1 || true
killall coreaudiod 2>/dev/null || true
echo "OpenSL1 has been removed."
