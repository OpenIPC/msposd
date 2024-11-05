#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "interface.h"

#define BUFFER_SIZE 1024

FrequencyChannel fc_list[MAX_ENTRIES];  // Array to store frequency-channel pairs

// Function to extract frequency and channel from a line
void parse_line(char *line, FrequencyChannel *fc, int *count) {
    int frequency = 0;
    int channel = 0;

    // Skip lines containing "disabled"
    if (strstr(line, "disabled") != NULL) {
        return;
    }    

    // Try to match the line to the format: * <frequency> MHz [<channel>] (dBm info)
    if (sscanf(line, " * %d MHz [%d]", &frequency, &channel) == 2) {
        // Store the parsed values in the structure
        fc[*count].frequency = frequency;
        fc[*count].channel = channel;
        (*count)++;  // Increment the count of entries
    }
}

int query_interface_for_available_frequencies() {
    FILE *fp;
    char buffer[BUFFER_SIZE];
    int count = 0;
    int in_frequencies_section = 0;

    // Open a pipe to the `iw list` command
    fp = popen("iw list", "r");
    if (fp == NULL) {
        perror("popen failed");
        return 1;
    }

    // Read the output line by line
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // Check if we've entered the Frequencies section
        if (strstr(buffer, "Frequencies") != NULL) {
            in_frequencies_section = 1;
        }

        // If we're in the Frequencies section, parse the lines with frequency/channel info
        if (in_frequencies_section && strstr(buffer, " MHz [") != NULL) {
            parse_line(buffer, fc_list, &count);
        }

        // Stop parsing if we hit an empty line after the Frequencies section
        if (in_frequencies_section && strlen(buffer) == 1) {
            break;
        }
    }

    // Close the pipe
    pclose(fp);

    // Print the stored frequency-channel pairs
    printf("Stored Frequency-Channel Pairs:\n");
    for (int i = 0; i < count; i++) {
        printf("Frequency: %d MHz, Channel: %d\n", fc_list[i].frequency, fc_list[i].channel);
        if (i < FREQ_TABLE_SIZE) {
            //printf("Adding Frequency: %d MHz\n", freq);
            channelFreqTable[i] = fc_list[i].frequency;
        } else {
            printf("Skipping Frequency: %d MHz vtxtable is full\n", fc_list[i].frequency);
        }
    }

    if ( count > 0 ) {
        // at least we found one supported channel on the interface
        // now our default band names are useless
        // fill with gerneric band name
        for (int band_index = 0; band_index < FREQ_LABEL_SIZE; band_index += BAND_NAME_LENGTH) {
            channelFreqLabel[band_index]     = 'B';
            channelFreqLabel[band_index + 1] = 'A';
            channelFreqLabel[band_index + 2] = 'N';
            channelFreqLabel[band_index + 3] = 'D';
            channelFreqLabel[band_index + 4] = ' ';
            channelFreqLabel[band_index + 5] = '1' + (band_index / BAND_NAME_LENGTH); // This appends the number '1' through '8'
            channelFreqLabel[band_index + 6] = ' ';
            channelFreqLabel[band_index + 7] = ' ';
        }
        // same goes for channel bandLetter
        for ( int band_index = 0 ; band_index < BAND_COUNT; band_index++) {
            bandLetter[band_index] = '1' + band_index;
        }
    }
    printf("mspVTX: Total %i out of %i used.\n",count,FREQ_TABLE_SIZE);    

    return 0;
}

int read_current_freq_from_interface(char *interface) {

    FILE *fp;
    char command[100];
    char buffer[256];
    const char *freq_keyword = "Frequency:";
    char *freq_start;
    float frequency = 0.0;

    // Create the command string dynamically based on the interface
    snprintf(command, sizeof(command), "iwconfig %s", interface);

    // Run the iwconfig command and open a pipe to read its output
    fp = popen(command, "r");
    if (fp == NULL) {
        printf("Failed to run iwconfig on interface %s\n", interface);
        return -1;
    }

    // Read the command output line by line
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // Search for the line containing "Frequency:"
        freq_start = strstr(buffer, freq_keyword);
        if (freq_start) {
            freq_start += strlen(freq_keyword);  // Move past "Frequency:"
            int x = sscanf(freq_start, "%f", &frequency); // Extract the float value
            pclose(fp); // Close the pipe
            return (int)(frequency * 1000); // Convert GHz to MHz
        }
    }

    // Close the pipe
    pclose(fp);
    return -1; // Return -1 if no frequency is found
}

void set_frequency(char *interface, int channel) {
    // Create a command string to set the channel using iwconfig
    char command[100];

    // Format the command to set the channel
    snprintf(command, sizeof(command), "iwconfig %s channel %d", interface, channel);

    // Execute the command
    int result = system(command);

    // Check if the command was executed successfully
    if (result == 0) {
        printf("Frequency set successfully on %s to channel %i\n", interface, channel);
    } else {
        printf("Failed to set channel on %s\n", interface);
    }
}
