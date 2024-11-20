#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <termios.h>
#include <assert.h>
#include <time.h>

#include <event2/event.h>
#include <event2/util.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>


#include "mavlink/common/mavlink.h"

#include "osd/msp/msp.h"
//#include "osd/msp/msp.h" // why the fuck
//#include "osd/msp_displayport_mux.c"
#include "osd/net/network.h"

#include "osd.c" //Should be merged

//#include "msp/msp.cpp"

#define MAX_MTU 9000
#include "osd/util/settings.h"


bool vtxMenuActive = false;
bool armed = true; // assume armed until we are told otherwise from the fc
bool AbortNow=false;
bool verbose = false;
bool ParseMSP = true;
bool DrawOSD = false;
bool mspVTXenabled = false;
bool vtxMenuEnabled = false;

//libevent base main loop
extern struct event_base *base = NULL;

int serial_fd = 0;

int in_sock = 0;

int MSPUDPPort=0;

int MSP_PollRate=20;

int matrix_size=0;

int AHI_Enabled=1;

const char *default_master = "/dev/ttyAMA0";
const int default_baudrate = 115200;
const char *defualt_out_addr = "";
const char *default_in_addr =  "127.0.0.1:0";
const int RC_CHANNELS = 65; //RC_CHANNELS ( #65 ) for regular MAVLINK RC Channels read (https://mavlink.io/en/messages/common.html#RC_CHANNELS)
const int RC_CHANNELS_RAW = 35; //RC_CHANNELS_RAW ( #35 ) for ExpressLRS,Crossfire and other RC procotols (https://mavlink.io/en/messages/common.html#RC_CHANNELS_RAW)


//if we gonna use MSP parsing
msp_state_t *rx_msp_state;

struct bufferevent *serial_bev;
struct sockaddr_in sin_out = {
	.sin_family = AF_INET,
};
int out_sock=0;


long aggregate=1;
/*
When aggregating packets, we flush when a message that draws artifical horizon (AHI). This shows the minimum size of packets in the buffer in order to flush
If we need smooth response of the OSD, then set this to 1.
Value of 3 is a compromise, 3 sequential packets for AHI change will be aggregated, this will save bandwidth, but will reduse the frame rate of the OSD.
*/
int minAggPckts=3;

bool monitor_wfb=false;
static int temp = false;
 

static void print_usage()
{
	printf("Usage: msposd [OPTIONS]\n"
	       "Where:\n"

 "	-m --master      Serial port to receive MSP (%s by default)\n"
 "	-b --baudrate    Serial port baudrate (%d by default)\n"
 "	-o --output	  	 UDP endpoint to forward aggregated MSP messages (%s)\n"
 "	-c --channels    RC Channel to listen for commands (0 by default) and exec channels.sh. This command can be repeated. Channel values are 1-based.\n"
 "	-w --wait        Delay after each command received(2000ms default)\n"
 "	-r --fps         Max MSP Display refresh rate(5..50)\n"
 "	-p --persist     How long a channel value must persist to generate a command - for multiposition switches (0ms default)\n"
 "	-t --temp        Read SoC temperature\n"
 "	-d --wfb         Monitors wfb.log file and reports errors via HUD messages\n"
 "	-s --osd         Parse MSP and draw OSD over the video\n"
 "	-a --ahi         Draw graphic AHI, mode [0-No, 2-Simple 1-Ladder, 3-LadderEx]\n"
 "	-x --matrix      OSD matrix (0 - 53:20 , 1- 50:18 chars)\n"
 "	   --mspvtx      Enable mspvtx support\n"
 "	-v --verbose     Show debug infot\n"	       
 "	--help           Display this help\n",



	       default_master, default_baudrate, defualt_out_addr);
}

static speed_t speed_by_value(int baudrate)
{
	switch (baudrate) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 500000:
		return B500000;
	case 921600:
		return B921600;
	case 1500000:
		return B1500000;
	default:
		printf("Not implemented baudrate %d\n", baudrate);
		exit(EXIT_FAILURE);
	}
}

