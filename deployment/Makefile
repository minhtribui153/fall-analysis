PROJECT_ROOT ?= $(shell realpath ..)
SHELL := /bin/bash
.ONESHELL:

ACTION    := $(firstword $(MAKECMDGOALS))
SERVICES  := $(wordlist 2, $(words $(MAKECMDGOALS)), $(MAKECMDGOALS))

export COMPOSE_PROJECT_DIRECTORY := $(PROJECT_ROOT)
export COMPOSE_ENV_FILES := $(PROJECT_ROOT)/.env

.PHONY: start stop reset clean

start stop reset clean:
	@set -euo pipefail

	if [ ! -f "$(PROJECT_ROOT)/.env" ]; then
		echo "[error] .env file not found at: $(PROJECT_ROOT)/.env"
		exit 1
	fi

	if [ "$@" != "clean" ] && [ -z "$(SERVICES)" ]; then
		echo "[error] missing service name(s) for action '$@'"
		exit 1
	fi

	COMPOSE_ARGS=()

	for service in $(SERVICES); do
		FILE="$(CURDIR)/compose.$${service}.yaml"

		if [ ! -f "$${FILE}" ]; then
			echo "[error] file not found: $${FILE}"
			exit 1
		fi

		COMPOSE_ARGS+=(-f "$${FILE}")
	done

	case "$@" in
		start)
			echo "[ info] starting services: $(SERVICES)..."
			docker compose "$${COMPOSE_ARGS[@]}" up -d --no-recreate
			;;

		stop)
			echo "[ info] stopping services: $(SERVICES)..."
			docker compose "$${COMPOSE_ARGS[@]}" stop
			;;

		reset)
			echo "[ info] resetting services: $(SERVICES)..."
			docker compose $${COMPOSE_FILES} down --remove-orphans
			docker compose $${COMPOSE_FILES} up -d --force-recreate
			;;

		clean)
			echo "[ info] cleaning orphaned and unused project resources..."

			if [ "$${#COMPOSE_ARGS[@]}" -gt 0 ]; then
				docker compose "$${COMPOSE_ARGS[@]}" down \
					--remove-orphans \
					--volumes
			else
				docker container prune -f \
					--filter "label=com.docker.compose.project.working_dir=$(PROJECT_ROOT)"

				docker network prune -f \
					--filter "label=com.docker.compose.project.working_dir=$(PROJECT_ROOT)"

				docker volume prune -f \
					--filter "label=com.docker.compose.project.working_dir=$(PROJECT_ROOT)"
			fi
			;;
	esac

# Treat additional command-line goals as service names.
%:
	@if [ "$@" = "$(ACTION)" ]; then \
		echo "[error] unknown action '$@' inside deployment module"; \
		exit 1; \
	fi