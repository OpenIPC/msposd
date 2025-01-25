#include "ini_parser.h"
#include "../msp/vtxmenu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern MenuSection *current_section;

// Helper function to split the list of values in a comma-separated list
void split_values(
	const char *values, char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH], int *value_count) {
	char temp[MAX_LINE_LENGTH]; // Temporary buffer to hold a copy of the
								// original string
	strncpy(temp, values,
		sizeof(temp) - 1);		   // Copy the original string to the temp buffer
	temp[sizeof(temp) - 1] = '\0'; // Ensure null termination

	char *token = strtok(temp, ",");
	*value_count = 0; // Initialize the value count

	while (token != NULL) {
		if (*value_count < MAX_VALUE_LIST_ITEMS) { // Check if there's space in the value list
			// Copy each token to the value_list with proper length checking
			strncpy(value_list[*value_count], token,
				MAX_VALUE_LENGTH - 1);							   // Copy with limit
			value_list[*value_count][MAX_VALUE_LENGTH - 1] = '\0'; // Ensure null termination
			(*value_count)++;									   // Increment the count
		} else {
			// Optionally handle case when too many values are provided
			printf("Warning: Maximum value count exceeded. Some values may be "
				   "ignored.\n");
			break; // Exit if the limit is reached
		}
		token = strtok(NULL, ","); // Get the next token
	}
}

void print_menu_system_state(MenuSystem *menu_system) {
	printf("\n=== Menu System Internal State ===\n");
	printf("We have %i sections:\n", menu_system->section_count);
	// Iterate over each section in the menu system
	for (int i = 0; i < menu_system->section_count; i++) {
		MenuSection *section = &menu_system->sections[i];
		printf("\nSection: %s\n", section->name);

		// Iterate over each option in the section
		for (int j = 0; j < section->option_count; j++) {
			MenuOption *option = &section->options[j];
			printf("  Option: %s (Internal Name: %s)", option->lable, option->name);
			switch (option->type) {
			case MENU_OPTION_LIST: {
				// If it's a list, display the current selected value
				char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
				int value_count;
				split_values(option->values, value_list, &value_count);
				printf(" (List): Current Value: %s Values: %s (Index: %d)\n",
					value_list[section->current_value_index[j]], option->values,
					section->current_value_index[j]);
				break;
			}
			case MENU_OPTION_RANGE: {
				// If it's a range, display the current value
				printf(" (Range): Current Value: %d (Min: %d, Max: %d)\n",
					section->current_value_index[j], option->min, option->max);
				break;
			}
			case MENU_OPTION_FLOATRANGE: {
				// If it's a range, display the current value
				printf(" (FloatRange): Current Value: %.1f (Min: %.1f, Max: "
					   "%.1f)\n",
					section->current_value_index[j] / 10.0f, option->min / 10.0f,
					option->max / 10.0f);
				break;
			}
			case MENU_OPTION_SUBMENU: {
				// If it's a submenu link, display the name of the linked
				// submenu
				printf(" (Submenu Link): Links to: %s\n", option->name);
				break;
			}
			case MENU_OPTION_COMMAND: {
				// If it's a submenu link, display the name of the linked
				// submenu
				if (option->command_function == runCustomCommand)
					printf(" (Command): Command to run: %s\n", option->read_command);
				else
					printf(" (Command): %s\n", option->lable);
				break;
			}
			default: {
				// Generic case for unsupported option types (if any)
				printf(" (Unknown Option Type)\n");
				break;
			}
			}
		}
	}
	printf("\n=================================\n");
}

// Function to run a shell command and capture the output
void run_command(const char *command, char *output, int output_size) {
	FILE *fp;
	printf("Running command: %s\n", command);
	if ((fp = popen(command, "r")) == NULL) {
		printf("Error running command: %s\n", command);
		return;
	}
	fgets(output, output_size, fp); // Capture output
	pclose(fp);
}

