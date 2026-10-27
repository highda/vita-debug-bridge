TARGET	= vdb
OBJS	= main.o
LIBS	= -lSceSysmemForDriver_stub -lSceThreadmgrForDriver_stub \
	-lSceCpuForDriver_stub -lSceUdcdForDriver_stub \
	-lSceIofilemgrForDriver_stub -lSceSysclibForDriver_stub -lSceCtrlForDriver_stub -lScePowerForDriver_stub -lSceKernelSuspendForDriver_stub -lSceAppMgrForDriver_stub -lSceDisplayForDriver_stub -lSceSysrootForDriver_stub -ltaihenForKernel_stub

PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
CFLAGS	= -Wl,-q -Wall -O2 -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -std=gnu11

all: $(TARGET).skprx

$(TARGET).skprx: $(TARGET).velf
	vita-make-fself -c $< $@

$(TARGET).velf: $(TARGET).elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET).skprx $(TARGET).velf $(TARGET).elf $(OBJS)
