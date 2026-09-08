

/*
  Simple INI-like reader/writer for msposd.ini

  Supported:
    [Section]
    key=value
    comments: ; or # at line start
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <stdbool.h>

extern bool verbose;

const char *exe_dir(void) {
    static char dir[512] = "";
    if (!dir[0]) {
        char exe[512] = "";
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) { exe[n] = '\0'; snprintf(dir, sizeof(dir), "%s", dirname(exe)); }
        else snprintf(dir, sizeof(dir), ".");
    }
    return dir;
}

static const char *ini_path(void) {
    static char path[512] = "";
    static bool reported = false;
    if (!path[0])
        snprintf(path, sizeof(path), "%s/msposd.ini", exe_dir());
    if (verbose && !reported) {
        reported = true;
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); printf("[ini] using %s\n", path); }
        else printf("[ini] not found: %s\n", path);
    }
    return path;
}

#define INI_FILENAME ini_path()

/* Absolute path to the ground-station runtime state (home/target), resolved
 * next to the binary so it matches mapserver's APP_DIR/state.ini regardless of
 * the working directory msposd is launched from. */
const char *gs_state_path(void) {
    static char path[512] = "";
    if (!path[0])
        snprintf(path, sizeof(path), "%s/gs/state.ini", exe_dir());
    return path;
}

#ifndef INI_MAX_LINE
#define INI_MAX_LINE 1024
#endif

/* ---------- helpers ---------- */

static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || isspace((unsigned char)s[n-1]))) {
        s[n-1] = '\0';
        n--;
    }
}

static char* lskip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static int is_comment_or_blank(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return (*s == '\0' || *s == ';' || *s == '#');
}

static int parse_section(const char *line, char *out, size_t outsz) {
    // expects something like: [Section]
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '[') return 0;
    p++;
    const char *end = strchr(p, ']');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    while (len > 0 && isspace((unsigned char)p[len-1])) len--;
    while (len > 0 && isspace((unsigned char)*p)) { p++; len--; }

    if (len == 0) return 0;
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int parse_key_value(char *line, char *key, size_t keysz, char *val, size_t valsz) {
    // modifies line in-place
    char *p = lskip(line);
    if (*p == '\0') return 0;
    if (*p == ';' || *p == '#') return 0;

    char *eq = strchr(p, '=');
    if (!eq) return 0;

    *eq = '\0';
    char *k = p;
    char *v = eq + 1;

    k = lskip(k);
    v = lskip(v);

    rtrim(k);
    rtrim(v);

    if (*k == '\0') return 0;

    // copy out
    strncpy(key, k, keysz - 1);
    key[keysz - 1] = '\0';
    strncpy(val, v, valsz - 1);
    val[valsz - 1] = '\0';
    return 1;
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* Reads string value into out buffer. Returns 1 if found, 0 if not found. */
static int build_tmp_path(const char *filename, char *tmp_path, size_t tmp_path_size) {
    int written;

    if (!filename || !tmp_path || tmp_path_size == 0) return 0;
    written = snprintf(tmp_path, tmp_path_size, "%s.tmp", filename);
    return written > 0 && (size_t)written < tmp_path_size;
}

static int IniReadStringPath(const char *filename, const char *section, const char *param,
                             char *out, size_t outsz) {
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;

    char line[INI_MAX_LINE];
    char cur_section[INI_MAX_LINE] = "";
    int in_section = 0;

    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (is_comment_or_blank(line)) continue;

        char sec[INI_MAX_LINE];
        if (parse_section(line, sec, sizeof(sec))) {
            strncpy(cur_section, sec, sizeof(cur_section)-1);
            cur_section[sizeof(cur_section)-1] = '\0';
            in_section = streq(cur_section, section);
            continue;
        }

        if (!in_section) continue;

        char key[INI_MAX_LINE], val[INI_MAX_LINE];
        char tmp[INI_MAX_LINE];
        strncpy(tmp, line, sizeof(tmp)-1);
        tmp[sizeof(tmp)-1] = '\0';

        if (parse_key_value(tmp, key, sizeof(key), val, sizeof(val))) {
            if (streq(key, param)) {
                strncpy(out, val, outsz - 1);
                out[outsz - 1] = '\0';
                fclose(f);
                return 1;
            }
        }
    }

    fclose(f);
    return 0;
}