uint64_t get_current_time_ms() // in milliseconds
{
    struct timespec ts;
    int rc = clock_gettime(1 /*CLOCK_MONOTONIC*/, &ts);
    //if (rc < 0) 
//		return get_current_time_ms_Old();
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static bool parse_host_port(const char *s, struct in_addr *out_addr,
			    in_port_t *out_port)
{
	char host_and_port[32] = { 0 };
	strncpy(host_and_port, s, sizeof(host_and_port) - 1);

	char *colon = strchr(host_and_port, ':');
	if (NULL == colon) {
		return -1;
	}

	*colon = '\0';
	const char *host = host_and_port, *port_ptr = colon + 1;

	const bool is_valid_addr = inet_aton(host, out_addr) != 0;
	if (!is_valid_addr) {
		printf("Cannot parse host `%s'.\n", host);
		return false;
	}

	int port;
	if (sscanf(port_ptr, "%d", &port) != 1) {
		printf("Cannot parse port `%s'.\n", port_ptr);
		return false;
	}
	*out_port = htons(port);

	return true;
}

static void signal_cb(evutil_socket_t fd, short event, void *arg)
{
	struct event_base *base = arg;
	(void)event;
	AbortNow=true;
	printf("Exit Request: %s signal received\n", strsignal(fd));
	event_base_loopbreak(base);
}


#define MAX_BUFFER_SIZE 50 //Limited by mavlink

static char MavLinkMsgFile[128]= "mavlink.msg";  

#ifdef _x86
static char WfbLogFile[128]= "wfb.log";  
#else
static char WfbLogFile[128]= "/tmp/wfb.log";  
#endif

bool Check4MavlinkMsg(char* buffer) {
    //const char *filename = MavLinkMsgFile;//"mavlink.msg";
    
    FILE *file = fopen(MavLinkMsgFile, "rb");
    if (file == NULL)// No file, no problem
        return false;
        
    size_t bytesRead = fread(buffer, 1, MAX_BUFFER_SIZE, file);
    fclose(file);

    if (bytesRead > 0) {        
		if (verbose)
        	printf("Mavlink msg from file:%s\n", buffer);
        if (remove(MavLinkMsgFile) != 0)             
        	printf("Error deleting file");        
		return true;
    } else 
        printf("Mavlink empty file ?!\n");    

	return false;
}
static uint8_t system_id=1;


static unsigned long long LastWfbSent=0;

/// @brief wfb_tx output should be redirected to wfb.log. Parse it and extracted dropped packets!
/// @return Number of dropped packets for the last period
int SendWfbLogToGround(){
	if (!monitor_wfb)
		return false;
	if ( abs(get_current_time_ms()-LastWfbSent) < 1000)//Once a second max
		return 0;

	LastWfbSent = abs(get_current_time_ms());		

	char msg_buf[200];

 	FILE *file = fopen(WfbLogFile, "r");
    if (file == NULL) {
		if (verbose)
        	printf("No file %s\n",WfbLogFile);
        return 0;
    }

    char buff[200];
    int total_dropped_packets = 0;

/*
UDP rxq overflow: 2 packets dropped
UDP rxq overflow: 45 packets dropped
*/
	//if (verbose)
	// printf("Parsing file: %s\n",WfbLogFile);
	int maxlinestoparse=0;
    // Read lines from the file and parse for dropped packets
    while (fgets(buff, sizeof(buff), file) != NULL) {        
		if ((maxlinestoparse++) >30){//If the file is very long, no need to struggle 
			total_dropped_packets=9999;
			break;
		}
        if (strstr(buff, "packets dropped") != NULL) { // Check if the line contains "packets dropped"            
            char *token = strtok(buff, " ");//split by space,  Parse the line to extract the number of dropped packets
            while (token != NULL) {
                if (isdigit(token[0])) {
                    int dropped_packets = atoi(token);
                    total_dropped_packets += dropped_packets;
                    break;
                }
                token = strtok(NULL, " ");
            }
        }
    }
    fclose(file);

	if (maxlinestoparse==0 && total_dropped_packets==0)//file was empty
		return 0;

    //remove(WfbLogFile) // This will break console output in the file!    
		
    file = fopen(WfbLogFile, "w");// Open the file in write mode, truncating it to zero size
    if (file != NULL) 
    	fclose(file);   
	     
	
	if (false /*out_sock>0*/){//if we have outbound socket for mavlink, but now if available it is used for MSP
		sprintf(msg_buf,"%d video pckts dropped!\n", total_dropped_packets);
		printf("%s",msg_buf);
		
		mavlink_message_t message;
		
		mavlink_msg_statustext_pack_chan(
			system_id,
			MAV_COMP_ID_SYSTEM_CONTROL,
			MAVLINK_COMM_1,
			&message,
			4,  	// 4 - Warning, 5 - Error
			msg_buf,
			0,0);

		uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
		const int len = mavlink_msg_to_send_buffer(buffer, &message);
			
		sendto(out_sock, buffer, len, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));
	}

	return total_dropped_packets;    

}


static bool SendInfoToGround(){
	char msg_buf[MAX_BUFFER_SIZE];
	if (!Check4MavlinkMsg(&msg_buf[0]))
		return false;

	//Huston, we have a message for you.
    mavlink_message_t message;
 	
    mavlink_msg_statustext_pack_chan(
        system_id,
        MAV_COMP_ID_SYSTEM_CONTROL,
        MAVLINK_COMM_1,
        &message,
		4,  	// 4 - Warning, 5 - Error
		msg_buf,
		0,0);

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    const int len = mavlink_msg_to_send_buffer(buffer, &message);
		
	sendto(out_sock, buffer, len, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));

	return true;    
}

uint64_t get_current_time_ms_Old() {
    time_t current_time = time(NULL);
    return (uint64_t)(current_time * 1000);
}


int last_wifi_temp;
static long last_wifi_temp_taken=0;
int Get8812EU2Temp(){

	if ((get_time_ms() - last_wifi_temp_taken  ) < 1000)//Set some caching to keep CPU load low                      
		return last_wifi_temp;
    last_wifi_temp_taken= get_time_ms();

	FILE *stat = popen("cat /proc/net/rtl88x2eu/wlan0/thermal_state", "r");
	if (stat == NULL) {
		fprintf(stderr, "Failed to run command\n");
		return 1;
	}
	char buffer[128];
	int temperature=0;
	char c[25];
	// Read the first line of output
	if (fgets(buffer, sizeof(buffer), stat) != NULL) {               
		if (sscanf(buffer, "rf_path: %*d, thermal_value: %*d, offset: %*d, temperature: %d", &temperature) == 1) {
			printf("WiFi Temperature from the first line: %d\n", temperature);
		} else {
			fprintf(stderr, "Failed to parse wifi temperature\n");
		}
	}
	// Close the pipe
	pclose(stat);
	last_wifi_temp=temperature;
	return temperature;
}

