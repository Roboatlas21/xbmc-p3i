#!/usr/bin/sh

echo 1 > /sys/class/graphics/fb0/blank
echo 1 > /sys/class/graphics/fb1/blank
echo 0 > /sys/class/video/disable_video

for X in /sys/bus/usb/devices/*
do
    # echo "Discovered: $X"
    if [ -e "$X/idVendor" ] && [ -e "$X/idProduct" ]
    then
        echo "Resetting device: $(cat "$X/idVendor") : $(cat "$X/idProduct")"
        echo 0 >"$X/authorized"
        echo 1 >"$X/authorized"
    fi
done