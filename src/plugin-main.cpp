/*
 * Speechmatics Captions for OBS
 * Real-time speech-to-text captions using Speechmatics RT API
 *
 * 오디오 캡처 → Speechmatics 전송 → 실시간 자막 표시
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── 소스 데이터 구조체 ───
struct speechmatics_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;

	// 핫키
	obs_hotkey_id hotkey_id{OBS_INVALID_HOTKEY_ID};

	// WebSocket
	std::unique_ptr<ix::WebSocket> websocket;
	std::atomic<bool> connected{false};
	std::atomic<bool> recognized{false}; // RecognitionStarted 수신 여부
	std::atomic<bool> captioning{false};
	std::atomic<bool> stopping{false};

	// 오디오 캡처
	obs_source_t *audio_source{nullptr};
	std::string audio_source_name;

	// seq_no 추적 (EndOfStream용)
	std::atomic<int> last_seq_no{0};

	// 자막 상태
	std::mutex text_mutex;
	std::string final_text;
	std::string partial_text;
	std::string translation_final;
	std::string translation_partial;
	int turn_count{0};

	// 설정
	int font_size{48};
	std::string font_face{"Apple SD Gothic Neo"};
	std::string api_key;
	std::string language{"ko"};
	bool translate{false};
	std::string target_lang{"en"};
};

// ─── 텍스트 표시 업데이트 ───
static void update_text_display(speechmatics_caption_data *data, const char *text)
{
	if (!data->text_source)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "flags", 0);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);
	obs_source_update(data->text_source, s);

	obs_data_release(font);
	obs_data_release(s);
}

// ─── 오디오 캡처 콜백 ───
static void audio_capture_callback(void *param, obs_source_t *, const struct audio_data *audio,
				   bool muted)
{
	auto *data = static_cast<speechmatics_caption_data *>(param);

	if (!data->captioning || !data->recognized || !data->websocket || muted)
		return;
	if (!audio->data[0] || audio->frames == 0)
		return;

	// OBS: float32, 48000Hz → Speechmatics: pcm_s16le, 16000Hz
	const float *src = reinterpret_cast<const float *>(audio->data[0]);
	uint32_t src_frames = audio->frames;
	uint32_t dst_frames = src_frames / 3;
	if (dst_frames == 0)
		return;

	std::vector<int16_t> pcm16(dst_frames);
	for (uint32_t i = 0; i < dst_frames; i++) {
		float sample = src[i * 3];
		if (sample > 1.0f)
			sample = 1.0f;
		if (sample < -1.0f)
			sample = -1.0f;
		pcm16[i] = static_cast<int16_t>(sample * 32767.0f);
	}

	data->websocket->sendBinary(
		std::string(reinterpret_cast<const char *>(pcm16.data()), pcm16.size() * sizeof(int16_t)));
}

// ─── transcript 필드에서 텍스트 추출 ───
static std::string extract_transcript(const json &msg)
{
	if (msg.contains("transcript"))
		return msg["transcript"].get<std::string>();

	// fallback: results 배열에서 조합
	std::string text;
	if (msg.contains("results")) {
		for (const auto &r : msg["results"]) {
			std::string content;

			// Translation 형식: results[].content (직접)
			if (r.contains("content") && r["content"].is_string()) {
				content = r["content"].get<std::string>();
			}
			// Transcription 형식: results[].alternatives[].content
			else if (r.contains("alternatives") && !r["alternatives"].empty()) {
				content = r["alternatives"][0].value("content", "");
			}

			if (content.empty())
				continue;

			std::string attaches = r.value("attaches_to", "none");
			if (!text.empty() && attaches == "none")
				text += " ";
			text += content;
		}
	}
	return text;
}

// ─── Speechmatics 메시지 파싱 ───
static void handle_speechmatics_message(speechmatics_caption_data *data, const std::string &msg_str)
{
	try {
		json resp = json::parse(msg_str);
		std::string msg_type = resp.value("message", "");

		// RecognitionStarted — 오디오 전송 시작 가능
		if (msg_type == "RecognitionStarted") {
			data->recognized = true;
			update_text_display(data, "Listening...");
			obs_log(LOG_INFO, "RecognitionStarted received");
			return;
		}

		// AudioAdded — seq_no 추적
		if (msg_type == "AudioAdded") {
			data->last_seq_no = resp.value("seq_no", 0);
			return;
		}

		// Error
		if (msg_type == "Error") {
			std::string reason = resp.value("reason", "Unknown error");
			std::string err_type = resp.value("type", "");
			obs_log(LOG_ERROR, "Speechmatics error [%s]: %s", err_type.c_str(),
				reason.c_str());
			update_text_display(data, ("Error: " + reason).c_str());
			return;
		}

		// EndOfTranscript — 세션 완료
		if (msg_type == "EndOfTranscript") {
			obs_log(LOG_INFO, "EndOfTranscript received");
			return;
		}

		// AddPartialTranscript — 중간 결과 (덮어쓰기됨)
		if (msg_type == "AddPartialTranscript") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);
			data->partial_text = text;

			std::string display = data->final_text + text;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());

			if (data->translate && !data->translation_final.empty()) {
				display += "\n" + data->translation_final + data->translation_partial;
			}

			if (!display.empty())
				update_text_display(data, display.c_str());
			return;
		}

		// AddTranscript — 확정 결과
		if (msg_type == "AddTranscript") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);

			// 확정 텍스트에 추가
			if (!text.empty()) {
				if (!data->final_text.empty())
					data->final_text += " ";
				data->final_text += text;
			}
			data->partial_text.clear();

			// 너무 길면 앞부분 제거 (자막용으로 최근 텍스트만 유지)
			if (data->final_text.size() > 200) {
				size_t space = data->final_text.find(' ', data->final_text.size() - 150);
				if (space != std::string::npos)
					data->final_text = data->final_text.substr(space + 1);
			}

			std::string display = data->final_text;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());

			if (data->translate && !data->translation_final.empty()) {
				display += "\n" + data->translation_final;
			}

			data->turn_count++;
			obs_log(LOG_INFO, "[Final %d] %s", data->turn_count, text.c_str());

			update_text_display(data, display.c_str());
			return;
		}

		// AddPartialTranslation — 번역 중간 결과
		if (msg_type == "AddPartialTranslation") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);
			data->translation_partial = text;

			std::string display = data->final_text + data->partial_text;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());
			if (!text.empty() || !data->translation_final.empty())
				display += "\n" + data->translation_final + text;

			update_text_display(data, display.c_str());
			return;
		}

		// AddTranslation — 번역 확정 결과
		if (msg_type == "AddTranslation") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);
			if (!text.empty()) {
				if (!data->translation_final.empty())
					data->translation_final += " ";
				data->translation_final += text;
			}
			data->translation_partial.clear();

			// 번역도 길이 제한
			if (data->translation_final.size() > 200) {
				size_t space = data->translation_final.find(
					' ', data->translation_final.size() - 150);
				if (space != std::string::npos)
					data->translation_final =
						data->translation_final.substr(space + 1);
			}

			std::string display = data->final_text + data->partial_text;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());
			display += "\n" + data->translation_final;

			update_text_display(data, display.c_str());
			return;
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "JSON parse error: %s", e.what());
	}
}

// ─── 캡셔닝 중지 ───
static void stop_captioning(speechmatics_caption_data *data)
{
	if (!data->captioning)
		return;

	data->stopping = true;
	data->captioning = false;

	if (data->audio_source) {
		obs_source_remove_audio_capture_callback(data->audio_source, audio_capture_callback,
							 data);
		obs_source_release(data->audio_source);
		data->audio_source = nullptr;
	}

	// EndOfStream 전송
	if (data->websocket && data->recognized) {
		json eos;
		eos["message"] = "EndOfStream";
		eos["last_seq_no"] = data->last_seq_no.load();
		data->websocket->send(eos.dump());
		obs_log(LOG_INFO, "EndOfStream sent (last_seq_no=%d)", data->last_seq_no.load());
	}

	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
	}

	data->connected = false;
	data->recognized = false;
	data->stopping = false;
	update_text_display(data, "Speechmatics Captions Ready!");
	obs_log(LOG_INFO, "Captioning stopped");
}

// ─── 캡셔닝 시작 ───
static void start_captioning(speechmatics_caption_data *data)
{
	if (data->captioning)
		stop_captioning(data);

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	obs_source_t *audio_src = obs_get_source_by_name(data->audio_source_name.c_str());
	if (!audio_src) {
		update_text_display(data, "[Select Audio Source!]");
		return;
	}

	update_text_display(data, "Connecting...");

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->final_text.clear();
		data->partial_text.clear();
		data->translation_final.clear();
		data->translation_partial.clear();
		data->turn_count = 0;
		data->last_seq_no = 0;
	}

	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl("wss://eu2.rt.speechmatics.com/v2");

	// Bearer 토큰 인증
	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Bearer " + data->api_key;
	data->websocket->setExtraHeaders(headers);

	// 자동 재연결
	data->websocket->enableAutomaticReconnection();
	data->websocket->setMinWaitBetweenReconnectionRetries(3000);
	data->websocket->setMaxWaitBetweenReconnectionRetries(30000);

	std::string lang = data->language;
	bool do_translate = data->translate;
	std::string trans_lang = data->target_lang;

	data->websocket->setOnMessageCallback(
		[data, lang, do_translate, trans_lang](const ix::WebSocketMessagePtr &msg) {
			switch (msg->type) {
			case ix::WebSocketMessageType::Open: {
				obs_log(LOG_INFO, "Speechmatics WebSocket connected");
				data->connected = true;

				// StartRecognition 메시지 전송
				json config;
				config["message"] = "StartRecognition";
				config["audio_format"] = {{"type", "raw"},
							  {"encoding", "pcm_s16le"},
							  {"sample_rate", 16000}};
				config["transcription_config"] = {{"language", lang},
								  {"enable_partials", true},
								  {"max_delay", 2.0},
								  {"operating_point", "enhanced"}};

				// 번역 활성화
				if (do_translate && !trans_lang.empty()) {
					config["translation_config"] = {
						{"target_languages", json::array({trans_lang})},
						{"enable_partials", true}};
					obs_log(LOG_INFO, "Translation enabled: %s → %s", lang.c_str(),
						trans_lang.c_str());
				}

				std::string cfg = config.dump();
				data->websocket->send(cfg);
				obs_log(LOG_INFO, "StartRecognition sent (%zu bytes)", cfg.size());

				update_text_display(data, "Waiting for recognition...");
				break;
			}

			case ix::WebSocketMessageType::Message:
				handle_speechmatics_message(data, msg->str);
				break;

			case ix::WebSocketMessageType::Error:
				obs_log(LOG_ERROR, "WS error: %s (status=%d)",
					msg->errorInfo.reason.c_str(), msg->errorInfo.http_status);
				data->connected = false;
				data->recognized = false;
				update_text_display(data,
						    ("Error: " + msg->errorInfo.reason).c_str());
				break;

			case ix::WebSocketMessageType::Close:
				obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)",
					msg->closeInfo.code, msg->closeInfo.reason.c_str());
				data->connected = false;
				data->recognized = false;
				if (!data->stopping && data->captioning) {
					std::string reason = msg->closeInfo.reason.empty()
								    ? "Connection closed"
								    : msg->closeInfo.reason;
					update_text_display(data, reason.c_str());
				}
				break;

			default:
				break;
			}
		});

	// 오디오 캡처 등록 (recognized=false이므로 아직 전송 안 함)
	data->audio_source = audio_src;
	obs_source_add_audio_capture_callback(audio_src, audio_capture_callback, data);

	data->captioning = true;
	data->websocket->start();
	obs_log(LOG_INFO, "Caption started, waiting for connection...");
}

// ─── 연결 테스트 (오디오 없이) ───
static void test_connection(speechmatics_caption_data *data)
{
	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
		data->recognized = false;
	}

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	update_text_display(data, "Testing connection...");

	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl("wss://eu2.rt.speechmatics.com/v2");
	data->websocket->disableAutomaticReconnection();

	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Bearer " + data->api_key;
	data->websocket->setExtraHeaders(headers);

	std::string lang = data->language;

	data->websocket->setOnMessageCallback([data, lang](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			data->connected = true;
			json config;
			config["message"] = "StartRecognition";
			config["audio_format"] = {
				{"type", "raw"}, {"encoding", "pcm_s16le"}, {"sample_rate", 16000}};
			config["transcription_config"] = {{"language", lang},
							  {"enable_partials", false}};
			data->websocket->send(config.dump());
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");
			break;
		}
		case ix::WebSocketMessageType::Message: {
			try {
				json resp = json::parse(msg->str);
				std::string msg_type = resp.value("message", "");
				if (msg_type == "RecognitionStarted") {
					update_text_display(data, "Connected! Ready.");
					data->recognized = true;
				} else if (msg_type == "Error") {
					std::string reason = resp.value("reason", "Unknown error");
					update_text_display(data, ("Error: " + reason).c_str());
				}
			} catch (...) {
			}
			break;
		}
		case ix::WebSocketMessageType::Error:
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;
		case ix::WebSocketMessageType::Close:
			if (!data->stopping)
				update_text_display(data, "Test: Disconnected");
			data->connected = false;
			data->recognized = false;
			break;
		default:
			break;
		}
	});

	data->websocket->start();
}

// ─── 핫키: Start/Stop Caption 토글 ───
static void hotkey_toggle_caption(void *private_data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *data = static_cast<speechmatics_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->translate = obs_data_get_bool(settings, "translate");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	obs_data_release(settings);

	if (data->captioning)
		stop_captioning(data);
	else
		start_captioning(data);
}

// ─── 콜백 함수들 ───

static const char *speechmatics_caption_get_name(void *)
{
	return "Speechmatics Captions";
}

static void *speechmatics_caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new speechmatics_caption_data();
	data->source = source;
	data->font_size = (int)obs_data_get_int(settings, "font_size");

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Speechmatics Captions Ready!");
	obs_data_set_int(ts, "font_size", data->font_size);
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "speechmatics_text", ts);
#else
	data->text_source =
		obs_source_create_private("text_ft2_source_v2", "speechmatics_text", ts);
#endif
	obs_data_release(ts);

	data->hotkey_id = obs_hotkey_register_source(source, "speechmatics_toggle_caption",
						     "Toggle Speechmatics Captions",
						     hotkey_toggle_caption, data);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void speechmatics_caption_destroy(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	stop_captioning(data);
	if (data->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(data->hotkey_id);
	if (data->text_source)
		obs_source_release(data->text_source);
	delete data;
}

static void speechmatics_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	data->font_size = (int)obs_data_get_int(settings, "font_size");
	data->font_face = obs_data_get_string(settings, "font_face");
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->translate = obs_data_get_bool(settings, "translate");
	data->target_lang = obs_data_get_string(settings, "target_lang");

	if (!data->captioning && !data->connected) {
		if (!data->api_key.empty())
			update_text_display(data, "Speechmatics Captions Ready!");
		else
			update_text_display(data, "[Set API Key in Properties]");
	}
}

// ─── 버튼 콜백들 ───
static bool on_test_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	obs_data_release(settings);

	if (data->connected) {
		data->stopping = true;
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
		data->recognized = false;
		data->stopping = false;
		update_text_display(data, "Speechmatics Captions Ready!");
	} else {
		test_connection(data);
	}
	return false;
}

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->translate = obs_data_get_bool(settings, "translate");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
	} else {
		start_captioning(data);
	}
	return true;
}

// ─── 오디오 소스 열거 ───
static bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && strlen(name) > 0)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

// Properties UI
static obs_properties_t *speechmatics_caption_get_properties(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	obs_properties_t *props = obs_properties_create();

	// API Key
	obs_properties_add_text(props, "api_key", "Speechmatics API Key", OBS_TEXT_PASSWORD);

	// 오디오 소스 선택
	obs_property_t *audio_list =
		obs_properties_add_list(props, "audio_source", "Audio Source", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);

	// 언어 선택
	obs_property_t *lang =
		obs_properties_add_list(props, "language", "Language", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Korean", "ko");
	obs_property_list_add_string(lang, "English", "en");
	obs_property_list_add_string(lang, "Japanese", "ja");
	obs_property_list_add_string(lang, "Chinese (Mandarin)", "cmn");
	obs_property_list_add_string(lang, "Spanish", "es");
	obs_property_list_add_string(lang, "French", "fr");
	obs_property_list_add_string(lang, "German", "de");

	// 번역 옵션
	obs_properties_add_bool(props, "translate", "Enable Translation");

	obs_property_t *target =
		obs_properties_add_list(props, "target_lang", "Translate To", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(target, "English", "en");
	obs_property_list_add_string(target, "Korean", "ko");
	obs_property_list_add_string(target, "Japanese", "ja");
	obs_property_list_add_string(target, "Chinese (Mandarin)", "cmn");
	obs_property_list_add_string(target, "Spanish", "es");
	obs_property_list_add_string(target, "French", "fr");
	obs_property_list_add_string(target, "German", "de");

	// 폰트 선택
	obs_property_t *font_list =
		obs_properties_add_list(props, "font_face", "Font", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
#ifdef _WIN32
	obs_property_list_add_string(font_list, "Malgun Gothic", "Malgun Gothic");
	obs_property_list_add_string(font_list, "Yu Gothic", "Yu Gothic");
#else
	obs_property_list_add_string(font_list, "Apple SD Gothic Neo", "Apple SD Gothic Neo");
	obs_property_list_add_string(font_list, "Hiragino Sans", "Hiragino Sans");
#endif
	obs_property_list_add_string(font_list, "Noto Sans CJK KR", "Noto Sans CJK KR");
	obs_property_list_add_string(font_list, "Noto Sans CJK JP", "Noto Sans CJK JP");
	obs_property_list_add_string(font_list, "Arial", "Arial");
	obs_property_list_add_string(font_list, "Helvetica", "Helvetica");

	// 폰트 크기
	obs_properties_add_int_slider(props, "font_size", "Font Size", 12, 120, 2);

	// 버튼들
	obs_properties_add_button(props, "test_connection", "Test Connection", on_test_clicked);

	const char *btn_text = (data && data->captioning) ? "Stop Caption" : "Start Caption";
	obs_properties_add_button(props, "start_stop", btn_text, on_start_stop_clicked);

	return props;
}

static void speechmatics_caption_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "api_key", "");
	obs_data_set_default_string(settings, "language", "ko");
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_bool(settings, "translate", false);
	obs_data_set_default_string(settings, "target_lang", "en");
#ifdef _WIN32
	obs_data_set_default_string(settings, "font_face", "Malgun Gothic");
#else
	obs_data_set_default_string(settings, "font_face", "Apple SD Gothic Neo");
#endif
	obs_data_set_default_int(settings, "font_size", 48);
}

static uint32_t speechmatics_caption_get_width(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	return data->text_source ? obs_source_get_width(data->text_source) : 0;
}

static uint32_t speechmatics_caption_get_height(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	return data->text_source ? obs_source_get_height(data->text_source) : 0;
}

static void speechmatics_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
}

// ─── 소스 등록 ───
static obs_source_info speechmatics_caption_source_info = {};

bool obs_module_load(void)
{
	speechmatics_caption_source_info.id = "speechmatics_caption";
	speechmatics_caption_source_info.type = OBS_SOURCE_TYPE_INPUT;
	speechmatics_caption_source_info.output_flags = OBS_SOURCE_VIDEO;
	speechmatics_caption_source_info.get_name = speechmatics_caption_get_name;
	speechmatics_caption_source_info.create = speechmatics_caption_create;
	speechmatics_caption_source_info.destroy = speechmatics_caption_destroy;
	speechmatics_caption_source_info.update = speechmatics_caption_update;
	speechmatics_caption_source_info.get_properties = speechmatics_caption_get_properties;
	speechmatics_caption_source_info.get_defaults = speechmatics_caption_get_defaults;
	speechmatics_caption_source_info.get_width = speechmatics_caption_get_width;
	speechmatics_caption_source_info.get_height = speechmatics_caption_get_height;
	speechmatics_caption_source_info.video_render = speechmatics_caption_video_render;

	obs_register_source(&speechmatics_caption_source_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