/*
  Writes/updates a key=value within [section].
  Returns 1 on success, 0 on failure.
*/
static int IniWriteStringPath(const char *filename, const char *section, const char *param,
                              const char *value) {
    char tmp_path[INI_MAX_LINE];
    FILE *in;
    FILE *out;

    if (!build_tmp_path(filename, tmp_path, sizeof(tmp_path))) return 0;

    in = fopen(filename, "rb");
    out = fopen(tmp_path, "wb");
    if (!out) {
        if (in) fclose(in);
        return 0;
    }

    int wrote = 0;
    int section_found = 0;
    int in_target_section = 0;

    char line[INI_MAX_LINE];
    char cur_section[INI_MAX_LINE] = "";

    if (!in) {
        // No existing file: just create it
        fprintf(out, "[%s]\n%s=%s\n", section, param, value);
        fclose(out);
        remove(filename);
        rename(tmp_path, filename);
        return 1;
    }

    while (fgets(line, sizeof(line), in)) {
        // Keep original line as-is for writing unless we need to change it
        char original[INI_MAX_LINE];
        strncpy(original, line, sizeof(original)-1);
        original[sizeof(original)-1] = '\0';

        char trimmed[INI_MAX_LINE];
        strncpy(trimmed, line, sizeof(trimmed)-1);
        trimmed[sizeof(trimmed)-1] = '\0';
        rtrim(trimmed);

        char sec[INI_MAX_LINE];
        if (parse_section(trimmed, sec, sizeof(sec))) {
            // We are about to leave target section; if we never wrote key, append it before new section
            if (in_target_section && !wrote) {
                fprintf(out, "%s=%s\n", param, value);
                wrote = 1;
            }

            strncpy(cur_section, sec, sizeof(cur_section)-1);
            cur_section[sizeof(cur_section)-1] = '\0';

            in_target_section = streq(cur_section, section);
            if (in_target_section) section_found = 1;

            fputs(original, out);
            continue;
        }

        if (in_target_section && !is_comment_or_blank(trimmed)) {
            // Try parse key=value
            char tmp[INI_MAX_LINE];
            strncpy(tmp, trimmed, sizeof(tmp)-1);
            tmp[sizeof(tmp)-1] = '\0';

            char key[INI_MAX_LINE], val[INI_MAX_LINE];
            if (parse_key_value(tmp, key, sizeof(key), val, sizeof(val))) {
                if (streq(key, param)) {
                    // Replace this line
                    fprintf(out, "%s=%s\n", param, value);
                    wrote = 1;
                    continue; // skip writing original
                }
            }
        }

        // Default: write original line unchanged
        fputs(original, out);
    }

    // If file ended while we were in target section and we still didn't write the key
    if (in_target_section && !wrote) {
        fprintf(out, "%s=%s\n", param, value);
        wrote = 1;
    }

    // If section never existed, append new section+key at end
    if (!section_found) {
        // Ensure there's a newline before appending if file doesn't end with one
        // (Best-effort: add a newline separator)
        fprintf(out, "\n[%s]\n%s=%s\n", section, param, value);
        wrote = 1;
    }

    fclose(in);
    fclose(out);

    // Replace original
    remove(filename);
    if (rename(tmp_path, filename) != 0) {
        // cleanup temp if rename fails
        remove(tmp_path);
        return 0;
    }

    return wrote ? 1 : 0;
}




/* ---------- Public API (C-friendly) ---------- */

int WriteIniString(const char *SectionName, const char *ParamName, const char *ParamValue) {
    if (!SectionName || !ParamName || !ParamValue) return 0;
    return IniWriteStringPath(INI_FILENAME, SectionName, ParamName, ParamValue);
}

int WriteIniInt(const char *SectionName, const char *ParamName, int ParamValue) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", ParamValue);
    return WriteIniString(SectionName, ParamName, buf);
}

int WriteIniStringPath(const char *IniPath, const char *SectionName, const char *ParamName,
                       const char *ParamValue) {
    if (!IniPath || !SectionName || !ParamName || !ParamValue) return 0;
    return IniWriteStringPath(IniPath, SectionName, ParamName, ParamValue);
}

int WriteIniIntPath(const char *IniPath, const char *SectionName, const char *ParamName,
                    int ParamValue) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", ParamValue);
    return WriteIniStringPath(IniPath, SectionName, ParamName, buf);
}

/* C version: pass pointer */
int ReadIniInt(const char *SectionName, const char *ParamName, int *ParamValue) {
    if (!SectionName || !ParamName || !ParamValue) return 0;
    char buf[INI_MAX_LINE];
    if (!IniReadStringPath(INI_FILENAME, SectionName, ParamName, buf, sizeof(buf))) return 0;
    *ParamValue = atoi(buf);
    return 1;
}

/* C version: provide output buffer and its size */
int ReadIniString(const char *SectionName, const char *ParamName, char *ParamValue, size_t ParamValueSize) {
    if (!SectionName || !ParamName || !ParamValue || ParamValueSize == 0) return 0;
    return IniReadStringPath(INI_FILENAME, SectionName, ParamName, ParamValue, ParamValueSize);
}

/* Path variants: read from an arbitrary ini file instead of INI_FILENAME. */
int ReadIniStringPath(const char *IniPath, const char *SectionName, const char *ParamName,
                      char *ParamValue, size_t ParamValueSize) {
    if (!IniPath || !SectionName || !ParamName || !ParamValue || ParamValueSize == 0) return 0;
    return IniReadStringPath(IniPath, SectionName, ParamName, ParamValue, ParamValueSize);
}

int ReadIniIntPath(const char *IniPath, const char *SectionName, const char *ParamName, int *ParamValue) {
    if (!IniPath || !SectionName || !ParamName || !ParamValue) return 0;
    char buf[INI_MAX_LINE];
    if (!IniReadStringPath(IniPath, SectionName, ParamName, buf, sizeof(buf))) return 0;
    *ParamValue = atoi(buf);
    return 1;
}
