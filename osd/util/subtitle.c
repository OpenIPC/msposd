#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <event2/event.h>
#include <stdbool.h>
#include "subtitle.h"
#include "../../osd.h"

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + NAME_MAX + 1))

extern char air_unit_info_msg[255];
extern int msg_colour;
extern char ready_osdmsg[80];
extern uint16_t character_map[MAX_OSD_WIDTH][MAX_OSD_HEIGHT];
extern bool verbose;
uint32_t subtitle_start_time = 0; // Start time in milliseconds
uint32_t subtitle_current_time = 0; // Current FlightTime in seconds
uint32_t sequence_number = 1; // Subtitle sequence number
uint32_t last_flight_time_seconds = 0; // Store the last FlightTime in seconds
char* recording_dir = NULL;
FILE* srt_file = NULL;
FILE* osd_file = NULL;
char* srt_file_name = NULL;
char* osd_file_name = NULL;
bool recording_running = false;


// Function to write Walksnail OSD header
void write_osd_header(FILE *file) {
    uint8_t header[HEADER_BYTES] = {0};
    memcpy(header, current_fc_identifier, FC_TYPE_BYTES);  // Copy FC identifier
    // Add any additional header data here if needed

    fwrite(header, 1, HEADER_BYTES, file);
}

void remove_control_codes(char *str) {
    int i, j;
    for (i = 0, j = 0; str[i] != '\0'; i++) {
        // Check for &L or &F followed by two digits
        if (str[i] == '&' && (str[i+1] == 'L' || str[i+1] == 'F') && 
            str[i+2] >= '0' && str[i+2] <= '9' && 
            str[i+3] >= '0' && str[i+3] <= '9') {
            // Skip the &LXX or &FXX substring
            i += 3;
        } else {
            // Copy the character to the new position
            str[j++] = str[i];
        }
    }
    // Null-terminate the modified string
    str[j] = '\0';
}

void write_srt_file() {

    // Open the file if it hasn't been opened yet
    if (!srt_file) {
        srt_file = fopen(srt_file_name, "w");
        if (srt_file == NULL) {
            perror("Failed to open file");
            return;
        }
    }

    // Convert current time to seconds
    uint32_t current_flight_time_seconds = subtitle_current_time / 1000;

    // Only write if the FlightTime has changed by at least 1 second
    if (current_flight_time_seconds == last_flight_time_seconds) {
        return; // No change, do nothing
    }

    // Calculate start and end times in SRT format (HH:MM:SS,ms)
    uint32_t start_time_ms = current_flight_time_seconds * 1000; // Start time in milliseconds (aligned to the second)
    uint32_t end_time_ms = start_time_ms + 1000; // Each subtitle lasts 1 second

    uint32_t start_hours = start_time_ms / 3600000;
    uint32_t start_minutes = (start_time_ms % 3600000) / 60000;
    uint32_t start_seconds = (start_time_ms % 60000) / 1000;
    uint32_t start_milliseconds = start_time_ms % 1000;

    uint32_t end_hours = end_time_ms / 3600000;
    uint32_t end_minutes = (end_time_ms % 3600000) / 60000;
    uint32_t end_seconds = (end_time_ms % 60000) / 1000;
    uint32_t end_milliseconds = end_time_ms % 1000;

    // Write the subtitle to the file
    fprintf(srt_file, "%u\n", sequence_number);
    fprintf(srt_file, "%02u:%02u:%02u,%03u --> %02u:%02u:%02u,%03u\n",
            start_hours, start_minutes, start_seconds, start_milliseconds,
            end_hours, end_minutes, end_seconds, end_milliseconds);
    if (msg_colour) { // ground mode
        fprintf(srt_file, "%s\n\n", air_unit_info_msg);
    } else { // air mode
        remove_control_codes(ready_osdmsg);
        fprintf(srt_file, "%s\n\n", ready_osdmsg);
    }
    fflush(srt_file);

    // Increment the sequence number and update the last FlightTime written
    sequence_number++;
    last_flight_time_seconds = current_flight_time_seconds;
}

