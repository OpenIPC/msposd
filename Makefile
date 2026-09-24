# Get the current date and time in the format YYYYMMDD_HHMMSS
VERSION_STRING := $(shell date +"%Y%m%d_%H%M%S")
CFLAGS ?=
CFLAGS += -Wno-address-of-packed-member -DVERSION_STRING="\"$(VERSION_STRING)\""

SRCS := compat.c msposd.c bmp/bitmap.c bmp/region.c bmp/lib/schrift.c bmp/text.c osd/net/network.c osd/msp/msp.c osd/msp/msp_displayport.c libpng/lodepng.c osd/util/interface.c osd/util/settings.c osd/util/ini_parser.c osd/msp/vtxmenu.c osd/util/subtitle.c osd/util/simple_ini.c
GS_SRCS := osd/util/terrain_elevation.c osd/util/terrain_agl.c
OUTPUT ?= $(PWD)
BUILD = $(CC) $(SRCS) -I $(SDK)/include -I$(TOOLCHAIN)/usr/include -I$(PWD) -L$(DRV) $(CFLAGS) $(LIB) -levent_core -Os -s $(CFLAGS) -o $(OUTPUT)

VERSION := $(shell git describe --always --dirty)

version.h:
	echo "Git version: $(VERSION)"
	echo "#ifndef VERSION_H" > version.h
	echo "#define VERSION_H" >> version.h
	echo "#define GIT_VERSION \"$(VERSION)\"" >> version.h
	echo "#endif // VERSION_H" >> version.h

all: version.h

clean:
	rm -f *.o msposd msposd_goke msposd_hisi msposd_star6b0 msposd_star6e msposd_rockchip

goke: version.h
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D__GOKE__)
	$(eval LIB = -ldl -ldnvqe -lgk_api -lhi_mpi -lsecurec -lupvqe -lvoice_engine -ldnvqe)
	$(BUILD)

hisi: version.h
	$(eval SDK = ./sdk/hi3516ev300)
	$(eval CFLAGS += -D__GOKE__)
	$(eval LIB = -ldnvqe -lmpi -lsecurec -lupvqe -lVoiceEngine)
	$(BUILD)

hi3536: version.h
	$(eval SDK = ./sdk/hi3536dv100)
	$(eval CFLAGS += -D__GOKE__ -D__HI3536__)
	$(eval LIB = -lm -ldnvqe -lmpi -ljpeg -lupvqe -lVoiceEngine)
	$(BUILD)

star6b0: version.h
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6B0__)
	$(eval LIB = -lcam_os_wrapper -lm -lmi_rgn -lmi_sys)
	$(BUILD)

star6c: version.h
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6C__)
	$(eval LIB = -lcam_os_wrapper -lmi_rgn -lmi_sys)
	$(BUILD)

star6e: version.h
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6E__)
	$(eval LIB = -lcam_os_wrapper -lm -lmi_rgn -lmi_sys)
	$(BUILD)

native: version.h
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D_x86)
	$(eval LIB = -lcsfml-graphics -lcsfml-window -lcsfml-system `pkg-config --libs cairo x11 xext` -lXext -lm -lsqlite3)
	$(eval BUILD = $(CC) $(SRCS) $(GS_SRCS) -I $(SDK)/include -L $(DRV) $(CFLAGS) $(LIB) -levent_core -O0 -g -o $(OUTPUT))
	$(BUILD)

rockchip: version.h
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D__ROCKCHIP__)
	$(eval LIB = `pkg-config --libs cairo x11 xext` -lXext -lm -lrt -lsqlite3)
	$(eval BUILD = $(CC) $(SRCS) $(GS_SRCS) -I $(SDK)/include -L $(DRV) $(CFLAGS) $(LIB) -levent_core -O0 -g -o $(OUTPUT))
	$(BUILD)

test-terrain-elevation:
	$(CC) tests/terrain_elevation_test.c osd/util/terrain_elevation.c -I$(PWD) \
		-DTERRAIN_ELEVATION_DB_PATH='"/tmp/msposd-terrain-elevation-test.db"' \
		-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
		-o /tmp/msposd-terrain-elevation-test -lsqlite3 -lm
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:handle_segv=0 \
		/tmp/msposd-terrain-elevation-test

test-terrain-agl:
	$(CC) tests/terrain_agl_test.c osd/util/terrain_agl.c osd/util/terrain_elevation.c \
		-I$(PWD) -DTERRAIN_ELEVATION_DB_PATH='"/tmp/msposd-terrain-agl-test.db"' \
		-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
		-o /tmp/msposd-terrain-agl-test -lsqlite3 -lm
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:handle_segv=0 \
		/tmp/msposd-terrain-agl-test

test-terrain: test-terrain-elevation test-terrain-agl
