TARGET	= vdb_shell
PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
CFLAGS	= -Wl,-q -Wall -O2 -nostdlib -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -std=gnu11
LIBS	= -L../user/stubs -lVDB_365_stub -lSceAppMgr_stub -lSceLibKernel_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub

all: $(TARGET).suprx

$(TARGET).suprx: $(TARGET).velf
	vita-make-fself -c $< $@

$(TARGET).velf: $(TARGET).elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(TARGET).c ../user/vdb_user.h
	$(CC) $(CFLAGS) $(TARGET).c $(LIBS) -o $@

clean:
	rm -f $(TARGET).suprx $(TARGET).velf $(TARGET).elf
