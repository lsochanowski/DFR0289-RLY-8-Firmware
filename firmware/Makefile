CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror
BUILD := build

# Toolchain: arm-none-eabi-gcc from PATH, unless a local copy exists under ../work/toolchain
# (override with: make ARM_BIN=/path/to/bin).
LOCAL_ARM_BIN := ../work/toolchain/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi/bin
ARM_BIN ?= $(if $(wildcard $(LOCAL_ARM_BIN)/arm-none-eabi-gcc),$(LOCAL_ARM_BIN),)
ARM_GCC := $(if $(ARM_BIN),$(ARM_BIN)/,)arm-none-eabi-gcc
ARM_SIZE := $(if $(ARM_BIN),$(ARM_BIN)/,)arm-none-eabi-size
ARM_OBJCOPY := $(if $(ARM_BIN),$(ARM_BIN)/,)arm-none-eabi-objcopy
VERSION := $(shell sed -n 's/^.define RLY_FW_VERSION "\(.*\)"/\1/p' core/rly_version.h)

# Flashing over SWD with pyOCD. pyOCD has no built-in STM32F107 target; the F103 flash
# algorithm is identical for this family (256 KB). STLINK_UID selects one of several probes.
PYOCD ?= $(if $(wildcard ../work/toolchain/py/pyocd),PYTHONPATH=../work/toolchain/py python3 -m pyocd,pyocd)
PYOCD_UID := $(if $(STLINK_UID),-u $(STLINK_UID),)
ARM_FLAGS := -mcpu=cortex-m3 -mthumb -Os -std=c11 -Wall -Wextra -Werror -ffreestanding -nostdlib

CORE_SRC := core/rly_frame.c core/rly_json.c core/rly_ha.c core/rly_mqtt.c core/rly_session.c core/rly_mqtt_client.c \
	core/rly_config.c core/rly_http.c core/rly_web.c core/rly_console.c core/rly_mdns.c \
	core/rly_crc.c core/rly_ota.c

TESTS := $(BUILD)/test_frame $(BUILD)/test_json $(BUILD)/test_mqtt $(BUILD)/test_session $(BUILD)/test_relay $(BUILD)/test_mqtt_client \
	$(BUILD)/test_config $(BUILD)/test_panel $(BUILD)/test_ota

.PHONY: test probe probe-gpio port-layer clean flash release boot full

probe: $(BUILD)/probe.elf

probe-gpio: $(BUILD)/gpio_scan.elf

probe-soak: $(BUILD)/mdio_soak.elf

probe-wait: $(BUILD)/mdio_wait.elf

probe-multi: $(BUILD)/relay_multi.elf

# One bank per run, one pin at a time, no click counting:
#   make probe-hold EXTRA="-DPORT_INDEX=0 -DSTART_PIN=0 -DEND_PIN=15"
probe-hold:
	$(ARM_GCC) $(ARM_FLAGS) $(EXTRA) -T port/probe/f107.ld port/probe/relay_hold.c \
		-o $(BUILD)/relay_hold.elf
	$(ARM_SIZE) $(BUILD)/relay_hold.elf

# Whole-bank switch-on: make probe-bank EXTRA="-DBANK=0"
probe-bank:
	$(ARM_GCC) $(ARM_FLAGS) $(EXTRA) -T port/probe/f107.ld port/probe/bank_all.c \
		-o $(BUILD)/bank_all.elf
	$(ARM_SIZE) $(BUILD)/bank_all.elf

# Every pin except the known relays and SWD: make probe-all [EXTRA=-DDRIVE_LOW=1]
probe-all:
	$(ARM_GCC) $(ARM_FLAGS) $(EXTRA) -T port/probe/f107.ld port/probe/all_pins.c \
		-o $(BUILD)/all_pins.elf
	$(ARM_SIZE) $(BUILD)/all_pins.elf

# Same, but holding MDIO/MDC as alternate function and polling the PHY
probe-all-mdio:
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/all_pins_mdio.c \
		-o $(BUILD)/all_pins_mdio.elf
	$(ARM_SIZE) $(BUILD)/all_pins_mdio.elf

$(BUILD)/relay_multi.elf: port/probe/relay_multi.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/relay_multi.c -o $@
	$(ARM_SIZE) $@

$(BUILD)/mdio_wait.elf: port/probe/mdio_wait.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/mdio_wait.c -o $@
	$(ARM_SIZE) $@

