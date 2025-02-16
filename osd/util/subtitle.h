#pragma once

#include "../../osd.h"

#define HEADER_BYTES 40
#define FC_TYPE_BYTES 4
#define MAX_OSD_WIDTH 54
#define MAX_OSD_HEIGHT 20

void write_srt_file();
void handle_osd_out();
void inotify_callback(evutil_socket_t fd, short events, void* arg);
void check_recoding_file();
