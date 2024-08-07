
echo $1 $2 >>/tmp/channels.log
# 
# gk7205v300 IMX335 board
# 6 position switch as defined by ELRS 
# Change resolutions in flight
#2000, 1725, 1575, 1425, 1275, 1000
#if [ $1 -eq 7 ]; then
    if [ $2 -lt 1050 ]; then
		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 7000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30
		yaml-cli -s .isp.sensorConfig /etc/sensors/imx335_i2c_4M.ini		
		 		
		echo "1920x1080/30fps/7Mb" >/tmp/mavlink.msg
		
		killall -1 majestic
		sleep 1
		/etc/gkrcparams --MaxQp 30 --MaxI 2
      
    elif [ $2 -gt 1250 ] && [ $2 -lt 1300 ]; then
		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30
		yaml-cli -s .isp.sensorConfig /etc/sensors/imx335_i2c_4M.ini
		
		echo "1920x1080/30fps/14Mb" >/tmp/mavlink.msg
		
		killall -1 majestic
		sleep 1
		/etc/gkrcparams --MaxQp 30 --MaxI 2		
		
	elif [ $2 -gt 1400 ] && [ $2 -lt 1450 ]; then
		yaml-cli -s .image.contrast 80
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 60
		yaml-cli -s .image.luminance 60
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30
		yaml-cli -s .isp.sensorConfig /etc/sensors/imx335_i2c_4M.ini
		
		echo "1920x1080/30fps/14Mb C:80%" >/tmp/mavlink.msg
		
		killall -1 majestic	
		sleep 1		
		/etc/gkrcparams --MaxQp 30 --MaxI 2
 
    elif [ $2 -gt 1550 ] && [ $2 -lt 1600 ]; then
	
		yaml-cli -s .image.contrast 80
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 60
		yaml-cli -s .image.luminance 60
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30
		yaml-cli -s .isp.sensorConfig /etc/sensors/imx335_i2c_4M.ini
		
		echo "1920x1080/30fps/14Mb C:80%" >/tmp/mavlink.msg
		
		
		killall -1 majestic
		sleep 1
		/etc/gkrcparams --MaxQp 30 --MaxI 2
		
    elif [ $2 -gt 1700 ] && [ $2 -lt 1750 ]; then

		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size size: 2592x1520
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -d .video0.fps		
		
		
		yaml-cli -s .isp.sensorConfig /etc/sensors/5M_imx335.ini	
		
		echo "2592x1520/25fps" >/tmp/mavlink.msg
		
		killall -1 majestic
		sleep 1
		/etc/gkrcparams --MaxQp 30 --MaxI 2
		
	elif [ $2 -gt 1950 ]; then
		# emergency
		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 3000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30
		yaml-cli -s .isp.sensorConfig /etc/sensors/imx335_i2c_4M.ini
				 		
		echo "1920x1080/30fps/3Mb" >/tmp/mavlink.msg
		
		killall -1 majestic
		sleep 1
		/etc/gkrcparams --MaxQp 30 --MaxI 2
		
    else
		echo "unknown value" $2
    fi
#fi


exit 1
