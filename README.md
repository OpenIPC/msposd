 # MAVFWD
Added stream parsing with official mavlink library.
mavfwd is a utility for broadcasting a mavlink telemetry stream between wifibroadcast and uart devices, for organizing one- or two-way telemetry between UAV and ground station. mavfwd is included in the OpenIPC FPV firmware as a lighter alternative to mavlink routerd and is usually used on the air part (camera).
Also, mavfwd can monitor the mavlink RC _CHANNELS package, and call the script `channels.sh` (located at /usr/bin or /usr/sbin) passing the channel number and its value to it when changing as $1 and $2 parameters. An example of channels.sh is present.
This allows for controlling the camera via the Remote Control Transmitter
```
Usage: mavfwd [OPTIONS]
-m --master      Local MAVLink master port (%s by default)
-b --baudrate    Serial port baudrate (%d by default)
-o --out         Remote output port (%s by default)
-i --in          Remote input port (%s by default)
-c --channels    RC Channel to listen for commands (0 by default) and call channels.sh
-w --wait        Delay after each command received(2000ms defaulr)
-a --aggregate   Aggregate packets in frames (1 no aggregation, 0 no parsing only raw data forward) (%d by default) 
-f --folder      Folder for file mavlink.msg (default is current folder)
-p --persist     How long a channel value must persist to generate a command - for multiposition switches (0ms default)
-t --temp        Inject SoC temperature into telemetry(HiSilicon and SigmaStart supported)
-j --wfb         Reports wfb_tx dropped packets as Mavlink messages. wfb_tx console must be redirected to <temp>/wfb.log
-s --osd         Parse MSP and draw OSD over the video
-v --verbose     Display each packet, default not       
--help         Display this help
```
Example :

```mavfwd --master /dev/ttyAMA0 --baudrate 115200 --out 192.168.1.20:14550 -c 7 -w 3000 -a 10 -t -f /tmp/```
Will read on the first UART with baudrade 115200 and will listen for values in RC channel 7 that come from the Remote Control via Flight Controller.
Every time the value is changed with more than 5% the bash script ```channels.sh {Channel} {Value}`````` will be started with params.
The script then can check the Value param (usually between 1000 and 2000) and do the tasks needed - reconfigure encoder, switch IR mode, restart WiFi, change MCS index. etc.
To protect the system from overloading (when rotating a pot on the Tx ), the script will not be started again for 3000ms.
-p option is useful for rotary switches, where each switch step will generate a command till the desired one is reached. In order to avoid this, the channel value must keep its value for a given period in order to be accepted, for example 1000ms. This way if you never stay more than a second on a step of the rotary switch while rotating it, only the last one will be accepted. The drawback is the delay this options adds.

Packets will be aggregated in chunks of 10 into one UDP frame. A MAVLINK_MSG_ID_ATTITUDE from the FC will flush the buffer. 
When aggregating, minimum buffer size is 3 packets. This can be changed by adding N*100 to the aggregated packet size
That is, value ```-a 109 ``` means:  Aggretage mavlink message in chunks up to 9. When a MAVLINK_MSG_ID_ATTITUDE is received flush if the current size is >=1.
This will allow faster Artificial Horizon response.

This way the OSD will be updated with the same rate and no lag will be added.
 -a 15 : will flush the cached messages into one UDP packet after count of message reaches 15
 -a 1024 : will flush the cached messages into one UDP packet total length of all message reaches 1024 bytes
in both cases the buffer will be flushed if there are at least 3 packets and a MAVLINK_MSG_ID_ATTITUDE is received.
Temperature will be read from the board and will be injected into the mavlink stream each second via MAVLINK_MSG_ID_RAW_IMU 27 message.

Option to send text from the cam. The file mavlink.msg in {tempfolder} is monitored and when found, all data from it are send as plain text message via   MAVLINK_MSG_ID_STATUSTEXT 253 to the ground station and the file is deleted.
Sample:
echo "Huston, this is IPC" >/tmp/mavlink.msg

killall -usr1 mavfwd   # will send text message to ground station even no data are received in the serial port, can be used to test Camera to Ground connection.

wfb reporting scans /tmp/wfb.log file every second and extract values from "UDP rxq overflow: 2 packets dropped" lines. wfb_tx stdout must be redirected to this file, like this : 
wfb_tx -p ${stream} -u ${udp_port} -R 512000 -K ${keydir}/${unit}.key -B ${bandwidth} -M ${mcs_index} -S ${stbc} -L ${ldpc} -G ${guard_interval} -k ${fec_k} -n ${fec_n} -T ${pool_timeout} -i ${link_id} -f ${frame_type} ${wlan} 2>&1 | tee -a /tmp/wfb.log &

In order to cross-compile for a specific camera., pull OpenIPC locally and compile it for the desired chipset.
Assuming it is compiled in ```/home/home/src/openipc```  , then mavfwd can be compiled and copied to cam with IP 192.168.1.88 like this:
```
cp mavfwd.c /home/home/src/openipc/output/build/mavfwd-220d30e118d26008e94445887a03d77ba73c2d29/
make -C /home/home/src/openipc/output/ mavfwd-rebuild
scp /home/home/src/openipc/output/build/mavfwd-220d30e118d26008e94445887a03d77ba73c2d29/mavfwd root@192.168.1.88:/usr/bin/
```