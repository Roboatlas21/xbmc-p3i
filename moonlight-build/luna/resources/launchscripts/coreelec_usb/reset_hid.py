import os

devices = os.listdir('/sys/bus/usb/devices/')
for a in devices:
    os.system(f"echo {a} > /sys/bus/usb/drivers/usb/unbind")
    os.system(f"echo {a} > /sys/bus/usb/drivers/usb/bind")
    except:
        continue