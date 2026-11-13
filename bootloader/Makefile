# PureThermal USB DFU bootloader (32 KB at 0x08000000). Built from the parent tree's
# HAL / CMSIS / USB core sources with section garbage collection to stay small.
TOP     := ..
SYSTEM  ?= arm-none-eabi-
CC      := $(SYSTEM)gcc
OBJCOPY := $(SYSTEM)objcopy
SIZE    := $(SYSTEM)size

CPU   := -mthumb -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
DEFS  := -DSTM32F411xE -DUSE_HAL_DRIVER "-D__weak=__attribute__((weak))" "-D__packed=__attribute__((__packed__))" -DPT_BOOTLOADER
INCS  := -I. -I$(TOP)/Inc -I$(TOP)/Drivers/STM32F4xx_HAL_Driver/Inc \
         -I$(TOP)/Drivers/CMSIS/Include -I$(TOP)/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
         -I$(TOP)/Middlewares/ST/STM32_USB_Device_Library/Core/Inc
BCFLAGS := $(CPU) $(DEFS) $(INCS) -Os -std=c99 -Wall -ffunction-sections -fdata-sections -fno-common -fno-short-enums
BLDFLAGS := $(CPU) -T bootloader.ld -Wl,--gc-sections,-Map=bootloader.map -specs=nano.specs -specs=nosys.specs -Wl,--no-enum-size-warning

HAL := $(TOP)/Drivers/STM32F4xx_HAL_Driver/Src
USBC := $(TOP)/Middlewares/ST/STM32_USB_Device_Library/Core/Src
SRCS := main.c it.c usbd_dfu.c usbd_desc_boot.c \
        $(TOP)/Src/usbd_conf.c \
        $(TOP)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c \
        $(HAL)/stm32f4xx_hal.c $(HAL)/stm32f4xx_hal_cortex.c $(HAL)/stm32f4xx_hal_rcc.c \
        $(HAL)/stm32f4xx_hal_rcc_ex.c $(HAL)/stm32f4xx_hal_gpio.c $(HAL)/stm32f4xx_hal_pcd.c \
        $(HAL)/stm32f4xx_hal_pcd_ex.c $(HAL)/stm32f4xx_ll_usb.c $(HAL)/stm32f4xx_hal_flash.c \
        $(HAL)/stm32f4xx_hal_flash_ex.c \
        $(USBC)/usbd_core.c $(USBC)/usbd_ctlreq.c $(USBC)/usbd_ioreq.c
ASRC := $(TOP)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f411xe.s

OBJDIR := build
OBJS := $(addprefix $(OBJDIR)/,$(notdir $(SRCS:.c=.o)) $(notdir $(ASRC:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(dir $(ASRC))

all: bootloader.bin

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@echo cc $<
	@$(CC) $(BCFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.s | $(OBJDIR)
	@$(CC) $(BCFLAGS) -x assembler-with-cpp -c -o $@ $<

bootloader.elf: $(OBJS) bootloader.ld
	$(CC) $(BLDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@

bootloader.bin: bootloader.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(stat -c %s $@) -le 32768 || (echo "bootloader exceeds 32 KB"; rm -f $@; exit 1)

clean:
	rm -rf $(OBJDIR) bootloader.elf bootloader.bin bootloader.map

.PHONY: all clean
