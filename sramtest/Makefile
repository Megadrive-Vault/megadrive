CROSS = m68k-elf-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

ASFLAGS += -m68000 --register-prefix-optional --bitwise-or -pic
ASFLAGS_CC += -Wa,-m68000 -Wa,--register-prefix-optional -Wa,--bitwise-or -Wa,-pic
CFLAGS += -Wall -g -O2 -m68000 -fomit-frame-pointer
LDLIBS += $(shell $(CC) -print-file-name=libgcc.a)

TARGET = sramtest
OBJS = sega_gcc.o main.o asmtools.o data.o

all: $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(LD) -o $@ -Tsega.ld -Map $(TARGET).map $^ $(LDLIBS)

$(TARGET)_e.bin: $(TARGET).bin
	dd if=/dev/zero of=$@ bs=1M count=1
	dd if=$^ of=$@ bs=1M seek=1
	dd if=$^ of=$@ conv=notrunc

clean:
	$(RM) $(TARGET).bin $(OBJS) $(TARGET).elf $(TARGET).map


%.bin: %.elf
	$(OBJCOPY) -I elf32-m68k -O binary $^ $@

%.o: %.S
	$(CC) -c -o $@ $^ $(ASFLAGS_CC)