int last_board_temp;

static long last_board_temp_taken=0;

/// @brief 
/// @return -100 if no sigmastar found 
int GetTempSigmaStar(){


	if ( (get_time_ms() - last_board_temp_taken  ) < 1000)//Set some caching to keep CPU load low                      
		return last_board_temp;
    last_board_temp_taken= get_time_ms();
//https://wx.comake.online/doc/doc/SigmaStarDocs-SSD220-SIGMASTAR-202305231834/platform/BSP/Ikayaki/frequency_en.html
 	FILE *file = fopen("/sys/devices/virtual/mstar/msys/TEMP_R", "r"); //Temperature 62
    if (file == NULL) {
		if (verbose)
        	printf("No temp data at %s\n",WfbLogFile);
        return -100;
    }
	last_board_temp=-100;
  	char buff[200];
    // Read lines from the file and parse for dropped packets
    if (fgets(buff, sizeof(buff), file) != NULL) {        	         
        char *temperature_str = strstr(buff, "Temperature"); // Find "Temperature"
        if (temperature_str != NULL) 
            last_board_temp = atoi(temperature_str + 12); // Extract temperature value        
    }
    fclose(file);

	if (verbose && last_board_temp<-90)
        printf("No temp data in file %s\n",WfbLogFile);

	//printf("SigmaStar temp: %d\n",last_board_temp);
	return last_board_temp;

}

static uint64_t LastTempSent;
 

static int SendTempToGround(unsigned char* mavbuf){

	if ( abs(get_current_time_ms()-LastTempSent) < 1000)//Once a second
		return 0;

	LastTempSent = abs(get_current_time_ms());

	if(temp==2)//read the temperature only once per second
		last_board_temp=GetTempSigmaStar();

	char msg_buf[MAX_BUFFER_SIZE];
    mavlink_message_t message;
 	
    mavlink_msg_raw_imu_pack_chan(
        system_id,
        MAV_COMP_ID_SYSTEM_CONTROL,
        MAVLINK_COMM_1,
        &message,0,0,0,0,0,0,0,0,0,0,0,
		last_board_temp*100	
		);

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    const int len = mavlink_msg_to_send_buffer(mavbuf, &message);
	//printf("temp %f sent with %d bytes",last_board_temp*100,len);		

	return len;    
}

bool version_shown=false;
static void ShowVersionOnce(mavlink_message_t* msg, uint8_t header){
	if (version_shown)
		return;
	version_shown=true;
/*
The major version can be determined from the packet start marker byte:
MAVLink 1: 0xFE
MAVLink 2: 0xFD
*/
	if (header==0xFE)
		printf("Detected MAVLink ver: 1.0  (%d)\n",msg->magic);
	if (header==0xFD)
		printf("Detected MAVLink version: 2.0  (%d)\n",msg->magic);

	printf("System_id = %d \n",system_id);		

}
bool fc_shown=false;
void handle_heartbeat(const mavlink_message_t* message)
{
	if (fc_shown)
		return;
    fc_shown=true;

    mavlink_heartbeat_t heartbeat;
    mavlink_msg_heartbeat_decode(message, &heartbeat);

    printf("Flight Controller Type :");
    switch (heartbeat.autopilot) {
        case MAV_AUTOPILOT_GENERIC:
            printf("Generic/INAV");
            break;
        case MAV_AUTOPILOT_ARDUPILOTMEGA:
            printf("ArduPilot");
            break;
        case MAV_AUTOPILOT_PX4:
            printf("PX4");
            break;
        default:
            printf("other");
            break;
    }
	printf("\n");    
}


void handle_statustext(const mavlink_message_t* message)
{	
    mavlink_statustext_t statustext;
    mavlink_msg_statustext_decode(message, &statustext);		
    printf("FC message:%s", statustext.text);

}

unsigned long long get_current_time_ms_simple() {
    clock_t current_clock_ticks = clock();	
    return (unsigned long long)(current_clock_ticks * 1000 / CLOCKS_PER_SEC);
	
}



uint16_t channels[18];

// Channel monitoring:

// Maximum number of channels to monitor.
#define kChannelCount 16
// Whether the channel monitoring was enabled.
bool rc_channel_mon_enabled = false;
// A boolean array that defines which channels are monitored.
// Setting a value to "true" will make this program run /usr/bin/channels.sh 
// if the channel value changes.
static bool rc_channel_mon[kChannelCount] = {0};

//Time to wait between bash script starts.
long wait_after_bash=2000;

//how long a RC value should stay at one level to issue a command
int ChannelPersistPeriodMS=2000;

static uint64_t LastStart[kChannelCount] = {0};//
static unsigned long LastValue[kChannelCount] = {0};
uint16_t NewValue[kChannelCount] = {0};
static uint64_t NewValueStart[kChannelCount] = {0};

unsigned int ChannelCmds[kChannelCount] = {0};
static uint64_t mavpckts_ttl=0;


