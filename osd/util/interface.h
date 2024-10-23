#pragma once

#if defined(__GOKE__) || defined(__INFINITY6B0__)
// Ensure the compiler supports the necessary features
#include <limits.h>
#include <stddef.h>

// Define exact-width unsigned integer types

typedef unsigned char   uint8_t;   // 8-bit unsigned integer
typedef unsigned short  uint16_t;  // 16-bit unsigned integer
typedef unsigned int    uint32_t;  // 32-bit unsigned integer
typedef unsigned long long uint64_t; // 64-bit unsigned integer

// Define minimum and maximum values for these types
#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

// __GOKE__ end
#else
#include <bits/stdint-uintn.h>
#endif


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
