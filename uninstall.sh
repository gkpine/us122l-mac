#!/bin/sh
# Removes the TASCAM US-122L driver:  sh uninstall.sh
cd "$(dirname "$0")"
echo "Removing the driver (enter your Mac password when asked)"
sudo sh coreaudio/uninstall.sh
