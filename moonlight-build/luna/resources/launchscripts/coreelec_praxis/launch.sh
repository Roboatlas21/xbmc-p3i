#!/usr/bin/bash

fixes(){
        sleep 2
        echo 1 > /sys/class/graphics/fb0/blank
        echo 0 > /sys/class/video/disable_video

        for X in /sys/bus/usb/devices/*
                do
                # echo “Discovered: $X”
                if [ -e “$X/idVendor” ] && [ -e “$X/idProduct” ]
                        then
                        echo “Resetting device: $(cat “$X/idVendor”) : $(cat “$X/idProduct”)”
                        echo 0 >“$X/authorized”
                        echo 1 >“$X/authorized”
                fi
                done
}

killall -STOP kodi.bin
fixes &
mv logfile logfile.old
moonlight stream -app "$@" >> logfile 2>&1
killall -CONT kodi.bin