void ProcessChannel(int rc_channel_no){
	//rc_channel_no, zero based
	uint16_t val=0;
	if (rc_channel_no<0 || rc_channel_no>15  /* || (mavpckts_ttl<100*/ ) //wait in the beginning for the values to settle
		return;

	if (!armed)
		handle_stickcommands(channels);

	if ( abs(get_current_time_ms()-LastStart[rc_channel_no]) < wait_after_bash)		
		return;

	val=channels[rc_channel_no];

	if (abs(val-NewValue[rc_channel_no])>32 && ChannelPersistPeriodMS>0){
		//We have a new value, let us wait for it to persist
		NewValue[rc_channel_no]=val;
		NewValueStart[rc_channel_no]=get_current_time_ms();		
		return;
	}else
		if (abs(get_current_time_ms()-NewValueStart[rc_channel_no])<ChannelPersistPeriodMS)		
			return;//New value should remain "stable" for a second before being approved					
		else{}//New value persisted for more THAN ChannelPersistPeriodMS					

	if (abs(val-LastValue[rc_channel_no])<32)//the change is too small	
		return;	
	
	NewValue[rc_channel_no]=val;
	LastValue[rc_channel_no]=val;
	
	char buff[60];

	// For compatibility purposes, channels.sh is called with 1-based channel argument.
    sprintf(buff, "/usr/bin/channels.sh %d %d &", rc_channel_no + 1, val);

	printf("Starting(%d): %s \n",ChannelCmds[rc_channel_no],buff);
	LastStart[rc_channel_no]=get_current_time_ms();
    
	if (ChannelCmds[rc_channel_no]>0){
		//intentionally skip the first command, since when starting  it will always receive some channel value and execute the script
    	system(buff);
	}

	printf("Started(%d): %s \n",ChannelCmds[rc_channel_no],buff);
	ChannelCmds[rc_channel_no]++;
	
}

void ProcessChannels(){
	for (int i=0; i < kChannelCount; ++i) {
		if (rc_channel_mon[i])
			ProcessChannel(i);
	}
}

void showchannels(int count){
	if (verbose){
		printf("Channels :"); 
		for (int i =0; i <count;i++)
			printf("| %02d", channels[i]);
		printf("\r\n");
	}
}

void handle_msg_id_rc_channels_raw(const mavlink_message_t* message){
	mavlink_rc_channels_raw_t rc_channels;
	mavlink_msg_rc_channels_raw_decode(message, &rc_channels);	
	memcpy(&channels[0], &rc_channels.chan1_raw, 8 * sizeof(uint16_t));	
	showchannels(8);
	ProcessChannels();
}


void handle_msg_id_rc_channels_override(const mavlink_message_t* message){
	mavlink_rc_channels_override_t rc_channels;
	mavlink_msg_rc_channels_override_decode(message, &rc_channels);	
	memcpy(&channels[0], &rc_channels.chan1_raw, 18 * sizeof(uint16_t));	
	showchannels(18);
	ProcessChannels();
}

void handle_msg_id_rc_channels(const mavlink_message_t* message){
	mavlink_rc_channels_t rc_channels;
	mavlink_msg_rc_channels_decode(message, &rc_channels);	
	memcpy(&channels[0], &rc_channels.chan1_raw, 18 * sizeof(uint16_t));	
	showchannels(18);
	ProcessChannels();
}

unsigned char mavbuf[2048];
unsigned int mavbuff_offset=0;
unsigned int mavpckts_count=0;
 
static void process_mavlink(uint8_t* buffer, int count, void *arg){

    mavlink_message_t message;
    mavlink_status_t status;
    for (int i = 0; i < count; ++i) {
		
		if (mavbuff_offset>2000){
			printf("Mavlink buffer overflowed! Packed lost!\n");
			mavbuff_offset=0;
		}
		mavbuf[mavbuff_offset]=buffer[i];
		mavbuff_offset++;
        if (mavlink_parse_char(MAVLINK_COMM_0, buffer[i], &message, &status) == 1) {
			mavpckts_ttl++;
			system_id=message.sysid;
			ShowVersionOnce(&message, (uint8_t) buffer[0]);
			if (verbose)
        	    printf("Mavlink msg %d no: %d\n",message.msgid, message.seq);
            switch (message.msgid) {
            	case MAVLINK_MSG_ID_RC_CHANNELS_RAW://35 Used by INAV
	                handle_msg_id_rc_channels_raw(&message);
	                break;
	            case MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE://70
    	            handle_msg_id_rc_channels_override(&message);
                	break;

				case MAVLINK_MSG_ID_RC_CHANNELS://65 used by ArduPilot
	                handle_msg_id_rc_channels(&message);
                	break;

             	case MAVLINK_MSG_ID_HEARTBEAT://Msg info from the FC
	                handle_heartbeat(&message);
                	break;
				case MAVLINK_MSG_ID_STATUSTEXT://Msg info from the FC					
	                //handle_statustext(&message);
                	break;					
            }
			 
			mavpckts_count++;			
			bool mustflush=false;
			if (aggregate>0){//We will send whole packets only				
				if (
					((aggregate>=1 && aggregate<50 ) && mavpckts_count>=aggregate ) ||  //if packets more than treshold
					((aggregate>50 && aggregate<2000 ) && mavbuff_offset>=aggregate ) || //if buffer  more than treshold
					((mavpckts_count>=minAggPckts) && message.msgid==MAVLINK_MSG_ID_ATTITUDE )	//MAVLINK_MSG_ID_ATTITUDE will always cause the buffer flushed
				){
					//flush and send all data
					if (sendto(out_sock, mavbuf, mavbuff_offset, 0, (struct sockaddr *)&sin_out, sizeof(sin_out)) == -1) {
						perror("sendto()");
						event_base_loopbreak(arg);
					}
					if (verbose)
						printf("%d Pckts / %d bytes sent\n",mavpckts_count,mavbuff_offset);
					mavbuff_offset=0;
					mavpckts_count=0;
					SendInfoToGround();

					SendWfbLogToGround();


					if (last_board_temp>-100){
						
						mavbuff_offset=SendTempToGround(mavbuf);
					}
					if (mavbuff_offset>0){
						mavpckts_count++;
					}

				}
			}
        }//if mavlink_parse_char
    }
}

