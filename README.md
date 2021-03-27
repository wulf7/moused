# moused
Moused with EVDEV support (WIP)

## System requirements

* FreeBSD 12.1+
* iichid/usbhid installed from ports or from base system (13+)
* usbhid activated through /boot/loader.conf with following lines:

```
hw.usb.usbhid.enable=1
usbhid_load="YES"
```

## Downloading

This project does not have a special home page. The source code and the
issue tracker are hosted on the Github:

https://github.com/wulf7/moused

## Building

To build driver, cd in to extracted archive directory and type

```
$ make
```

## Installing

To install file already built just type:

```
$ sudo make install
```

and you will get the compiled moused installed in **/usr/local/sbin**.
You may need to restart devd to apply bundled devd rules.

To run **moused** at a boot time, disable system moused in **/etc/rc.conf**
with commenting out of appropriate line and add following lines to
**/etc/rc.local**

```
vidcontrol -m on
for file in /dev/input/event[0-9]*; do
        /usr/local/sbin/moused -p $file -I /var/run/moused.`echo $file | cut -c 12-`.pid
done
```

To prevent double movement on bluetooth devices, following patch should be
applied to bthidd:
```
diff --git a/usr.sbin/bluetooth/bthidd/hid.c b/usr.sbin/bluetooth/bthidd/hid.c
--- usr.sbin/bluetooth/bthidd/hid.c
+++ usr.sbin/bluetooth/bthidd/hid.c
@@ -560,12 +560,12 @@ hid_interrupt(bthid_session_p s, uint8_t *data, int32_t len)
 		mi.u.data.y = mouse_y;
 		mi.u.data.z = mouse_z;
 		mi.u.data.buttons = mouse_butt;
-
+/*
 		if (ioctl(s->srv->cons, CONS_MOUSECTL, &mi) < 0)
 			syslog(LOG_ERR, "Could not process mouse events from " \
 				"%s. %s (%d)", bt_ntoa(&s->bdaddr, NULL),
 				strerror(errno), errno);
-
+*/
 		if (hid_device->mouse && s->srv->uinput &&
 		    uinput_rep_mouse(s->umouse, mouse_x, mouse_y, mouse_z,
 					mouse_t, mouse_butt, s->obutt) < 0)
```
