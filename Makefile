export PROJECT_ROOT := $(shell pwd)
SHELL := /bin/bash
.ONESHELL:

CMD  := $(firstword $(MAKECMDGOALS))
ARGS := $(wordlist 2, $(words $(MAKECMDGOALS)), $(MAKECMDGOALS))

SPECIAL_TARGETS := init

# Handle directory routing aliases
ifeq ($(CMD),deploy)
    TARGET_DIR := deployment
else
    TARGET_DIR := $(CMD)
endif

.PHONY: init $(CMD)

init:
	@if [ ! -f "$(PROJECT_ROOT)/.env" ]; then \
		echo "[error] .env file not found"; \
		exit 1; \
	fi
	set -a
	source "$(PROJECT_ROOT)/.env"
	set +a
	mkdir -p "$${DIR_DATA}/instances/pgadmin"
	if [ "$$(stat -c '%u:%g' "$${DIR_DATA}/instances/pgadmin")" != "5050:5050" ]; then \
		echo "[ info] fixing pgadmin permissions..."; \
		sudo chown -R 5050:5050 "$${DIR_DATA}/instances/pgadmin"; \
	fi


ifneq ($(filter $(CMD),$(SPECIAL_TARGETS)),)
else
$(CMD):
	@if [ -z "$(ARGS)" ]; then \
		echo "[error] no arguments provided for '$(CMD)'"; \
		exit 1; \
	fi
	if [ ! -f "$(TARGET_DIR)/Makefile" ]; then \
		echo "[error] no Makefile found in directory '$(TARGET_DIR)/'"; \
		exit 1; \
	fi
	$(MAKE) -s -C $(TARGET_DIR) $(ARGS)
endif

# Catch-all rule to swallow arguments so Make doesn't try to execute them as targets
%:
	@true