// Function to execute the save command with the given value
void save_value_to_system(const char *save_command, const char *value) {
	char formatted_command[256];	// Buffer to hold the final formatted command
	const char *placeholder = "{}"; // Define the placeholder
	const char *placeholder_pos =
		strstr(save_command, placeholder); // Find the position of the placeholder

	if (placeholder_pos) {
		// Copy the part before the placeholder
		int prefix_length = placeholder_pos - save_command;
		strncpy(formatted_command, save_command, prefix_length);
		formatted_command[prefix_length] = '\0'; // Null-terminate the string

		// Append the value
		strncat(
			formatted_command, value, sizeof(formatted_command) - strlen(formatted_command) - 1);

		// Append the part after the placeholder
		strncat(formatted_command, placeholder_pos + strlen(placeholder),
			sizeof(formatted_command) - strlen(formatted_command) - 1);
	} else {
		// If there's no placeholder, just copy the command as-is
		strncpy(formatted_command, save_command, sizeof(formatted_command) - 1);
		formatted_command[sizeof(formatted_command) - 1] = '\0'; // Ensure null termination
	}

	// Debug print to show the full command that will be executed
	printf("Executing command: %s\n", formatted_command);

	// Execute the formatted command
	int result = system(formatted_command);

	// Check the result of the system call
	if (result != 0) {
		printf("Error: Command failed with code %d\n", result);
	}
}

// Function to save all current values of options in a section
void save_all_changes() {

	MenuSection *section = current_section;

	for (int i = 0; i < section->option_count; i++) {
		MenuOption *option = &section->options[i];

		// Only attempt to save if there's a save_command
		if (strlen(option->save_command) > 0) {
			char formatted_command[256];  // Buffer for formatted save command
			char value[MAX_VALUE_LENGTH]; // Buffer for current value

			// Get the current value depending on the type of option
			switch (option->type) {
			case MENU_OPTION_LIST: {
				// For list options, get the currently selected value
				char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
				int value_count;
				split_values(option->values, value_list, &value_count);
				strncpy(value, value_list[section->current_value_index[i]], sizeof(value));
				value[sizeof(value) - 1] = '\0'; // Ensure null termination
				break;
			}
			case MENU_OPTION_RANGE:
				// For range options, just store the current value as a string
				snprintf(value, sizeof(value), "%d", section->current_value_index[i]);
				break;
			case MENU_OPTION_FLOATRANGE:
				// For range options, just store the current value as a string
				snprintf(value, sizeof(value), "%.1f", section->current_value_index[i] / 10.0f);
				break;
			default:
				continue; // Skip other option types
			}

			// Execute the save command with the formatted value
			save_value_to_system(option->save_command, value);
		}
	}
}

void add_option(
	MenuSection *section, const char *name, const char *label, void (*command_function)(void *)) {
	MenuOption option;
	// Copy name and label dynamically
	strncpy(option.name, name, MAX_NAME_LENGTH - 1);
	option.name[MAX_NAME_LENGTH - 1] = '\0'; // Ensure null termination
	strncpy(option.lable, label, MAX_LABLE_LENGTH - 1);
	option.lable[MAX_LABLE_LENGTH - 1] = '\0'; // Ensure null termination
	option.type = MENU_OPTION_COMMAND;
	option.command_function = command_function; // Function to save all changes

	// Add the save option to the section's list of options
	section->options[section->option_count++] = option;
}

