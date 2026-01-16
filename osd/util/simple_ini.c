

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

#define INI_FILENAME "msposd.ini"
#define INI_TMPFILE  "msposd.ini.tmp"

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
static int IniReadString(const char *section, const char *param, char *out, size_t outsz) {
    FILE *f = fopen(INI_FILENAME, "rb");
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
static int IniWriteString(const char *section, const char *param, const char *value) {
    FILE *in = fopen(INI_FILENAME, "rb");
    FILE *out = fopen(INI_TMPFILE, "wb");
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
        remove(INI_FILENAME);
        rename(INI_TMPFILE, INI_FILENAME);
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
    remove(INI_FILENAME);
    if (rename(INI_TMPFILE, INI_FILENAME) != 0) {
        // cleanup temp if rename fails
        remove(INI_TMPFILE);
        return 0;
    }

    return wrote ? 1 : 0;
}




/* ---------- Public API (C-friendly) ---------- */

int WriteIniString(const char *SectionName, const char *ParamName, const char *ParamValue) {
    if (!SectionName || !ParamName || !ParamValue) return 0;
    return IniWriteString(SectionName, ParamName, ParamValue);
}

int WriteIniInt(const char *SectionName, const char *ParamName, int ParamValue) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", ParamValue);
    return WriteIniString(SectionName, ParamName, buf);
}

/* C version: pass pointer */
int ReadIniInt(const char *SectionName, const char *ParamName, int *ParamValue) {
    if (!SectionName || !ParamName || !ParamValue) return 0;
    char buf[INI_MAX_LINE];
    if (!IniReadString(SectionName, ParamName, buf, sizeof(buf))) return 0;
    *ParamValue = atoi(buf);
    return 1;
}

/* C version: provide output buffer and its size */
int ReadIniString(const char *SectionName, const char *ParamName, char *ParamValue, size_t ParamValueSize) {
    if (!SectionName || !ParamName || !ParamValue || ParamValueSize == 0) return 0;
    return IniReadString(SectionName, ParamName, ParamValue, ParamValueSize);
}