static long ttl_packets=0;
static long ttl_bytes=0;

static long stat_bytes=0;
static long stat_pckts=0;
static long last_stat=0;
 
//handles both UART and Serial read
static void serial_read_cb(struct bufferevent *bev, void *arg)
{
	if (AbortNow)//Abort request by user pending...
		return;

	int fd = bufferevent_getfd(bev); // Get the file descriptor associated with the event
    if (fd == serial_fd)       
        {}//printf("Reading from serial port...\n");        
     else if (fd == in_sock)    
        {}//printf("Reading from UDP socket...\n");       
    

	struct evbuffer *input = bufferevent_get_input(bev);
	int packet_len, in_len;
	struct event_base *base = arg;	

	while ((in_len = evbuffer_get_length(input))) {
		unsigned char *data = evbuffer_pullup(input, in_len);
		if (data == NULL) {
			return;
		}
		packet_len = in_len;

		if (get_time_ms() - last_stat>1000) {//no faster than 1 per second    
			last_stat=(get_time_ms());
			if (stat_screen_refresh_count==0)
				stat_screen_refresh_count++;
			if (verbose){
				if (DrawOSD)
				printf("UART Events:%u MessagesTTL:%u AttitMSGs:%u(%dms) Bytes/S:%u FPS:%u of %u (skipped:%d), AvgFrameLoad ms:%d | %d | %d | \r\n",stat_pckts,stat_msp_msgs,stat_msp_msg_attitude, (stat_attitudeDelay / (stat_msp_msg_attitude+1) ),
					stat_bytes, stat_screen_refresh_count, stat_MSP_draw_complete_count, stat_skipped_frames, stat_draw_overlay_1/stat_screen_refresh_count,stat_draw_overlay_2/stat_screen_refresh_count,stat_draw_overlay_3/stat_screen_refresh_count);
					else
				printf("UART Events:%u MessagesTTL:%u AttitMSGs:%u(%dms) Bytes/S Recvd:%u Sent:%u, Screen FPS:%u  MSP_FPS:%u  MSP_UDP_Pckts:%u, AvgFrameLoad ms:%d | %d | \r\n",stat_pckts,stat_msp_msgs,stat_msp_msg_attitude, (stat_attitudeDelay / (stat_msp_msg_attitude+1) ),
					stat_bytes, stat_MSPBytesSent, stat_screen_refresh_count, stat_MSP_draw_complete_count,stat_UDP_MSPframes, stat_draw_overlay_1/stat_screen_refresh_count,stat_draw_overlay_3/stat_screen_refresh_count);

				showchannels(18);
			}
			stat_screen_refresh_count=0;
			stat_pckts=0;
			stat_bytes=0;
			stat_msp_msgs=0;
			stat_msp_msg_attitude=0;
			stat_draw_overlay_1=0;
			stat_draw_overlay_2=0;
			stat_draw_overlay_3=0;
			stat_skipped_frames=0;
			stat_MSPBytesSent=0;
			stat_MSP_draw_complete_count=0;
			stat_UDP_MSPframes=0;
    	}
		stat_pckts++;
		stat_bytes+=packet_len;
		ttl_packets++;
		ttl_bytes+=packet_len;
		if (ParseMSP){
			//Test to do RAW MSP forward here
			//sendto(out_sock, data, packet_len, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));
			for(int i=0;i<packet_len;i++)
				msp_process_data(rx_msp_state, data[i]);
			
			
			//continue;
		}else{
			if (!version_shown && ttl_packets%10==3)//If garbage only, give some feedback do diagnose
				printf("Packets:%d  Bytes:%d\n",ttl_packets,ttl_bytes);

			if (aggregate==0){
				if (sendto(out_sock, data, packet_len, 0,
				(struct sockaddr *)&sin_out,
				sizeof(sin_out)) == -1) {
						perror("sendto()");
						//event_base_loopbreak(base);
						return false;
				}
			}

			//Let's try to parse the stream	
			if (aggregate>0 || rc_channel_mon_enabled)//if no RC channel control needed, only forward the data
				process_mavlink(data,packet_len, arg);//Let's try to parse the stream		
		}

		evbuffer_drain(input, packet_len);		
	}
}



// Signal handler function
void sendtestmsg(int signum) {
	printf("Sending test mavlink msg.\n");
	char buff[200];
	sprintf(buff, "echo Hello_From_OpenIPC > %s", MavLinkMsgFile);
	system(buff);
	SendInfoToGround();     
}


static void serial_event_cb(struct bufferevent *bev, short events, void *arg)
{
	(void)bev;
	struct event_base *base = arg;

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		printf("Serial connection closed\n");
		event_base_loopbreak(base);
	}
}

