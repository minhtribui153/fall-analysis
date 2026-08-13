#include "mqtt_publisher.hpp"

#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mqtt_client.h"

namespace {
	constexpr char TAG[] = "mqtt";
	constexpr std::size_t BROKER_URI_CAPACITY = 64;
	constexpr std::size_t TOPIC_CAPACITY = 128;
	constexpr std::size_t DEVICE_ID_CAPACITY = 18;
	constexpr std::size_t WORKER_ID_CAPACITY = 13;
	constexpr std::size_t TOKEN_CAPACITY = 64;
	constexpr std::size_t SESSION_TEXT_CAPACITY = 37;

	esp_mqtt_client_handle_t client = nullptr;
	std::atomic_bool connected{false};
	std::atomic_bool rejected{false};
	std::atomic_bool accepted{false};
	std::atomic<std::uint64_t> published_batches{0};
	std::atomic<std::uint64_t> published_samples{0};
	std::atomic<std::uint64_t> publish_failures{0};
	std::atomic<std::int64_t> last_publish_ms{0};
	char broker_uri[BROKER_URI_CAPACITY]{};
	char mqtt_device_id[DEVICE_ID_CAPACITY]{};
	char mqtt_worker_id[WORKER_ID_CAPACITY]{};
	char mqtt_session_id[SESSION_TEXT_CAPACITY]{};
	char mqtt_bearer_token[TOKEN_CAPACITY]{};
	char telemetry_topic[TOPIC_CAPACITY]{};
	char auth_status_topic[TOPIC_CAPACITY]{};

	void uuid_to_text(const tervanix::identity::SessionUuid& value, char* out, std::size_t out_size)
	{
		std::snprintf(
			out, out_size,
			"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
			value[8], value[9], value[10], value[11], value[12], value[13], value[14], value[15]
		);
	}

	bool build_session_topic(const char* suffix, char* out, std::size_t out_size)
	{
		char compact[33]{};
		std::size_t n = 0;
		for (const char* p = mqtt_session_id; *p; ++p) {
			if (*p != '-') compact[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
		}
		compact[n] = '\0';
		const int written = std::snprintf(out, out_size, "tervanix/sessions/%s/%s", compact, suffix);
		return written > 0 && static_cast<std::size_t>(written) < out_size;
	}

	bool base64_encode(const std::uint8_t* input, std::size_t input_len, char*& output)
	{
		std::size_t required = 0;
		mbedtls_base64_encode(nullptr, 0, &required, input, input_len);
		output = static_cast<char*>(std::malloc(required + 1));
		if (!output) return false;
		std::size_t written = 0;
		if (mbedtls_base64_encode(
				reinterpret_cast<unsigned char*>(output), required + 1, &written, input, input_len) != 0) {
			std::free(output); output = nullptr; return false;
		}
		output[written] = '\0';
		return true;
	}

	void derive_aes_key(std::uint8_t key[32])
	{
		mbedtls_sha256_context ctx;
		mbedtls_sha256_init(&ctx);
		mbedtls_sha256_starts(&ctx, 0);
		mbedtls_sha256_update(
			&ctx,
			reinterpret_cast<const unsigned char*>(mqtt_bearer_token),
			std::strlen(mqtt_bearer_token)
		);
		mbedtls_sha256_finish(&ctx, key);
		mbedtls_sha256_free(&ctx);
	}

	void handle_auth_status(const esp_mqtt_event_handle_t event)
	{
		if (!event || !event->topic || !event->data || event->topic_len <= 0 || event->data_len <= 0) return;
		const std::size_t expected_len = std::strlen(auth_status_topic);
		if (static_cast<std::size_t>(event->topic_len) != expected_len ||
			std::memcmp(event->topic, auth_status_topic, expected_len) != 0) return;

		cJSON* root = cJSON_ParseWithLength(event->data, event->data_len);
		if (!root) return;
		const cJSON* status = cJSON_GetObjectItemCaseSensitive(root, "status");
		const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
		if (cJSON_IsString(status) && status->valuestring) {
			if (std::strcmp(status->valuestring, "rejected") == 0) {
				rejected.store(true); accepted.store(false);
				ESP_LOGE(TAG, "Encrypted telemetry rejected%s%s",
						cJSON_IsString(reason) && reason->valuestring ? ": " : "",
						cJSON_IsString(reason) && reason->valuestring ? reason->valuestring : "");
			} else if (std::strcmp(status->valuestring, "accepted") == 0) {
				accepted.store(true); rejected.store(false);
				ESP_LOGI(TAG, "Encrypted telemetry session accepted");
			}
		}
		cJSON_Delete(root);
	}

	void on_mqtt_event(void*, esp_event_base_t, std::int32_t event_id, void* event_data)
	{
		auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
		switch (event_id) {
			case MQTT_EVENT_CONNECTED: {
				connected.store(true);
				ESP_LOGI(TAG, "Connected to %s session=%s worker=%s", broker_uri, mqtt_session_id, mqtt_worker_id);
				const int mid = esp_mqtt_client_subscribe(client, auth_status_topic, 1);
				if (mid >= 0) ESP_LOGI(TAG, "Subscribed to %s", auth_status_topic);
				break;
			}
			case MQTT_EVENT_DISCONNECTED:
				connected.store(false);
				ESP_LOGW(TAG, "Disconnected; MQTT client will reconnect");
				break;
			case MQTT_EVENT_DATA:
				handle_auth_status(event);
				break;
			case MQTT_EVENT_ERROR:
				connected.store(false);
				ESP_LOGE(TAG, "MQTT error event");
				break;
			default: break;
		}
	}
}  // namespace

namespace tervanix::mqtt {

