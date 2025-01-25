#include "msp.h"
#include "../util/interface.h"
#include "../util/settings.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern bool verbose;
bool vtxInitDone = false;

uint8_t channelFreqLabel[FREQ_LABEL_SIZE] = {
	'B', 'A', 'N', 'D', '_', 'A', ' ', ' ', // A
	'B', 'A', 'N', 'D', '_', 'B', ' ', ' ', // B
	'B', 'A', 'N', 'D', '_', 'E', ' ', ' ', // E
	'F', 'A', 'T', 'S', 'H', 'A', 'R', 'K', // F
	'R', 'A', 'C', 'E', ' ', ' ', ' ', ' ', // R
	'R', 'A', 'C', 'E', '_', 'L', 'O', 'W', // L
	'W', 'L', 'A', 'N', 'C', 'H', 'A', 'N', // WLAN
};

uint8_t bandLetter[BAND_COUNT] = {'A', 'B', 'E', 'F', 'R', 'L', 'W'};

uint16_t channelFreqTable[FREQ_TABLE_SIZE] = {
	5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, // A
	5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, // B
	5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, // E
	5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, // F
	5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, // R
	5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621, // LowRace
	5660, 5700, 5720, 5765, 5805, 5845, 5885,
	5925 // WLAN CHANNEL: 132, 140, 144, 153, 161, 169, 177, 185
};

uint8_t saPowerLevelsLut[NUM_POWER_LEVELS] = {20, 25, 30, 35, 45, 50, 55, 58};

uint8_t saPowerLevelsLabel[NUM_POWER_LEVELS * POWER_LEVEL_LABEL_LENGTH] = {'2', '0', ' ', '2', '5',
	' ', '3', '0', ' ', '3', '5', ' ', '4', '5', ' ', '5', '0', ' ', '5', '5', ' ', '5', '8', ' '};

int frequency = 0;

uint16_t msp_data_from_msg(uint8_t message_buffer[], msp_msg_t *msg) {
	// return size
	construct_msp_command(message_buffer, msg->cmd, msg->payload, msg->size, msg->direction);
	return msg->size + 6;
}

void wipeVtxTable(int serial_fd, int band, int channel) {

	// lets first wipe all
	uint8_t payload[15];
	payload[0] = 0;	 // idx LSB
	payload[1] = 0;	 // idx MSB
	payload[2] = 0;	 // power
	payload[3] = 0;	 // pitmode
	payload[4] = 0;	 // lowPowerDisarm
	payload[5] = 0;	 // pitModeFreq LSB
	payload[6] = 0;	 // pitModeFreq MSB
	payload[7] = 0;	 // newBand - WLAN Channel
	payload[8] = 0;	 // newChannel - 161
	payload[9] = 0;	 // newFreq  LSB
	payload[10] = 0; // newFreq  MSB
	payload[11] = 0; // newBandCount
	payload[12] = 0; // newChannelCount
	payload[13] = 0; // newPowerCount
	payload[14] = 1; // vtxtable should be cleared

	uint8_t buffer[256];

	construct_msp_command(buffer, MSP_SET_VTX_CONFIG, payload, sizeof(payload), MSP_OUTBOUND);
	write(serial_fd, &buffer, sizeof(buffer));

	// now prepare the table
	payload[0] = 0;					// idx LSB
	payload[1] = 0;					// idx MSB
	payload[2] = 7;					// 55, default wfb-ng
	payload[3] = 0;					// pitmode
	payload[4] = 0;					// lowPowerDisarm
	payload[5] = 0;					// pitModeFreq LSB
	payload[6] = 0;					// pitModeFreq MSB
	payload[7] = band;				// newBand - WLAN Channel
	payload[8] = channel;			// newChannel - 161
	payload[9] = 0;					// newFreq  LSB
	payload[10] = 0;				// newFreq  MSB
	payload[11] = BAND_COUNT;		// newBandCount
	payload[12] = CHANNEL_COUNT;	// newChannelCount
	payload[13] = NUM_POWER_LEVELS; // newPowerCount
	payload[14] = 0;				// vtxtable should be cleared

	construct_msp_command(buffer, MSP_SET_VTX_CONFIG, payload, sizeof(payload), MSP_OUTBOUND);
	write(serial_fd, &buffer, sizeof(buffer));
}

void setVtxTableBand(int serial_fd, uint8_t band) {

	// see: OpenVTX

	uint8_t buffer[256];
	uint8_t payload2[29];

	payload2[0] = band;				// band
	payload2[1] = BAND_NAME_LENGTH; // BAND_NAME_LENGTH

	for (uint8_t i = 0; i < BAND_NAME_LENGTH; i++) {
		payload2[2 + i] = channelFreqLabel[(band - 1) * BAND_NAME_LENGTH + i];
	}

	payload2[2 + BAND_NAME_LENGTH] = bandLetter[band - 1];
	payload2[3 + BAND_NAME_LENGTH] = IS_FACTORY_BAND;
	payload2[4 + BAND_NAME_LENGTH] = CHANNEL_COUNT;

	int i;
	for (i = 0; i < CHANNEL_COUNT; i++) {
		payload2[(5 + BAND_NAME_LENGTH) + (i * 2)] =
			channelFreqTable[((band - 1) * CHANNEL_COUNT) + i] & 0xFF;
		payload2[(6 + BAND_NAME_LENGTH) + (i * 2)] =
			(channelFreqTable[((band - 1) * CHANNEL_COUNT) + i] >> 8) & 0xFF;
	}

	construct_msp_command(buffer, MSP_SET_VTXTABLE_BAND, payload2, sizeof(payload2), MSP_OUTBOUND);
	write(serial_fd, &buffer, sizeof(buffer));
}