$(BUILD)/relay_verify.elf: port/probe/relay_verify.c port/board.h port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/relay_verify.c -o $@
	$(ARM_SIZE) $@

probe-relay: $(BUILD)/relay_sweep.elf

# Board layer objects (no main yet); proves the port code builds for the target.
# Named port-layer, not "port": a target named like the port/ directory would always be
# considered up to date by make.
port-layer:
	$(ARM_GCC) $(ARM_FLAGS) -c port/f107/clock.c -o $(BUILD)/clock.o
	$(ARM_GCC) $(ARM_FLAGS) -c port/f107/relay.c -o $(BUILD)/relay.o
	$(ARM_SIZE) $(BUILD)/clock.o $(BUILD)/relay.o

# Application firmware: clock, relays, ETH MAC/DMA, lwIP, MQTT.
APP_SRC := app/startup.c app/main.c port/f107/clock.c port/f107/eth.c port/f107/relay.c \
	port/f107/eth_dma.c port/f107/ethernetif.c port/f107/systick.c port/f107/lwip/sys_arch.c \
	port/f107/tcp_stream.c port/f107/flash_cfg.c port/f107/ui.c port/f107/httpd.c \
	port/f107/discovery.c port/f107/usb_cdc.c port/f107/flash.c $(CORE_SRC)

LWIP_SRC := third_party/lwip/core/init.c third_party/lwip/core/def.c \
	third_party/lwip/core/inet_chksum.c third_party/lwip/core/ip.c \
	third_party/lwip/core/mem.c third_party/lwip/core/memp.c \
	third_party/lwip/core/netif.c third_party/lwip/core/pbuf.c \
	third_party/lwip/core/raw.c third_party/lwip/core/stats.c \
	third_party/lwip/core/sys.c third_party/lwip/core/timeouts.c \
	third_party/lwip/core/tcp.c third_party/lwip/core/tcp_in.c \
	third_party/lwip/core/tcp_out.c third_party/lwip/core/udp.c \
	third_party/lwip/core/dns.c third_party/lwip/core/ipv4/etharp.c \
	third_party/lwip/core/ipv4/icmp.c third_party/lwip/core/ipv4/ip4.c \
	third_party/lwip/core/ipv4/ip4_addr.c third_party/lwip/core/ipv4/ip4_frag.c \
	third_party/lwip/core/ipv4/dhcp.c third_party/lwip/core/ipv4/autoip.c \
	third_party/lwip/core/ipv4/acd.c third_party/lwip/netif/ethernet.c

LWIP_INC := -Ithird_party/lwip/include -Ithird_party/lwip -Iport/f107/lwip -Iport/f107 -Icore

app: $(BUILD)/app.elf

# Bootloader (first 8 KB): installs images staged by the web panel (core/rly_ota.c).
BOOT_SRC := boot/boot.c port/f107/flash.c core/rly_ota.c core/rly_crc.c
boot: $(BUILD)/boot.elf

$(BUILD)/boot.elf: $(BOOT_SRC) boot/boot.ld core/rly_ota.h port/f107/flash.h | $(BUILD)
	$(ARM_GCC) $(filter-out -nostdlib,$(ARM_FLAGS)) -nostartfiles -specs=nano.specs \
		-specs=nosys.specs -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-T boot/boot.ld $(BOOT_SRC) -o $@
	$(ARM_SIZE) $@

# Full image for SWD flashing: bootloader padded to 8 KB + application.
full: $(BUILD)/full.bin

$(BUILD)/full.bin: $(BUILD)/boot.elf $(BUILD)/app.elf
	$(ARM_OBJCOPY) -O binary --gap-fill 0xFF --pad-to 0x08002000 $(BUILD)/boot.elf $(BUILD)/boot.bin
	$(ARM_OBJCOPY) -O binary $(BUILD)/app.elf $(BUILD)/app.bin
	cat $(BUILD)/boot.bin $(BUILD)/app.bin > $@
	$(ARM_OBJCOPY) -I binary -O ihex --change-addresses 0x08000000 $@ $(BUILD)/full.hex

# Release artefacts in dist/:
#   rly8-fw-vX-full.bin/.hex  first install over SWD (bootloader + app, at 0x08000000)
#   rly8-fw-vX-update.bin     upload in the web panel (application only)
#   rly8-fw-vX.elf            application with symbols
release: test $(BUILD)/full.bin
	mkdir -p dist
	cp $(BUILD)/full.bin dist/rly8-fw-v$(VERSION)-full.bin
	cp $(BUILD)/full.hex dist/rly8-fw-v$(VERSION)-full.hex
	cp $(BUILD)/app.bin dist/rly8-fw-v$(VERSION)-update.bin
	cp $(BUILD)/app.elf dist/rly8-fw-v$(VERSION).elf
	cd dist && shasum -a 256 rly8-fw-v$(VERSION)[-.]* > SHA256SUMS-v$(VERSION).txt

