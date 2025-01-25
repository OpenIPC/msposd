#include <stdio.h>
#include <string.h>

#define MAX_LINE_LENGTH 256

char *read_setting(const char *filename, char *key);
int write_setting(const char *filename, const char *key, const char *new_value);