void setVtxTablePowerLevel(int serial_fd, uint8_t idx) {

	// see: OpenVTX

	uint8_t buffer[256];
	uint8_t payload2[7];

	payload2[0] = idx;
	payload2[1] = saPowerLevelsLut[idx - 1] & 0xFF;		   // powerValue LSB
	payload2[2] = (saPowerLevelsLut[idx - 1] >> 8) & 0xFF; // powerValue MSB
	payload2[3] = POWER_LEVEL_LABEL_LENGTH;
	payload2[4] = saPowerLevelsLabel[((idx - 1) * POWER_LEVEL_LABEL_LENGTH) + 0];
	payload2[5] = saPowerLevelsLabel[((idx - 1) * POWER_LEVEL_LABEL_LENGTH) + 1];
	payload2[6] = saPowerLevelsLabel[((idx - 1) * POWER_LEVEL_LABEL_LENGTH) + 2];

	construct_msp_command(
		buffer, MSP_SET_VTXTABLE_POWERLEVEL, payload2, sizeof(payload2), MSP_OUTBOUND);
	write(serial_fd, &buffer, sizeof(buffer));
}

void msp_set_vtx_config(int serial_fd) {

	// mspVTX read current frequency setting
	query_interface_for_available_frequencies();

	frequency = read_current_freq_from_interface(read_setting("/etc/wfb.conf", "wlan"));

	int band = 6;
	int channel = 3;
	for (int index = 0; index < FREQ_TABLE_SIZE; index++) {
		if (channelFreqTable[index] == frequency) {
			// Determine band and channel using division and modulo
			band = index / 8 + 1;	   // Which band (integer division)
			channel = (index % 8) + 1; // Channel within the band (modulo)

			if (verbose)
				printf("Frequency %d found in band %i, channel %d\n", frequency, band, channel);
			break; // Exit after finding the frequency
		}
	}

	wipeVtxTable(serial_fd, band, channel);

	for (int i = 1; i <= BAND_COUNT; i++) {
		setVtxTableBand(serial_fd, i);
	}
	for (int i = 1; i <= NUM_POWER_LEVELS; i++) {
		setVtxTablePowerLevel(serial_fd, i);
	}
	vtxInitDone = true;
}

msp_error_e construct_msp_command(uint8_t message_buffer[], uint8_t command, uint8_t payload[],
	uint8_t size, msp_direction_e direction) {
	uint8_t checksum;
	message_buffer[0] = '$'; // Header
	message_buffer[1] = 'M'; // MSP V1
	if (direction == MSP_OUTBOUND) {
		message_buffer[2] = '<';
	} else {
		message_buffer[2] = '>';
	}
	message_buffer[3] = size; // Payload Size
	checksum = size;
	message_buffer[4] = command; // Command
	checksum ^= command;
	for (uint8_t i = 0; i < size; i++) {
		message_buffer[5 + i] = payload[i];
		checksum ^= message_buffer[5 + i];
	}
	message_buffer[5 + size] = checksum;
	return 0;
}

msp_error_e msp_process_data(msp_state_t *msp_state, uint8_t dat) {
	switch (msp_state->state) {
	default:
	case MSP_IDLE: // look for begin
		if (dat == '$') {
			msp_state->state = MSP_VERSION;
		} else {
			return MSP_ERR_HDR;
		}
		break;
	case MSP_VERSION: // Look for 'M' (MSP V1, we do not support V2 at this
					  // time)
		if (dat == 'M') {
			msp_state->state = MSP_DIRECTION;
		} else { // Got garbage instead, try again
			msp_state->state = MSP_IDLE;
			return MSP_ERR_HDR;
		}
		break;
	case MSP_DIRECTION: // < for command, > for reply
		msp_state->state = MSP_SIZE;
		switch (dat) {
		case '<':
			msp_state->message.direction = MSP_OUTBOUND;
			break;
		case '>':
			msp_state->message.direction = MSP_INBOUND;
			break;
		default: // garbage, try again
			msp_state->state = MSP_IDLE;
			return MSP_ERR_HDR;
			break;
		}
		break;
	case MSP_SIZE: // next up is supposed to be size
		msp_state->message.checksum = dat;
		msp_state->message.size = dat;
		msp_state->state = MSP_CMD;
		if (msp_state->message.size > 256) { // bogus message, too big. this can't
											 // actually happen but good to check
			msp_state->state = MSP_IDLE;
			return MSP_ERR_LEN;
			break;
		}
		break;
	case MSP_CMD: // followed by command
		msp_state->message.cmd = dat;
		msp_state->message.checksum ^= dat;
		msp_state->buf_ptr = 0;
		if (msp_state->message.size > 0) {
			msp_state->state = MSP_PAYLOAD;
		} else {
			msp_state->state = MSP_CHECKSUM;
		}
		break;
	case MSP_PAYLOAD: // if we had a payload, keep going
		msp_state->message.payload[msp_state->buf_ptr] = dat;
		msp_state->message.checksum ^= dat;
		msp_state->buf_ptr++;
		if (msp_state->buf_ptr == msp_state->message.size) {
			msp_state->buf_ptr = 0;
			msp_state->state = MSP_CHECKSUM;
		}
		break;
	case MSP_CHECKSUM:
		if (msp_state->message.checksum == dat) {
			// printf("MSP: %s\n", msp_state->message.payload);
			if (msp_state->cb != 0) {
				msp_state->cb(&msp_state->message);
			}
			memset(&msp_state->message, 0, sizeof(msp_msg_t));
			msp_state->state = MSP_IDLE;
			break;
		} else {
			msp_state->state = MSP_IDLE;
			return MSP_ERR_CKS;
		}
		break;
	}
	return MSP_ERR_NONE;
}