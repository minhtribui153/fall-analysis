.PHONY: deploy

# 1. Intercept 'make deploy <sub-command>' syntax
ifeq ($(firstword $(MAKECMDGOALS)),deploy)
  # Extract the second word (the sub-command)
  SUB_CMD := $(word 2,$(MAKECMDGOALS))
  # Turn the sub-command into a do-nothing target
  $(eval $(SUB_CMD):;@:)
endif

# 2. Forward the call to the deployment folder safely
deploy:
	@if [ -z "$(SUB_CMD)" ]; then \
		echo "[Error] Missing sub-command. Usage: make deploy <component>"; \
		exit 1; \
	fi || exit 0
	@$(MAKE) -s -C deployment $(SUB_CMD)
