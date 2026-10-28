#!/usr/bin/sh

echo 1 > /sys/class/graphics/fb0/blank
echo 1 > /sys/class/graphics/fb1/blank
echo 0 > /sys/class/video/disable_video
python reset_hid.py