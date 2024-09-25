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
 -a --ahi         Draw graphic AHI, mode [0-No, 2-Simple 1-Ladder, 3-LadderEx (home indicator on ladder)]
 -x --matrix      OSD matrix [0- 53:20 , 1- 50:18 chars, 11 Variable font size]
 -v --verbose     Show debug info
 --help           Display this help
```

**Support for two font sizes.** (on FullHD mode only!)  
set --matrix  to a value 11 or higher, each value represents a template to be used to map the OSD config.  
```--matrix 11``` will use template 1. (click for a video sample)  
<a href="https://youtu.be/uKa1P8-Soxw">
    <img src="pics/OSD_Variable_font.jpg" alt="Video sample" width="300"/>
</a>  
All the OSD symbols in the red rectangles will be rendered using smaller font size. They will be aligned to the outer corner of each rectangle (up-left for the upper left one).


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
 -&p - Dropped packet injections by wfb-ng
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
**For SigmaStar** based SoC (ssc338q, sc30kq) :
```
curl -L -o /usr/bin/msposd https://raw.githubusercontent.com/openipc/msposd/main/release/star6e/msposd
chmod 755 /usr/bin/msposd
```
Copy the font files for your flight controller firmware INAV/ Betaflight/ ArduPilot from here  https://github.com/openipc/msposd/tree/main/fonts   
Choose one of the following below.   
**For INAV**:
```
curl -L -o /usr/bin/font.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/inav/font.png
curl -L -o /usr/bin/font_hd.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/inav/font_hd.png
```

**For Betaflight**:
```
curl -L -o /usr/bin/font.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/betaflight/font.png
curl -L -o /usr/bin/font_hd.png https://raw.githubusercontent.com/openipc/msposd/main/fonts/betaflight/font_hd.png
```

Start msposd or reference it in OpenIPC boot scripts.  

### Diagnosing
Q: _I see a static map of all characters on the screen but no realtime OSD_.  
A: There are no data being received by the cam.  
    - Check your wiring. You need Tx/Rx/Gnd wires.  Data lines must be crossed (Rx goes to Tx ).  
    - Check Inav/Ardu config. Enable MSP and OSD where needed! There is a separate option for OSD in INAV. Do not enable mavlink and msp on a single UART. (This is a bug in BF). Do not start msposd twice.  
    - On the cam. execute ```top``` , find the line for msposd, copy it (the whole line!) . Then ```killall msposd```, add   ```-v``` to the end of the line and start it.  
    Take a look at the console logs:  

<a href="pics/diag_1.png">
  <img src="pics/diag_1.png" alt="Diagram Thumbnail" width="350"/>
</a>  

If they are stuck at step 1, there is no data on te UART. If there is data past step 1, but OSD is not visible, the data is not the format expected - MSP Display Port.

Q: _OSD changes, but I see strange symbols on the screen_.  
A: Download the appropriate font set for you flight controller software. Check for loose connectors, speed settings and make sure you have ground wire between the FC and the cam.

Q: _AHI is shown but is not updated, even though msposd has "--ahi 1" argument. Telemetry is working._
A: Check your connections, specifically Camera's TX <-> FC RX. Camera needs to request extra data from Flight controller for this feature to work.

### Acknowledgements:
- Default fonts included in this project are created by SNEAKY_FPV.  These and other excellent additional fonts can be found here: https://sites.google.com/view/sneaky-fpv/home  
This work is based on these projects:
- https://github.com/fpv-wtf/msp-osd
- https://github.com/OpenIPC/osd