// Callback function to handle keyboard input
#ifdef __KEYBOARD_INPUT__
void stdin_read_callback(int fd, short event, void *arg) {
    char buffer[256];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    
    if (n > 0 && ! armed) {
        buffer[n] = '\0';  // Null-terminate the input
        printf("You typed: %s\n", buffer);
		if (buffer[0] == 'm') {
			printf("Disableing msposd\n");
  	    	vtxMenuEnabled = init_state_manager();
			if (vtxMenuEnabled)
				vtxMenuActive = true;
		}
		if (buffer[0] == 'q') {
			printf("Enableing msposd\n");
			vtxMenuActive = false;
		}
        if (buffer[0] == 'w') {
			uint16_t channels[18];
			channels[0] = 1500;
			channels[1] = 2000;
			channels[2] = 1500;
			channels[3] = 1500;
		    handle_stickcommands(channels); // Move up
        } else if (buffer[0] == 's') {
			uint16_t channels[18];
			channels[0] = 1500;
			channels[1] = 1000;
			channels[2] = 1500;
			channels[3] = 1500;
		    handle_stickcommands(channels);// Move down
        } else if (buffer[0] == 'a') {
			uint16_t channels[18];
			channels[0] = 1000;
			channels[1] = 1500;
			channels[2] = 1500;
			channels[3] = 1500;
		    handle_stickcommands(channels);// Move left
        } else if (buffer[0] == 'd') {
 			uint16_t channels[18];
			channels[0] = 2000;
			channels[1] = 1500;
			channels[2] = 1500;
			channels[3] = 1500;
		    handle_stickcommands(channels);// Move right
        } else if (buffer[0] == 'e') {
            //unused
        }	
    }
}
#endif



static void* setup_temp_mem(off_t base, size_t size)
{
	int mem_fd;

	mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem_fd < 0) {
		fprintf(stderr, "can't open /dev/mem\n");
		return NULL;
	}

	char *mapped_area =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, base);
	if (mapped_area == MAP_FAILED) {
		fprintf(stderr, "read_mem_reg mmap error: %s (%d)\n",
				strerror(errno), errno);
		return NULL;
	}

	uint32_t MISC_CTRL45 = 0;

	// Set the T-Sensor cyclic capture mode by configuring MISC_CTRL45 bit[30]
	MISC_CTRL45 |= 1 << 30;

	// Set the capture period by configuring MISC_CTRL45 bit[27:20]
	// The formula for calculating the cyclic capture periodis as follows:
	//	T = N x 2 (ms)
	//	N is the value of MISC_CTRL45 bit[27:20]
	MISC_CTRL45 |= 50 << 20;

	// Enable the T-Sensor by configuring MISC_CTRL45 bit[31] and start to collect the temperature
	MISC_CTRL45 |= 1 << 31;

	*(volatile uint32_t *)(mapped_area + 0xB4) = MISC_CTRL45;

	return mapped_area;
}

static void temp_read(evutil_socket_t sock, short event, void *arg)
{
	(void)sock;
	(void)event;
	char *mapped_area = arg;

	uint32_t val = *(volatile uint32_t *)(mapped_area + 0xBC);
	float tempo = val & ((1 << 16) - 1);
	//tempo = ((tempo - 117) / 798.0f) * 165.0f - 40.0f;
      tempo = ((tempo - 117) / 798) * 165 - 40;

	if (last_board_temp == -100)//only once
		printf("Temp read %f C\n", tempo);
	last_board_temp=tempo;
}
int VariantCounter=0;


static void send_variant_request2(int serial_fd) {
    uint8_t buffer[6];
	int res=0;


	if (rc_channel_mon_enabled && VariantCounter%5==1){//once every 5 cycles, 4 times per second
		construct_msp_command(buffer, MSP_RC, NULL, 0, MSP_OUTBOUND);
    	res = write(serial_fd, &buffer, sizeof(buffer));
	}else if (AHI_Enabled){
		if (AHI_Enabled==3 && VariantCounter%13==1){
			construct_msp_command(buffer, MSP_COMP_GPS, NULL, 0, MSP_OUTBOUND);
    		res = write(serial_fd, &buffer, sizeof(buffer));
		}else{//this is called at every POLL, skipping every 5th and every 13th, that is 15 times per second
			construct_msp_command(buffer, MSP_ATTITUDE, NULL, 0, MSP_OUTBOUND);
    		res = write(serial_fd, &buffer, sizeof(buffer));
			last_MSP_ATTITUDE=get_time_ms();
		}
	}
	
	if (MSP_PollRate <= ++VariantCounter){//poll every one second
		construct_msp_command(buffer, MSP_CMD_FC_VARIANT, NULL, 0, MSP_OUTBOUND);
		res = write(serial_fd, &buffer, sizeof(buffer));

		if (mspVTXenabled) {
			// Poll for mspVTX
			construct_msp_command(buffer, MSP_GET_VTX_CONFIG, NULL, 0, MSP_OUTBOUND);
			res = write(serial_fd, &buffer, sizeof(buffer));
		}		

		//poll for FC_STATUS
		construct_msp_command(buffer, MSP_CMD_STATUS, NULL, 0, MSP_OUTBOUND);
	    res = write(serial_fd, &buffer, sizeof(buffer));

		//usleep(20*1000);
		VariantCounter=0;
	}

	//construct_msp_command(buffer, MSP_CMD_BATTERY_STATE, NULL, 0, MSP_OUTBOUND);
    //res = write(serial_fd, &buffer, sizeof(buffer));

	//printf("Sent %d\n", res);
}

static void poll_msp(evutil_socket_t sock, short event, void *arg)
{
    int serial_fd = *((int *)arg);
	send_variant_request2(serial_fd);
}