	esp_err_t connect(
		const char* broker_ip,
		const char* device_id,
		const char* worker_id,
		const identity::SessionUuid& session_id,
		const char* bearer_token)
	{
		if (!broker_ip || !*broker_ip || !device_id || !*device_id || !worker_id || !*worker_id || !bearer_token || !*bearer_token)
			return ESP_ERR_INVALID_ARG;
		if (client) return ESP_OK;
		if (std::strlen(device_id) >= sizeof(mqtt_device_id) ||
			std::strlen(worker_id) >= sizeof(mqtt_worker_id) ||
			std::strlen(bearer_token) >= sizeof(mqtt_bearer_token)) return ESP_ERR_INVALID_SIZE;

		std::strcpy(mqtt_device_id, device_id);
		std::strcpy(mqtt_worker_id, worker_id);
		std::strcpy(mqtt_bearer_token, bearer_token);
		uuid_to_text(session_id, mqtt_session_id, sizeof(mqtt_session_id));
		if (!build_session_topic("telemetry", telemetry_topic, sizeof(telemetry_topic)) ||
			!build_session_topic("auth/status", auth_status_topic, sizeof(auth_status_topic))) return ESP_ERR_INVALID_ARG;

		if (std::snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s", broker_ip) <= 0) return ESP_ERR_INVALID_ARG;

		esp_mqtt_client_config_t config{};
		config.broker.address.uri = broker_uri;
		
		// Do not transmit the bearer token as an MQTT password over plaintext TCP.
		// The session UUID is non-secret routing metadata; confidentiality/integrity
		// comes from AES-256-GCM on each telemetry envelope.
		config.credentials.username = mqtt_session_id;

		client = esp_mqtt_client_init(&config);
		if (!client) return ESP_FAIL;
		esp_err_t rc = esp_mqtt_client_register_event(
			client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), on_mqtt_event, nullptr);
		if (rc != ESP_OK) { esp_mqtt_client_destroy(client); client = nullptr; return rc; }
		rc = esp_mqtt_client_start(client);
		if (rc != ESP_OK) { esp_mqtt_client_destroy(client); client = nullptr; return rc; }
		ESP_LOGI(TAG, "MQTT started; broker=%s; AES-256-GCM payload encryption enabled", broker_uri);
		return ESP_OK;
	}

	bool is_connected() { return connected.load(); }
	bool auth_rejected() { return rejected.load(); }
	bool auth_accepted() { return accepted.load(); }
	Stats stats() { return {published_batches.load(), published_samples.load(), publish_failures.load(), last_publish_ms.load()}; }

