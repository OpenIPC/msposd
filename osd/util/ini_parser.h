#ifndef INI_PARSER_H
#define INI_PARSER_H

#define MAX_OPTIONS 50
#define MAX_SECTIONS 20
#define MAX_VALUE_LENGTH 500
#define MAX_NAME_LENGTH 50
#define MAX_LABLE_LENGTH 50
#define MAX_LINE_LENGTH MAX_VALUE_LENGTH * 3 + MAX_NAME_LENGTH + MAX_LABLE_LENGTH
#define MAX_VALUE_LIST_ITEMS 50

typedef enum {
	MENU_OPTION_LIST,		// For list options
	MENU_OPTION_RANGE,		// For range options
	MENU_OPTION_FLOATRANGE, // For range with floating point precision
	MENU_OPTION_SUBMENU,	// For linking to a submenu
	MENU_OPTION_COMMAND		// For executing a command when selected
} MenuOptionType;

typedef struct {
	char name[MAX_NAME_LENGTH];
	char lable[MAX_LABLE_LENGTH];
	char values[MAX_VALUE_LENGTH]; // Possible values for list/range
	MenuOptionType type;		   // Holds the type of option (list, range, submenu, or command)
	int min;					   // Min value for range
	int max;					   // Max value for range
	char read_command[MAX_VALUE_LENGTH]; // Command to read current system value
	char save_command[MAX_VALUE_LENGTH]; // Command to save current system value
	// Only used for MENU_OPTION_COMMAND to execute a function
	void (*command_function)(void *); // Function pointer for command execution
									  // (modified to take void*)
} MenuOption;

typedef struct {
	char name[MAX_NAME_LENGTH];
	char lable[MAX_LABLE_LENGTH];
	int option_count;
	MenuOption options[MAX_OPTIONS];
	int current_value_index[MAX_OPTIONS]; // Add this array to store the current
										  // value index for each option in this
										  // section
} MenuSection;

typedef struct {
	MenuSection sections[MAX_SECTIONS];
	int section_count;
} MenuSystem;

// Function to parse the INI file
void split_values(
	const char *values, char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH], int *value_count);
int parse_ini(const char *filename, MenuSystem *menu_system);
void print_menu_system_state(MenuSystem *menu_system);
void run_command(const char *command, char *output, int output_size);

#endif // INI_PARSER_H