# Writes the firmware and resets the board (a reset is needed even when pyOCD reports the
# image as identical). The two config pages at the end of flash are left untouched.
flash: $(BUILD)/full.bin
	$(PYOCD) flash -t stm32f103rc $(PYOCD_UID) $(BUILD)/full.hex
	$(PYOCD) reset -t stm32f103rc $(PYOCD_UID)

$(BUILD)/app.elf: $(APP_SRC) $(LWIP_SRC) $(wildcard core/*.h port/f107/*.h port/f107/lwip/arch/*.h) \
		third_party/lwip/lwipopts.h port/f107/app.ld | $(BUILD)
	$(ARM_GCC) $(filter-out -nostdlib,$(ARM_FLAGS)) -nostartfiles -specs=nosys.specs $(LWIP_INC) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-Map=$(BUILD)/app.map \
		$(EXTRA_CFLAGS) -T port/f107/app.ld $(APP_SRC) $(LWIP_SRC) -o $@
	$(ARM_SIZE) $@

# One port per run: make probe-count EXTRA=-DPORT_INDEX=0   (0=A, 1=B, 2=C, 3=D, 4=E)
probe-count:
	$(ARM_GCC) $(ARM_FLAGS) $(EXTRA) -T port/probe/f107.ld port/probe/relay_count.c \
		-o $(BUILD)/relay_count.elf
	$(ARM_SIZE) $(BUILD)/relay_count.elf

$(BUILD)/relay_sweep.elf: port/probe/relay_sweep.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/relay_sweep.c -o $@
	$(ARM_SIZE) $@

$(BUILD)/mdio_soak.elf: port/probe/mdio_soak.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/mdio_soak.c -o $@
	$(ARM_SIZE) $@

$(BUILD)/probe.elf: port/probe/mdio_scan.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/mdio_scan.c -o $@
	$(ARM_SIZE) $@

$(BUILD)/gpio_scan.elf: port/probe/gpio_scan.c port/probe/f107.ld | $(BUILD)
	$(ARM_GCC) $(ARM_FLAGS) -T port/probe/f107.ld port/probe/gpio_scan.c -o $@
	$(ARM_SIZE) $@

test: $(TESTS)
	./$(BUILD)/test_frame
	./$(BUILD)/test_json
	./$(BUILD)/test_mqtt
	./$(BUILD)/test_session
	./$(BUILD)/test_relay
	./$(BUILD)/test_mqtt_client
	./$(BUILD)/test_config
	./$(BUILD)/test_panel
	./$(BUILD)/test_ota

$(BUILD)/test_frame: $(CORE_SRC) core/test_frame.c core/rly_frame.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_frame.c -o $@

$(BUILD)/test_json: $(CORE_SRC) core/test_json.c core/rly_json.h core/rly_ha.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_json.c -o $@

$(BUILD)/test_mqtt: $(CORE_SRC) core/test_mqtt.c core/rly_mqtt.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_mqtt.c -o $@

$(BUILD)/test_session: $(CORE_SRC) core/test_session.c core/rly_session.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_session.c -o $@

$(BUILD)/test_board: core/test_board.c port/board.h | $(BUILD)
	$(CC) $(CFLAGS) core/test_board.c -o $@

$(BUILD)/test_relay: core/test_relay.c port/f107/relay.c port/f107/relay.h port/f107/board.h | $(BUILD)
	$(CC) $(CFLAGS) port/f107/relay.c core/test_relay.c -o $@

$(BUILD)/test_mqtt_client: $(CORE_SRC) core/test_mqtt_client.c core/rly_mqtt_client.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_mqtt_client.c -o $@

$(BUILD)/test_config: $(CORE_SRC) core/test_config.c core/rly_config.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_config.c -o $@

$(BUILD)/test_ota: $(CORE_SRC) core/test_ota.c core/rly_ota.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_ota.c -o $@

$(BUILD)/test_panel: $(CORE_SRC) core/test_panel.c core/rly_web.h core/rly_http.h core/rly_console.h core/rly_mdns.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) core/test_panel.c -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) build-arm
