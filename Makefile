.PHONY: deploy

ifeq ($(firstword $(MAKECMDGOALS)),deploy)
    SUB_CMD := $(word 2,$(MAKECMDGOALS))
    $(eval $(SUB_CMD):;@:)
endif

deploy:
	@if [ -z "$(SUB_CMD)" ]; then \
		echo "[Error] Missing sub-command. Usage: make deploy <component>"; \
		exit 1; \
	fi || exit 0
	@$(MAKE) -s -C deployment $(SUB_CMD)
