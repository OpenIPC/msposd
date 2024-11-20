# Get the current date and time in the format YYYYMMDD_HHMMSS
VERSION_STRING := $(shell date +"%Y%m%d_%H%M%S")
CFLAGS ?=
CFLAGS += -Wno-address-of-packed-member -DVERSION_STRING="\"$(VERSION_STRING)\""

SRCS := compat.c msposd.c bmp/bitmap.c bmp/region.c bmp/lib/schrift.c bmp/text.c osd/net/network.c osd/msp/msp.c osd/msp/msp_displayport.c libpng/lodepng.c osd/util/interface.c osd/util/settings.c osd/util/ini_parser.c osd/msp/vtxmenu.c
OUTPUT ?= $(PWD)
BUILD = $(CC) $(SRCS) -I $(SDK)/include -I$(TOOLCHAIN)/usr/include -L$(DRV) $(CFLAGS) $(LIB) -levent_core -Os -s $(CFLAGS) -o $(OUTPUT)

clean:
	rm -f *.o msposd_x86 msposd_goke msposd_hisi msposd_star6b0 msposd_star6e

goke:
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D__GOKE__)
	$(eval LIB = -shared -ldl -ldnvqe -lgk_api -lhi_mpi -lsecurec -lupvqe -lvoice_engine -ldnvqe)
	$(BUILD)

hisi:
	$(eval SDK = ./sdk/hi3516ev300)
	$(eval CFLAGS += -D__GOKE__)
	$(eval LIB = -shared -ldnvqe -lmpi -lsecurec -lupvqe -lVoiceEngine)
	$(BUILD)

star6b0:
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6B0__)
	$(eval LIB = -lcam_os_wrapper -lm -lmi_rgn -lmi_sys)
	$(BUILD)

star6e:
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6E__)
	$(eval LIB = -lcam_os_wrapper -lm -lmi_rgn -lmi_sys -lmi_venc)
	$(BUILD)

jetson:
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D_x86 -D_jetson)
	$(eval LIB = -lcsfml-graphics -lcsfml-window -lcsfml-system `pkg-config --libs cairo x11` -lm)
	$(eval BUILD = $(CC) $(SRCS) -I $(SDK)/include -L $(DRV) $(CFLAGS) $(LIB) -levent_core -O0 -g -o $(OUTPUT))
	$(BUILD)

x86:
	$(eval SDK = ./sdk/gk7205v300)
	$(eval CFLAGS += -D_x86)
	$(eval LIB = -lcsfml-graphics -lcsfml-window -lcsfml-system `pkg-config --libs cairo x11` -lm)
	 
	$(eval BUILD = $(CC) $(SRCS) -I $(SDK)/include -L $(DRV) $(CFLAGS) $(LIB) -levent_core -O0 -g -o $(OUTPUT))
	$(BUILD)
