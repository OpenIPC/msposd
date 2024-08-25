 # MSPOSD
## A tool for drawing betaflight/inav/ardupilot MSP Display Port OSD over OpenIPC video stream.

**Click the image below to watch a video sample:**
[![Video sample](pics/AHI_OSD.png)](https://www.youtube.com/watch?v=4907k5c7b4U)

```
Usage: msposd [OPTIONS]
 -m --master      Serial port to receive MSP (%s by default)
 -b --baudrate    Serial port baudrate (%d by default)
 -o --output	  UDP endpoint to forward aggregated MSP messages
 -c --channels    RC Channel to listen for commands (0 by default) and exec channels.sh
 -w --wait        Delay after each command received (2000ms default)
 -r --fps         Max MSP Display refresh rate(1..50)
 -p --persist     How long a channel value must persist to generate a command - for multiposition switches (0ms default)
 -t --temp        Read SoC temperature
 -d --wfb         Monitors wfb.log file and reports errors via HUD messages
 -s --osd         Parse MSP and draw OSD over the video
 -a --ahi         Draw graphic AHI, mode [0-No, 2-Simple 1-Ladder, 3-LadderEx]
 -v --verbose     Show debug info
 --help           Display this help
```

### Additional options.
Forwarding of MSP packets via UDP.  
Can monitor RC Channels values in FC and call the script `channels.sh` (located at /usr/bin or /usr/sbin).Will passing the channel number and its value to it as $1 and $2 parameters. This allows for controlling the camera via the Remote Control Transmitter.  
AHI (Artificial Horizon Indicator) ladder - Graphical AHI , that is drawn over the standard OSD.  

This work is based on these exceptional projects: https://github.com/fpv-wtf/msp-osd and https://github.com/OpenIPC/osd

## Example :

```
msposd  --master /dev/ttyS2 --baudrate 115200 --channels 7 --out 127.0.0.1:14555 -osd -r 20 --ahi 1 -v
```
Read on  UART2 with baudrade 115200 and listen for value changes of RC channel 7 that come from the Remote Control via Flight Controller.
Every time the value is changed with more than 5% the bash script ```channels.sh {Channel} {Value}``` will be started with the provided parameters.  
Forward MSP to UDP port 14555 so that it can be handled by wfb-ng and sent to the ground.
Draw an Artificial Horizon Indicator (AHI) Ladder with color-coded vertical steps.
The refresh rate of the OSD is limited to 20 frames per second, depending on the Flight Controller and MSP DisplayPort implementation, usually ranging between 12 and 17 frames per second.

Font files for each Flight Controller firmware have two versions—one for 720p and one for 1080p resolutions. They should be named **font_hd.png** and **font.png** respectively, and saved in the **/usr/bin** folder on the camera.
They vary depending on the Flight Controller, so choose the appropriate pair.
The program will read from /etc/majestic.yaml and will select the type of font to use based on the video resolution configured there.

## To install:
Copy msposd for the architecture you need on the cam.  
Prebuild binaries for x86 and SigmaStar are at release/ folder.  
```
curl -L -o msposd https://raw.githubusercontent.com/tipoman9/msposd/MSP_OSD/release/star6e/msposd
chmod 755 /usr/bin/msposd
```
Don't forget to copy the font files for you flight controller firmware!  https://github.com/tipoman9/msposd/tree/MSP_OSD/fonts  
For INAV these would be:
```
curl -L -o font.png https://raw.githubusercontent.com/tipoman9/msposd/MSP_OSD/fonts/inav/font.png
curl -L -o font_hd.png https://raw.githubusercontent.com/tipoman9/msposd/MSP_OSD/fonts/inav/font_hd.png
```
Start msposd or reference it in OpenIPC scripts.  
Excellent additional fonts can be found here: https://sites.google.com/view/sneaky-fpv/home 

