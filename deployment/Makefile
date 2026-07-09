.PHONY: %

%:
	@if [ -f "./deploy-$@.sh" ]; then \
		echo "Executing deployment/deploy-$@.sh..."; \
		bash ./deploy-$@.sh; \
	else \
		echo "[Error] No script found for target '$@'"; \
		exit 1; \
	fi