static int handle_data(const char *port_name, int baudrate,
		       const char *out_addr )
{	
	struct event *sig_int = NULL, *in_ev = NULL, *temp_tmr = NULL, *msp_tmr=NULL;
	struct event *sig_term;
	int ret = EXIT_SUCCESS;
	 
	

	if (strlen(port_name) > 0 && port_name[0] >= '0' && port_name[0] <= '9') {//Read from UDP

		struct sockaddr_in sin_in = {
		.sin_family = AF_INET,
		};
		if (!parse_host_port(port_name, (struct port_name *)&sin_in.sin_addr.s_addr,
					&sin_in.sin_port))
			goto err;

		if (sin_in.sin_port>0)
			in_sock=socket(AF_INET, SOCK_DGRAM, 0);

		if (in_sock>0 && bind(in_sock, (struct sockaddr *)&sin_in, sizeof(sin_in))) {// we may not need this
			perror("bind()");
			exit(EXIT_FAILURE);
		}
		if(in_sock>0)
			printf("Listening UDP on %s...\n", port_name);

	}else{//Read from UART
		serial_fd = open(port_name, O_RDWR | O_NOCTTY);
		if (serial_fd < 0) {
			printf("Error while openning port %s: %s\n", port_name,
				strerror(errno));
			return EXIT_FAILURE;
		};
		evutil_make_socket_nonblocking(serial_fd);
		printf("Listening UART on %s...\n", port_name);
	}
	struct termios options;
	tcgetattr(serial_fd, &options);
	cfsetspeed(&options, speed_by_value(baudrate));

	options.c_cflag &= ~CSIZE; // Mask the character size bits
	options.c_cflag |= CS8; // 8 bit data
	options.c_cflag &= ~PARENB; // set parity to no
	options.c_cflag &= ~PARODD; // set parity to no
	options.c_cflag &= ~CSTOPB; // set one stop bit

	options.c_cflag |= (CLOCAL | CREAD);

	options.c_oflag &= ~OPOST;

	options.c_lflag &= 0;
	options.c_iflag &= 0; // disable software flow controll
	options.c_oflag &= 0;

	cfmakeraw(&options);
	tcsetattr(serial_fd, TCSANOW, &options);

	// tell the fc what vtx config we support
	if (mspVTXenabled) {
		printf("Setup mspVTX ...\n");
		msp_set_vtx_config(serial_fd);
	}

	if (strlen(out_addr)>1){
		out_sock = socket(AF_INET, SOCK_DGRAM, 0);

		if (!parse_host_port(out_addr,
					(struct in_addr *)&sin_out.sin_addr.s_addr,
					&sin_out.sin_port))
			goto err;
	}

	struct sockaddr_in sin_in = {
		.sin_family = AF_INET,
	};
 

	base = event_base_new();

	sig_int = evsignal_new(base, SIGINT, signal_cb, base);
	event_add(sig_int, NULL);

	// it's recommended by libevent authors to ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);

    // Handle SIGTERM (e.g., killall myapp)
    sig_term = evsignal_new(base, SIGTERM, signal_cb, base);
    event_add(sig_term, NULL);

	//Test inject a simple packet to test malvink communication Camera to Ground
	signal(SIGUSR1, sendtestmsg);

	if (serial_fd>0){//if UART opened...
		serial_bev = bufferevent_socket_new(base, serial_fd, 0);

		/* Trigger the read callback only whenever there is at least 16 bytes of data in the buffer. */
		//So that the read event is not triggered so often
		bufferevent_setwatermark(serial_bev, EV_READ, 16, 0);
		bufferevent_setcb(serial_bev, serial_read_cb, NULL, serial_event_cb,
				base);
		bufferevent_enable(serial_bev, EV_READ);	 
	}


	if (in_sock>0){
		//in_ev = event_new(base, in_sock, EV_READ | EV_PERSIST, in_read, NULL);
		//event_add(in_ev, NULL);
		 // Using bufferevent for the UDP socket too, for a unified callback
        struct bufferevent *udp_bev = bufferevent_socket_new(base, in_sock, 0);
        bufferevent_setcb(udp_bev, serial_read_cb, NULL, NULL, base);
        bufferevent_enable(udp_bev, EV_READ | EV_PERSIST);
	}



    // Set up an event for stdin (file descriptor 0)
#ifdef __KEYBOARD_INPUT__
    struct event *stdin_event = event_new(base, STDIN_FILENO, EV_READ | EV_PERSIST, stdin_read_callback, base);

    // Add the event to the event loop
    event_add(stdin_event, NULL);
#endif

	if (temp) {
		if (GetTempSigmaStar()>-90){
			temp=2;//SigmaStar
			printf("Found SigmaStart temp sensor\n");
		}else{		//Goke/Hisilicon method			
			void* mem = setup_temp_mem(0x12028000, 0xFFFF);
			temp_tmr = event_new(base, -1, EV_PERSIST, temp_read, mem);
			evtimer_add(temp_tmr, &(struct timeval){.tv_sec = 1});
		}
	}

   //MSP_PollRate
	if (ParseMSP && msp_tmr==NULL && serial_fd>0){ //Only if we are on Cam, on ground no need to poll
		msp_tmr = event_new(base, -1, EV_PERSIST, poll_msp, &serial_fd);
		 // Set poll interval to 50 milliseconds if pollrate is 20
    	struct timeval interval = {
	        .tv_sec = 0,         // 0 seconds
        	.tv_usec = 1000000/ MSP_PollRate   // 200 milliseconds (200,000 microseconds)
    	};

		evtimer_add(msp_tmr, &interval  /*(struct timeval){.tv_sec = 1}*/ /*&(struct timeval){.tv_usec = 5000000}*/);		
	}

	event_base_dispatch(base);
err:
	
if (temp_tmr ) {
		event_del(temp_tmr);
		event_free(temp_tmr);
	}
	if (out_sock>0)
		close(out_sock);
 
	if (serial_fd >= 0)
		close(serial_fd);

	if (serial_bev)
		bufferevent_free(serial_bev);

	if (in_ev) {
		event_del(in_ev);
		event_free(in_ev);
	}

	if (sig_int)
		event_free(sig_int);

	if (base)
		event_base_free(base);

	libevent_global_shutdown();
	
	CloseMSP();

	return ret;
}

