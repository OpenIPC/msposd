#ifndef INI_H
#define INI_H

#include <stddef.h>  // size_t

#ifdef __cplusplus
extern "C" {
#endif

/* Write returns 1 on success, 0 on failure */
int WriteIniString(const char *SectionName, const char *ParamName, const char *ParamValue);
int WriteIniInt(const char *SectionName, const char *ParamName, int ParamValue);
int WriteIniStringPath(const char *IniPath, const char *SectionName, const char *ParamName,
	const char *ParamValue);
int WriteIniIntPath(const char *IniPath, const char *SectionName, const char *ParamName,
	int ParamValue);

/* Returns the directory containing the running binary (no trailing slash). */
const char *exe_dir(void);

/* Absolute path to the GS runtime state file (home/target), next to the binary. */
const char *gs_state_path(void);

/* Read returns 1 if found (and output set), 0 if not found / error */
int ReadIniInt(const char *SectionName, const char *ParamName, int *ParamValue);
int ReadIniString(const char *SectionName, const char *ParamName, char *ParamValue, size_t ParamValueSize);

/* Path variants: read from an arbitrary ini file instead of the default config. */
int ReadIniIntPath(const char *IniPath, const char *SectionName, const char *ParamName, int *ParamValue);
int ReadIniStringPath(const char *IniPath, const char *SectionName, const char *ParamName,
	char *ParamValue, size_t ParamValueSize);

#ifdef __cplusplus
}
#endif

#endif /* INI_H */
