SHELL := /bin/bash
.ONESHELL:

%:
	@:

CMD  := $(firstword $(MAKECMDGOALS))
ARGS := $(wordlist 2, $(words $(MAKECMDGOALS)), $(MAKECMDGOALS))

# Handle commands
# If need to create a command alias then add here.
ifeq ($(CMD),deploy)
    TARGET_DIR := deployment
# else ifeq ($(CMD),test)
#     TARGET_DIR := testing
else
    TARGET_DIR := $(CMD)
endif

.PHONY: $(CMD)
$(CMD):
	@if [ -z "$(ARGS)" ]; then
		echo "[Error] No arguments provided for '$(CMD)'"
		exit 1
	fi
	if [ ! -f "$(TARGET_DIR)/Makefile" ]; then
		echo "[Error] No Makefile found in directory '$(TARGET_DIR)/'"
		exit 1
	fi
	$(MAKE) -s -C $(TARGET_DIR) $(ARGS)
