PROJECT_ROOT ?= $(shell realpath ..)
SHELL := /bin/bash
.ONESHELL:

ACTION    := $(firstword $(MAKECMDGOALS))
SERVICES  := $(wordlist 2, $(words $(MAKECMDGOALS)), $(MAKECMDGOALS))

.PHONY: up down

$(ACTION):
	@if [ ! -f "$(PROJECT_ROOT)/.env" ]; then \
		echo "[error] .env file not found at: $(PROJECT_ROOT)/.env"; \
		exit 1; \
	fi

	if [ -z "$(SERVICES)" ]; then \
		echo "[error] missing service name(s) for action '$(ACTION)'"; \
		exit 1; \
	fi

	for service in $(SERVICES); do \
		FILE="./compose.$${service}.yaml"; \
		if [ ! -f "$${FILE}" ]; then \
			echo "[error] file not found: deployment/compose.$${service}.yaml"; \
			exit 1; \
		fi; \
		if [ "$(ACTION)" = "up" ]; then \
			echo "[ info] deploying $$service via $$FILE..."; \
			docker compose \
				--project-directory "$(PROJECT_ROOT)" \
				--env-file "$(PROJECT_ROOT)/.env" \
				-f $$FILE up -d; \
		elif [ "$(ACTION)" = "down" ]; then \
			echo "[ info] shutting down $$service..."; \
			docker compose \
				--project-directory "$(PROJECT_ROOT)" \
				--env-file "$(PROJECT_ROOT)/.env" \
				-f $$FILE down; \
		else \
			echo "[error] unknown action '$(ACTION)' inside deployment module"; \
			exit 1; \
		fi; \
	done

%:
	@true