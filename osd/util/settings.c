#include "settings.h"
#include <stdio.h>
#include <string.h>

// Function to read a value for a given key from the config file (dynamically
// allocate memory)
char *read_setting(const char *filename, char *key) {
	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		printf("Error: Could not open file %s\n", filename);
		return NULL;
	}

	char line[MAX_LINE_LENGTH];
	char *value = NULL;

	while (fgets(line, sizeof(line), file)) {
		// Ignore comment lines starting with '#'
		if (line[0] == '#') {
			continue;
		}

		char file_key[MAX_LINE_LENGTH];
		char file_value[MAX_LINE_LENGTH];

		// Parse key=value pairs from the file
		if (sscanf(line, "%[^=]=%s", file_key, file_value) == 2) {
			// Check if the key matches
			if (strcmp(file_key, key) == 0) {
				// Dynamically allocate memory for the value
				value = strdup(file_value); // or custom memory allocation
				fclose(file);
				return value;
			}
		}
	}

	fclose(file);
	printf("Error: Key %s not found in file\n", key);
	return NULL;
}

// Function to update or add a key-value pair in the config file
int write_setting(const char *filename, const char *key, const char *new_value) {

	// Create a temp file in the same directory as the original file
	char temp_filename[MAX_LINE_LENGTH];
	snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);

	FILE *file = fopen(filename, "r");
	FILE *temp_file = fopen(temp_filename, "w");

	if (file == NULL || temp_file == NULL) {
		printf("Error: Could not open file\n");
		return -1;
	}

	char line[MAX_LINE_LENGTH];
	int found_key = 0;

	while (fgets(line, sizeof(line), file)) {
		char file_key[MAX_LINE_LENGTH];
		char file_value[MAX_LINE_LENGTH];

		// If it's a key=value line, check if the key matches
		if (sscanf(line, "%[^=]=%s", file_key, file_value) == 2) {
			if (strcmp(file_key, key) == 0) {
				// Write the updated key=value pair
				fprintf(temp_file, "%s=%s\n", key, new_value);
				found_key = 1;
			} else {
				// Otherwise, copy the line as is
				fprintf(temp_file, "%s", line);
			}
		} else {
			// Copy comments and other non key-value lines
			fprintf(temp_file, "%s", line);
		}
	}

	// If key was not found, append it at the end
	if (!found_key) {
		fprintf(temp_file, "%s=%s\n", key, new_value);
	}

	fclose(file);
	fclose(temp_file);

	// Replace original file with updated file
	remove(filename);
	rename(temp_filename, filename);

	return 0;
}
