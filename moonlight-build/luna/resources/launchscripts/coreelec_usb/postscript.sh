#!/usr/bin/sh

echo 0 > /sys/class/graphics/fb0/blank
echo 0 > /sys/class/graphics/fb1/blank
python reset_hid.py