int parse_ini(const char *filename, MenuSystem *menu_system) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		return -1; // Error opening file
	}

	char line[MAX_LINE_LENGTH];
	MenuSection *current_section = NULL;

	menu_system->section_count = 0;

	while (fgets(line, sizeof(line), file)) {
		// Remove newline character
		line[strcspn(line, "\n")] = 0;

		// Ignore empty lines
		if (strlen(line) == 0) {
			continue;
		}

		// Ignore commented lines
		if (line[0] == ';' || line[0] == '#') {
			continue;
		}

		// Check for section header
		if (line[0] == '[') {
			current_section = &menu_system->sections[menu_system->section_count++];
			sscanf(line + 1, "%[^]]", current_section->name);
			current_section->option_count = 0;
		}
		// Check for Options
		else if (line[0] == 'O') {
			char option_name[MAX_NAME_LENGTH];
			char option_lable[MAX_LABLE_LENGTH];
			char option_value[MAX_VALUE_LENGTH];
			char read_command[MAX_VALUE_LENGTH] = "";
			char save_command[MAX_VALUE_LENGTH] = "";

			// Updated format: Option1=Label:Values:ReadCommand:SaveCommand
			sscanf(line, "%[^=]=%[^:]:%[^:]:%[^:]:%[^\n]", option_name, option_lable, option_value,
				read_command, save_command);

			// Add option to the current section
			MenuOption *option = &current_section->options[current_section->option_count++];
			strcpy(option->name, option_name);
			strcpy(option->lable, option_lable);
			strcpy(option->values, option_value);
			strcpy(option->read_command, read_command);
			strcpy(option->save_command, save_command);

			// Check if the value is a list or a range and initialize current
			// value index
			if (strchr(option_value, ',')) {
				option->type = MENU_OPTION_LIST;
				current_section->current_value_index[current_section->option_count - 1] =
					0; // Default to the first list item
			} else if (strstr(option_value, "-")) {
				if (strstr(option_value, ".")) {
					option->type = MENU_OPTION_FLOATRANGE;
					float min_value, max_value;
					sscanf(option_value, "%f-%f", &min_value, &max_value);
					option->min = (int)(min_value * 10);
					option->max = (int)(max_value * 10);
					current_section->current_value_index[current_section->option_count - 1] =
						option->min; // Default to min value
				} else {
					option->type = MENU_OPTION_RANGE;
					sscanf(option_value, "%d-%d", &option->min, &option->max);
					current_section->current_value_index[current_section->option_count - 1] =
						option->min; // Default to min value
				}
			}
			// Read Values from system
			if (strlen(option->read_command) > 0) {
				char output[MAX_VALUE_LENGTH] = "";
				run_command(option->read_command, output, sizeof(output));

				// Process the output to match the current value index in the
				// list or range
				output[strcspn(output, "\n")] = '\0'; // Remove the newline from output

				// For list options, match the output to the corresponding index
				switch (option->type) {
				case MENU_OPTION_LIST: {
					char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
					int value_count;
					split_values(option->values, value_list, &value_count);

					for (int i = 0; i < value_count; i++) {
						if (strcmp(output, value_list[i]) == 0) {
							current_section
								->current_value_index[current_section->option_count - 1] = i;
							break;
						}
					}
					break;
				}
				case MENU_OPTION_RANGE: {
					int current_value = atoi(output);
					if (current_value >= option->min && current_value <= option->max) {
						current_section->current_value_index[current_section->option_count - 1] =
							current_value;
					}
					break;
				}
				case MENU_OPTION_FLOATRANGE: {
					int current_value = (int)(atof(output) * 10);
					if (current_value >= option->min && current_value <= option->max) {
						current_section->current_value_index[current_section->option_count - 1] =
							current_value;
					}
					break;
				}
				default:
					break;
				}
			}

		}
		// Otherwise it's a submenu
		else if (line[0] == 'S') {
			char option_name[MAX_NAME_LENGTH];
			char option_lable[MAX_LABLE_LENGTH];
			// Split the line into name and value
			sscanf(line, "%[^=]=%[^\n]", option_name, option_lable);

			// Add option to the current section
			MenuOption *option = &current_section->options[current_section->option_count++];
			option->type = MENU_OPTION_SUBMENU;
			strcpy(option->name, option_name);
			strcpy(option->lable, option_lable);
		}
		// parse command only options
		else if (line[0] == 'C') {
			char option_name[MAX_NAME_LENGTH];
			char option_lable[MAX_LABLE_LENGTH];
			char option_command[MAX_LABLE_LENGTH];
			// Split the line into name and value
			sscanf(line, "%[^=]=%[^:]:%[^\n]", option_name, option_lable, option_command);

			// Add option to the current section
			MenuOption *option = &current_section->options[current_section->option_count++];
			option->type = MENU_OPTION_COMMAND;
			strcpy(option->name, option_name);
			strcpy(option->lable, option_lable);
			strcpy(option->read_command,
				option_command); // store command in read command, we onle
								 // need one
			option->command_function = runCustomCommand;
		}
	}

	// Add programmatic commands
	for (int s = 1; s < menu_system->section_count; s++) {
		MenuSection *cs = &menu_system->sections[s];
		add_option(cs, "Save", "SAVE", save_all_changes);
	}
	add_option(&menu_system->sections[0], "Reboot", "REBOOT", doreboot);
	add_option(&menu_system->sections[0], "Exit", "EXIT", exitVTXMenu);

	fclose(file);
	return 0;
}