void handle_osd_out() {
    if (! osd_file) 
		osd_file = fopen(osd_file_name, "wb");
    if (osd_file) {
        if (subtitle_start_time == 0) {
            write_osd_header(osd_file);
            subtitle_start_time = (uint32_t)get_time_ms();
        }

        // Calculate elapsed time since subtitle_start_time
        subtitle_current_time = (uint32_t)get_time_ms() - subtitle_start_time;

        fwrite(&subtitle_current_time, sizeof(uint32_t), 1, osd_file);
        // Write OSD data
        for (int y = 0; y < MAX_OSD_HEIGHT; y++) {
            for (int x = 0; x < MAX_OSD_WIDTH -1; x++) { // -1 ??? no clue why
                // Write glyph (2 bytes)
                fwrite(&character_map[x][y], sizeof(uint16_t), 1, osd_file);
            }
        }
        fflush(osd_file);
	}
}

int inotify_fd;
int watch_desc;
void setup_recording_watch(char *file_to_watch) {
    inotify_fd = inotify_init();
    if (inotify_fd == -1) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }

    // Set the inotify file descriptor to non-blocking mode
    int flags = fcntl(inotify_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(inotify_fd);
        exit(EXIT_FAILURE);
    }
    if (fcntl(inotify_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(inotify_fd);
        exit(EXIT_FAILURE);
    }

    watch_desc = inotify_add_watch(inotify_fd, file_to_watch, IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
    if (watch_desc == -1) {
        perror("inotify_add_watch");
        close(inotify_fd);
        exit(EXIT_FAILURE);
    }

    printf("Watching %s for close events (non-blocking mode)...\n", file_to_watch);    

}

void check_recoding_file() {
    char buffer[BUF_LEN];
    ssize_t len = read(inotify_fd, buffer, BUF_LEN);
    if (len == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available, continue polling
            return;
        } else {
            perror("read");
            return;            
        }
    }

    for (char *ptr = buffer; ptr < buffer + len; ) {
        struct inotify_event *event = (struct inotify_event *) ptr;

        if (event->mask & IN_CLOSE_WRITE || event->mask & IN_CLOSE_NOWRITE) {
            printf("Stopping OSD/STR writeing\n");
            if (srt_file) {
                fclose(srt_file);
                srt_file = NULL;
            }
            if (osd_file) {
                fclose(osd_file);
                osd_file = NULL;
            }
            recording_running = false;
        }
        ptr += EVENT_SIZE + event->len;
    }

    inotify_rm_watch(inotify_fd, watch_desc);
    close(inotify_fd);
}

// Function to handle new file creation
void handle_new_file(char* filename) {
    printf("New recording detected: %s\r\n", filename);

    // detected a new recodring, closeing current files
    if (srt_file) {
        fclose(srt_file);
        srt_file = NULL;
    }
    if (osd_file) {
        fclose(osd_file);
        osd_file = NULL;
    }

    // reset values
    subtitle_start_time = 0;
    subtitle_current_time = 0;
    sequence_number = 1;
    last_flight_time_seconds = 0;

    setup_recording_watch(filename);

    // Free any previously allocated memory to avoid memory leaks
    free(srt_file_name);
    free(osd_file_name);

    // Remove the suffix
    char* dot = strrchr(filename, '.');
    if (dot) {
        *dot = '\0';
    }    

    // Allocate memory for the new filenames
    srt_file_name = (char*)malloc(strlen(filename) + 5); // +5 for ".srt" and null terminator
    osd_file_name = (char*)malloc(strlen(filename) + 5); // +5 for ".osd" and null terminator

    if (srt_file_name == NULL || osd_file_name == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }

    // Create the new filenames
    snprintf(srt_file_name, strlen(filename) + 5, "%s.srt", filename);
    snprintf(osd_file_name, strlen(filename) + 5, "%s.osd", filename);    

	if (verbose) {
        printf("srt file: %s\r\n", srt_file_name);    
        printf("osd file: %s\r\n", osd_file_name);    
    }

    recording_running = true;
}

// Callback function for inotify events
void inotify_callback(evutil_socket_t fd, short events, void* arg) {
    char buffer[BUF_LEN];
    ssize_t length = read(fd, buffer, BUF_LEN);

    if (length < 0) {
        perror("read");
        return;
    }

    // Process inotify events
    for (char* ptr = buffer; ptr < buffer + length; ) {
        struct inotify_event* event = (struct inotify_event*) ptr;

        if (event->mask & IN_CREATE) {
            // Construct the full path
            char filename[PATH_MAX];
            snprintf(filename, PATH_MAX, "%s/%s", (const char*) arg, event->name);

            // Filter file names
            if (strstr(event->name, ".mp4") != NULL) {
                // Handle the new file
                handle_new_file(filename);
            } else {
                printf("Ignoring non-.mp4 file: %s\n", event->name);
            }
        }

        ptr += EVENT_SIZE + event->len;
    }
}
