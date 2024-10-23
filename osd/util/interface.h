#pragma once
#include <stdint.h>
#include "../msp/msp.h"

#define MAX_ENTRIES 100  // Maximum number of frequency-channel pairs to store

extern uint8_t channelFreqLabel[FREQ_TABLE_SIZE];
extern uint8_t bandLetter[BAND_COUNT];
extern uint16_t channelFreqTable[FREQ_TABLE_SIZE];

// Structure to hold frequency and channel
typedef struct {
    int frequency;
    int channel;
} FrequencyChannel;

// Function to extract frequency and channel from a line
void parse_line(char *line, FrequencyChannel *fc, int *count);

int query_interface_for_available_frequencies();

void set_frequency(char *interface, int channel);

int read_current_freq_from_interface(char *interface);