void resetLastStartValues() {
	for (int i=0; i < kChannelCount; ++i) {
		LastStart[i] = get_current_time_ms();
	}
}

int main(int argc, char **argv)
{
	const struct option long_options[] = {
		{ "master", required_argument, NULL, 'm' },
		{ "baudrate", required_argument, NULL, 'b' },
		{ "out", required_argument, NULL, 'o' },
		{ "ahi", required_argument, NULL, 'a' },		
		{ "in", required_argument, NULL, 'i' },
		{ "channels", required_argument, NULL, 'c' },
		{ "wait_time", required_argument, NULL, 'w' },				
		{ "refreshrate", required_argument, NULL, 'r' },				
		{ "folder", required_argument, NULL, 'f' },						
		{ "persist", required_argument, NULL, 'p' },
		{ "matrix", required_argument, NULL, 'x' },
		{ "osd", no_argument, NULL, 'd' },	
		{ "verbose", no_argument, NULL, 'v' },		
		{ "temp", no_argument, NULL, 't' },						
		{ "wfb", no_argument, NULL, 'j' },
		{ "mspvtx", no_argument, NULL, '1' },							
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	const char *port_name = default_master;
	int baudrate = default_baudrate;
	const char *out_addr = defualt_out_addr;
	const char *in_addr = default_in_addr;
	MinTimeBetweenScreenRefresh=50;
	last_board_temp=-100;

	printf("Ver: %s\n", VERSION_STRING);
	int opt;
	int r;
	int long_index = 0;
	int rc_channel_no = 0;

	while ((opt = getopt_long_only(argc, argv, "", long_options, &long_index)) != -1) {
		switch (opt) {
		case 'm':
			port_name = optarg;	
			printf("Listen on port :%s\n",port_name);		
			break;
		case 'b':
			baudrate = atoi(optarg);
			break;
		case 'o':
			out_addr = optarg;
			break;
		case 'i':
			 
			break;
		case 'a':	
			AHI_Enabled = atoi(optarg);
						
		break;
		
		case 'c':
			rc_channel_no = atoi(optarg);
			if(rc_channel_no == 0) 
				printf("rc_channels  monitoring disabled\n");
			else 
			{
				printf("Monitoring RC channel %d \n", rc_channel_no);
				rc_channel_mon_enabled = true;
				// The array is zero-based, so substract 1 from the index:
				rc_channel_mon[rc_channel_no-1] = true;
			}
			resetLastStartValues();
			break;

		case 'f':
			 if (optarg != NULL) {        		
        		
        		snprintf(MavLinkMsgFile, sizeof(MavLinkMsgFile), "%smavlink.msg", optarg);
				snprintf(WfbLogFile, sizeof(MavLinkMsgFile), "%swfb.log", optarg);
			 }

			break;
		case 'w':
			wait_after_bash = atoi(optarg);			
			resetLastStartValues();
			break;

		case 'x':
			matrix_size = atoi(optarg);						
			break;

		case 'r':
			r=atoi(optarg);
			if (r>100){
				enable_fast_layout=true;
				r=r%100;
			}
			MinTimeBetweenScreenRefresh = 1000/atoi(optarg);			
			resetLastStartValues();
			break;

		case 'p':
			ChannelPersistPeriodMS = atoi(optarg);			
			resetLastStartValues();
			break;

		case 't':		
			temp = 1;//1  HiSilicon/Goke , 2 SigmaStar SOC
			break;

		case 'j':		
			monitor_wfb = true;
			break;
		case '1':		
			mspVTXenabled = true;
			break;
		case 'v':
			verbose = true;
			printf("Verbose mode!");
			break;	
		case 'd':
			DrawOSD=true;
			printf("MSP to OSD mode!");
			break;			
		case 'h':
		default:
			print_usage();
			return EXIT_SUCCESS;
		}
	}	

    

	if (ParseMSP){
 		//msp_process_data(rx_msp_state, serial_data[i]);
		rx_msp_state = calloc(1, sizeof(msp_state_t));   
		rx_msp_state->cb = &rx_msp_callback;  
		//if (DrawOSD)
		InitMSPHook();//We need to create screen overlay if we gonna show message on screen

		if (false){//Forwarding MSP enabled 
		//this opens the UDP port so that it can be used ith file descriptors
			socket_fd = bind_socket(MSP_PORT+1); 
			// Connect the socket to the target address and port so that we can debug
			struct sockaddr_in si_other;
			memset((char *)&si_other, 0, sizeof(si_other));
			si_other.sin_family = AF_INET;
			si_other.sin_port = htons(MSP_PORT);
			si_other.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Loopback address (localhost)

			if (connect(socket_fd, (struct sockaddr *)&si_other, sizeof(si_other)) == -1) {
				perror("Failed to connect");
				close(socket_fd);
				return 1;
			}
		}
		//loadfonts    	          
	}

	return handle_data(port_name, baudrate, out_addr);
}