	esp_err_t publish_batch(const TelemetrySample* samples, int count)
	{
		if (!samples || count <= 0) return ESP_ERR_INVALID_ARG;
		if (!client || !connected.load() || rejected.load()) return ESP_ERR_INVALID_STATE;

		timeval now{}; gettimeofday(&now, nullptr);
		const std::int64_t wall_now_ms = static_cast<std::int64_t>(now.tv_sec) * 1000LL + now.tv_usec / 1000LL;
		const std::int64_t uptime_now_ms = esp_timer_get_time() / 1000LL;

		cJSON* plain = cJSON_CreateObject();
		cJSON* readings = cJSON_CreateArray();
		if (!plain || !readings) { cJSON_Delete(plain); cJSON_Delete(readings); return ESP_ERR_NO_MEM; }
		cJSON_AddStringToObject(plain, "session_id", mqtt_session_id);
		cJSON_AddStringToObject(plain, "worker_id", mqtt_worker_id);
		cJSON_AddStringToObject(plain, "device_id", mqtt_device_id);
		cJSON_AddItemToObject(plain, "readings", readings);
		for (int i = 0; i < count; ++i) {
			const auto& sample = samples[i];
			const std::int64_t sample_epoch_ms = wall_now_ms - (uptime_now_ms - sample.source_uptime_ms);
			cJSON* r = cJSON_CreateObject();
			if (!r) { cJSON_Delete(plain); return ESP_ERR_NO_MEM; }
			cJSON_AddNumberToObject(r, "sequence", sample.sequence);
			cJSON_AddNumberToObject(r, "source_uptime_ms", static_cast<double>(sample.source_uptime_ms));
			cJSON_AddNumberToObject(r, "ts", static_cast<double>(sample_epoch_ms));
			cJSON_AddNumberToObject(r, "ax", sample.imu.ax); cJSON_AddNumberToObject(r, "ay", sample.imu.ay);
			cJSON_AddNumberToObject(r, "az", sample.imu.az); cJSON_AddNumberToObject(r, "gx", sample.imu.gx);
			cJSON_AddNumberToObject(r, "gy", sample.imu.gy); cJSON_AddNumberToObject(r, "gz", sample.imu.gz);
			cJSON_AddItemToArray(readings, r);
		}
		char* plaintext = cJSON_PrintUnformatted(plain);
		cJSON_Delete(plain);
		if (!plaintext) return ESP_ERR_NO_MEM;
		const std::size_t plaintext_len = std::strlen(plaintext);

		std::uint8_t key[32]{}; derive_aes_key(key);
		std::uint8_t nonce[12]{}; esp_fill_random(nonce, sizeof(nonce));
		std::uint8_t tag[16]{};
		auto* ciphertext = static_cast<std::uint8_t*>(std::malloc(plaintext_len));
		if (!ciphertext) { cJSON_free(plaintext); return ESP_ERR_NO_MEM; }

		char aad[96]{};
		const int aad_len = std::snprintf(aad, sizeof(aad), "tervanix|v2|%s|%s", mqtt_session_id, mqtt_worker_id);
		mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
		int crypto_rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
		if (crypto_rc == 0) {
			crypto_rc = mbedtls_gcm_crypt_and_tag(
				&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_len,
				nonce, sizeof(nonce), reinterpret_cast<const unsigned char*>(aad), aad_len,
				reinterpret_cast<const unsigned char*>(plaintext), ciphertext,
				sizeof(tag), tag
			);
		}
		mbedtls_gcm_free(&gcm);
		std::memset(key, 0, sizeof(key));
		cJSON_free(plaintext);
		if (crypto_rc != 0) { std::free(ciphertext); publish_failures.fetch_add(1); return ESP_FAIL; }

		char *nonce_b64=nullptr, *cipher_b64=nullptr, *tag_b64=nullptr;
		if (!base64_encode(nonce, sizeof(nonce), nonce_b64) ||
			!base64_encode(ciphertext, plaintext_len, cipher_b64) ||
			!base64_encode(tag, sizeof(tag), tag_b64)) {
			std::free(ciphertext); std::free(nonce_b64); std::free(cipher_b64); std::free(tag_b64);
			return ESP_ERR_NO_MEM;
		}
		std::free(ciphertext);

		cJSON* envelope = cJSON_CreateObject();
		cJSON_AddNumberToObject(envelope, "schema_version", 2);
		cJSON_AddStringToObject(envelope, "alg", "A256GCM");
		cJSON_AddStringToObject(envelope, "session_id", mqtt_session_id);
		cJSON_AddStringToObject(envelope, "worker_id", mqtt_worker_id);
		cJSON_AddStringToObject(envelope, "nonce", nonce_b64);
		cJSON_AddStringToObject(envelope, "ciphertext", cipher_b64);
		cJSON_AddStringToObject(envelope, "tag", tag_b64);
		std::free(nonce_b64); std::free(cipher_b64); std::free(tag_b64);
		char* payload = cJSON_PrintUnformatted(envelope);
		cJSON_Delete(envelope);
		if (!payload) return ESP_ERR_NO_MEM;

		const int mid = esp_mqtt_client_publish(client, telemetry_topic, payload, 0, 1, 0);
		cJSON_free(payload);
		if (mid < 0) { publish_failures.fetch_add(1); return ESP_FAIL; }
		published_batches.fetch_add(1);
		published_samples.fetch_add(static_cast<std::uint64_t>(count));
		last_publish_ms.store(esp_timer_get_time() / 1000LL);
		return ESP_OK;
	}
}  // namespace tervanix::mqtt
