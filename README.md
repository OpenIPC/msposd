## MSPOSD

### A tool for drawing betaflight/inav/ardupilot MSP Display Port OSD over OpenIPC video stream.

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
 -x --matrix      OSD matrix [0- 53:20 , 1- 50:18 chars]
 -v --verbose     Show debug info
 --help           Display this help
```

###  Options.
Forwarding of MSP packets via UDP.  
Can monitor RC Channels values in FC and call the script `channels.sh` (located at /usr/bin or /usr/sbin).Will passing the channel number and its value to it as $1 and $2 parameters. This allows for controlling the camera via the Remote Control Transmitter.  
AHI (Artificial Horizon Indicator) ladder - Graphical AHI , that is drawn over the standard OSD.  
**Show custom mesage and diagnostics** on screen when text is written to file /tmp/MSPOSD.msg 
```
echo "Custom Message... &L04 &F22 CPU:&C &B temp:&T" >/tmp/MSPOSD.msg
```
Extra params withing the message to be shown:
- &T - Board temperature  
- &B - Video Bitrate and FPS  
- &C - CPU Usage percent  
- &t - Time  
- &Fxx - Set text font size (10 to 99)  
- &Lxx - Set text colour (first digit 0- white, 1 - black, 2- red, 3 - green, 4 - blue, 5 - yellow, 6 - magenta, 7 - cyan) and postion on the screen(second digit)  0-TopLeft, 1-TopCenter, 2-TopRight, 3-TopMoving,4-BottomLeft, 5-BottomCenter, 6-BottomRight, 7-BottomMoving   
 
### Usage Example:

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

### To install:
Copy msposd for the architecture you need on the cam.  
Prebuild binaries for x86, SigmaStar, Goke and Hisilicon are at release/ folder.  
```
curl -L -o /usr/bin/msposd https://raw.githubusercontent.com/openipc/msposd/main/release/star6e/msposd
chmod 755 /usr/bin/msposd
```
Don't forget to copy the font files for you flight controller firmware!  https://github.com/openipc/msposd/tree/main/fonts  
For INAV these would be:
```
curl -L -o /usr/bin/font.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/inav/font.png
curl -L -o /usr/bin/font_hd.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/inav/font_hd.png
```
Start msposd or reference it in OpenIPC scripts.  

### Acknowledgements:
- Default fonts included in this project are created by SNEAKY_FPV.  These and other excellent additional fonts can be found here: https://sites.google.com/view/sneaky-fpv/home
This work is based on these projects:
- https://github.com/fpv-wtf/msp-osd
- https://github.com/OpenIPC/osd
