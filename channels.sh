. /etc/wfb.conf

function set_mcs() { 
  echo $(date "+%H:%M:%S") " Setting MCS $1"
  if [ ${mcs_index} -eq $1 ]; then
	echo $(date "+%H:%M:%S") " MCS the same : $1"
  else
#	echo $(date "+%H:%M:%S") " changing MCS from ${mcs_index} to $1. Hold on!"
	echo "Changing MCS from ${mcs_index} to $1. Hold on!" >/tmp/mavlink.msg 	
# wait to make this transmitted before wfb_tx is killed to change MCS 	
	sleep 0.2

# wfb_tx will be killed when changing MCS index.	
#	kill -9 $(pidof wfb_tx)
# telemetry_tx will be killed when changing MCS index only if it tries to inject
	kill -9 $(pidof telemetry_tx)
# better not to kill it, since it may call this script again on restart !
#	kill -9 $(pidof mavfwd)
	
	sed -i 's/^mcs_index=.*/mcs_index='$1'/' /etc/wfb.conf	
	
	wifibroadcast start		
	
	echo $(date "+%H:%M:%S") " wifibroadcast started " 
  fi
}


#for debugging
#echo 'Starting channels.sh ' $1 $2 >>/tmp/channels.log
#echo 'Channels Called' $1 $2


#2000, 1725, 1575, 1425, 1275, 1000
#  contrast: 45
#  hue: 45
#  saturation: 40
#  luminance: 50

# 6 position switch as defined by ELRS 
#channel 7
#if [ $1 -eq 7 ]; then
    if [ $2 -lt 1050 ]; then
		yaml-cli -s .image.contrast 70
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 60
		yaml-cli -s .video0.minQp 12		
		
		yaml-cli -s .isp.exposure 7	

		#delibaretely not set
		#set_mcs 3 
		echo "1920x1080/60fps/E7ms" >/tmp/mavlink.msg
		
#		killall -1 majestic
		killall majestic # just in case majestic had started sending long packets
		sleep 1
		killall -9 majestic
		majestic &
		echo "Majestic brute force restart" >/tmp/mavlink.msg
      
    elif [ $2 -gt 1250 ] && [ $2 -lt 1300 ]; then
		yaml-cli -s .image.contrast 70
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		yaml-cli -s .image.sharpen 100
		yaml-cli -s .image.denoise 0
		
		yaml-cli -s .video0.bitrate 14000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 60	
		yaml-cli -s .video0.minQp 12
		
		yaml-cli -s .isp.exposure 13	
		
		set_mcs 3 
		
		echo "1920x1080/60fps/E13ms" >/tmp/mavlink.msg
		
		killall -1 majestic
		
	elif [ $2 -gt 1400 ] && [ $2 -lt 1450 ]; then
	
		yaml-cli -s .image.contrast 70
		yaml-cli -s .image.hue 55
		yaml-cli -s .image.saturation 60
		yaml-cli -s .image.luminance 50
		yaml-cli -s .image.sharpen 100
		yaml-cli -s .image.denoise 0
		
		yaml-cli -s .video0.bitrate 15000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 30	
		yaml-cli -s .video0.minQp 12
		
		yaml-cli -s .isp.exposure 28	
		yaml-cli -s .isp.IPQPDelta -5	

		set_mcs 3 
		
		echo "1920x1080/30fps/E28ms" >/tmp/mavlink.msg	
				
		killall -1 majestic
		
    elif [ $2 -gt 1550 ] && [ $2 -lt 1600 ]; then
	
		yaml-cli -s .image.contrast 70
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		yaml-cli -s .image.sharpen 100
		yaml-cli -s .image.denoise 0
		
		
		yaml-cli -s .video0.bitrate 19000		
		yaml-cli -s .video0.size 1920x1080
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 60	
		yaml-cli -s .video0.minQp 12
		
		yaml-cli -s .isp.exposure 18	
		
		set_mcs 4 
		
		echo "1920x1080/60fps/E18ms" >/tmp/mavlink.msg
		
		killall -1 majestic
 
#    elif [ $2 -gt 1550 ] && [ $2 -lt 1600 ]; then
#	
#		yaml-cli -s .image.contrast 60
#		yaml-cli -s .image.hue 50
#		yaml-cli -s .image.saturation 50
#		yaml-cli -s .image.luminance 50
#		
#		yaml-cli -s .video0.bitrate 20000		
#		yaml-cli -s .video0.size 2944x1656
#		yaml-cli -s .video0.gopSize 1
#		yaml-cli -s .video0.fps 30
#		yaml-cli -s .isp.exposure 20
#		
#		set_mcs 4 
#		
#		echo "2944x1656/30fps/E20/MCS3" >/tmp/mavlink.msg		
#				
#		killall -1 majestic
#		
    elif [ $2 -gt 1700 ] && [ $2 -lt 1750 ]; then
				
		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50	
		yaml-cli -s .image.sharpen 100
		yaml-cli -s .image.denoise 0
		
		yaml-cli -s .video0.bitrate 21000		
		yaml-cli -s .video0.size 3200x1800
		yaml-cli -s .video0.gopSize 1
		yaml-cli -s .video0.fps 30
		yaml-cli -s .video0.minQp 12
		
		yaml-cli -s .isp.exposure 15
		
		set_mcs 4 
		
		echo "3200x1800/30fps/E30/MCS4" >/tmp/mavlink.msg		
				
		killall -1 majestic
		
	elif [ $2 -gt 1950 ]; then
	
		yaml-cli -s .image.contrast 60
		yaml-cli -s .image.hue 50
		yaml-cli -s .image.saturation 50
		yaml-cli -s .image.luminance 50
		
		yaml-cli -s .video0.bitrate 18000		
		yaml-cli -s .video0.size 3840x2160
		yaml-cli -s .video0.gopSize 1.5
		yaml-cli -s .video0.fps 20
		yaml-cli -s .video0.qpDelta -8		
		yaml-cli -s .video0.minQp 30
#		yaml-cli -s .video0.minQp 12
#		yaml-cli -s .video0.maxQp 48		
  
		yaml-cli -s .isp.exposure 36
#		yaml-cli -s .isp.IPQPDelta -8	
		
		set_mcs 4  
		
		echo "3840x2160/20fps/E36/MCS4" >/tmp/mavlink.msg		
		killall -1 majestic
		 
    else
		echo "unknown value" $2
    fi
#fi


exit 1
