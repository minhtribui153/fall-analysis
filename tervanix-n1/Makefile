.PHONY: esp32d nesso build clean

esp32d:
	pio run -e esp32d -t compiledb
	@echo "clangd configured for ESP32-D / Xtensa"

nesso:
	pio run -e nesso_n1 -t compiledb
	@echo "clangd configured for Nesso N1 / ESP32-C6 / RISC-V"

build:
	pio run

clean:
	pio run -t clean