This is a very simple utility (targetting VS2019) to patch the firmware for Macrosilicon 2109 HDMI to USB capture devices.

These devices have a bug with their audio format descriptor that makes them report the audio as 96000Hz mono instead of
48000Hz stereo. There's been various other workarounds developed for this (there is a linux kernel patch, or a windows 
utility that restreams the audio as stereo) but it's actually possible to patch the firmware to fix it, which is what
this utility does.

Run it from the command prompt, it will try to locate the device to patch it then use pnputil to uninstall the current
drivers. This is required because otherwise windows won't properly recognize that the USB descriptors have changed.
The device will also need to be power-cycled (unplugged/replugged) for the patch to take effect.

The MS2109 still has another bug that causes the stereo channels to be reversed and out-of-phase by one sample; it's
up to the user to figure out how to fix this depending on which app they use.

