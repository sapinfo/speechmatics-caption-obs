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
#include <cmath>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── 소스 데이터 구조체 ───
struct speechmatics_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;       // 원문용
	obs_source_t *text_source_trans; // 번역용

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
	std::string font_style{"Regular"};
	int font_flags{0};
	std::string api_key;
	std::string language{"ko"};
	std::string display_mode{"original"}; // "original", "translation", or "both"
	std::string target_lang{"en"};
	float eou_silence{0.5f}; // end_of_utterance_silence_trigger (conversation_config)




	// 텍스트 스타일
	uint32_t color1{0xFFFFFFFF}; // ABGR (OBS 내부 포맷)
	uint32_t color2{0xFFFFFFFF};
	bool outline{false};
	bool drop_shadow{false};
	int custom_width{0};
	bool word_wrap{false};
};

// ─── 텍스트 표시 업데이트 ───
static void update_source_text(speechmatics_caption_data *data, obs_source_t *src, const char *text)
{
	if (!src)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", data->font_style.c_str());
	obs_data_set_int(font, "flags", data->font_flags);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);

#ifdef _WIN32
	// text_gdiplus 속성
	obs_data_set_int(s, "color", data->color1);
	obs_data_set_int(s, "opacity", 100);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_int(s, "outline_size", 4);
	obs_data_set_int(s, "outline_color", 0x000000);
	obs_data_set_int(s, "outline_opacity", 100);
	if (data->custom_width > 0) {
		obs_data_set_bool(s, "extents", true);
		obs_data_set_int(s, "extents_cx", data->custom_width);
		obs_data_set_int(s, "extents_cy", 0);
		obs_data_set_bool(s, "extents_wrap", data->word_wrap);
	} else {
		obs_data_set_bool(s, "extents", false);
	}
#else
	// text_ft2_source_v2 속성
	obs_data_set_int(s, "color1", data->color1);
	obs_data_set_int(s, "color2", data->color2);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_bool(s, "drop_shadow", data->drop_shadow);
	obs_data_set_int(s, "custom_width", data->custom_width);
	obs_data_set_bool(s, "word_wrap", data->word_wrap);
#endif

	obs_source_update(src, s);

	obs_data_release(font);
	obs_data_release(s);
}

static void update_text_display(speechmatics_caption_data *data, const char *text)
{
	update_source_text(data, data->text_source, text);
}

static void update_trans_display(speechmatics_caption_data *data, const char *text)
{
	update_source_text(data, data->text_source_trans, text);
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

		// Error — 재연결 중지하고 캡셔닝 종료
		if (msg_type == "Error") {
			std::string reason = resp.value("reason", "Unknown error");
			std::string err_type = resp.value("type", "");
			obs_log(LOG_ERROR, "Speechmatics error [%s]: %s", err_type.c_str(),
				reason.c_str());
			if (data->websocket)
				data->websocket->disableAutomaticReconnection();
			data->captioning = false;
			update_text_display(data, ("Error: " + reason).c_str());
			return;
		}

		// EndOfTranscript — 세션 완료
		if (msg_type == "EndOfTranscript") {
			obs_log(LOG_INFO, "EndOfTranscript received");
			return;
		}

		// AddPartialTranscript — 중간 결과 (누적 final + 현재 partial)
		if (msg_type == "AddPartialTranscript") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);
			data->partial_text = text;

			if (data->display_mode != "translation") {
				std::string display = data->final_text + text;
				while (!display.empty() && display.front() == ' ')
					display.erase(display.begin());
				if (!display.empty())
					update_text_display(data, display.c_str());
			}
			return;
		}

		// AddTranscript — 확정 결과 (누적, EndOfUtterance에서 클리어)
		if (msg_type == "AddTranscript") {
			std::string text = extract_transcript(resp);
			if (text.empty())
				return;

			std::lock_guard<std::mutex> lock(data->text_mutex);
			if (!data->final_text.empty())
				data->final_text += " ";
			data->final_text += text;
			data->partial_text.clear();

			data->turn_count++;
			obs_log(LOG_INFO, "[Final %d] %s", data->turn_count, text.c_str());

			if (data->display_mode != "translation") {
				std::string display = data->final_text;
				while (!display.empty() && display.front() == ' ')
					display.erase(display.begin());
				update_text_display(data, display.c_str());
			}
			return;
		}

		// EndOfUtterance — 발화 종료, 즉시 클리어
		if (msg_type == "EndOfUtterance") {
			std::lock_guard<std::mutex> lock(data->text_mutex);
			obs_log(LOG_INFO, "[EndOfUtterance] %s", data->final_text.c_str());
			data->final_text.clear();
			data->partial_text.clear();
			data->translation_final.clear();
			data->translation_partial.clear();
			if (data->display_mode != "translation")
				update_text_display(data, "");
			if (data->display_mode == "both")
				update_trans_display(data, "");
			return;
		}

		// AddPartialTranslation — 번역 중간 결과
		if (msg_type == "AddPartialTranslation") {
			std::string text = extract_transcript(resp);

			std::lock_guard<std::mutex> lock(data->text_mutex);
			data->translation_partial = text;

			std::string display = data->translation_final + text;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());
			if (!display.empty()) {
				if (data->display_mode == "both")
					update_trans_display(data, display.c_str());
				else if (data->display_mode == "translation")
					update_text_display(data, display.c_str());
			}
			return;
		}

		// AddTranslation — 번역 확정 결과 (누적)
		if (msg_type == "AddTranslation") {
			std::string text = extract_transcript(resp);
			if (text.empty())
				return;

			std::lock_guard<std::mutex> lock(data->text_mutex);
			if (!data->translation_final.empty())
				data->translation_final += " ";
			data->translation_final += text;
			data->translation_partial.clear();

			std::string display = data->translation_final;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());
			if (!display.empty()) {
				if (data->display_mode == "both")
					update_trans_display(data, display.c_str());
				else if (data->display_mode == "translation")
					update_text_display(data, display.c_str());
			}
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
	update_trans_display(data, "");
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
	std::string disp_mode = data->display_mode;
	std::string trans_lang = data->target_lang;
	float eou_silence = data->eou_silence;

	data->websocket->setOnMessageCallback(
		[data, lang, disp_mode, trans_lang, eou_silence](const ix::WebSocketMessagePtr &msg) {
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
								  {"operating_point", "enhanced"}};

				// End-of-utterance: conversation_config 안에 배치 (API 스키마 요구)
				if (eou_silence > 0.0f) {
					double eou_clean = std::round(eou_silence * 10.0) / 10.0;
					config["transcription_config"]["conversation_config"] = {
						{"end_of_utterance_silence_trigger", eou_clean}};
				}

				// 번역 활성화 (translation 또는 both 모드)
				if (disp_mode != "original" && !trans_lang.empty()) {
					config["translation_config"] = {
						{"target_languages", json::array({trans_lang})},
						{"enable_partials", true}};
					obs_log(LOG_INFO, "Translation enabled: %s → %s", lang.c_str(),
						trans_lang.c_str());
				}

				std::string cfg = config.dump();
				data->websocket->send(cfg);
				obs_log(LOG_INFO, "StartRecognition sent: %s", cfg.c_str());

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
				// 치명적 에러 시 재연결 중지
				data->websocket->disableAutomaticReconnection();
				data->captioning = false;
				update_text_display(data,
						    ("Error: " + msg->errorInfo.reason).c_str());
				break;

			case ix::WebSocketMessageType::Close:
				obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)",
					msg->closeInfo.code, msg->closeInfo.reason.c_str());
				data->connected = false;
				data->recognized = false;
				if (!data->stopping && data->captioning) {
					// 서버가 에러로 닫은 경우 (4xxx) 재연결 중지
					if (msg->closeInfo.code >= 4000) {
						data->websocket->disableAutomaticReconnection();
						data->captioning = false;
					}
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
					// 테스트 성공 → 즉시 세션 종료 (quota 소진 방지)
					json eos;
					eos["message"] = "EndOfStream";
					eos["last_seq_no"] = 0;
					data->websocket->send(eos.dump());
				} else if (msg_type == "EndOfTranscript") {
					// EndOfStream 응답 → 커넥션 닫기
					data->websocket->stop();
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
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->eou_silence = (float)obs_data_get_double(settings, "eou_silence");
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

	// 폰트 설정 읽기
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Speechmatics Captions Ready!");
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "speechmatics_text", ts);
#else
	data->text_source =
		obs_source_create_private("text_ft2_source_v2", "speechmatics_text", ts);
#endif
	obs_data_release(ts);

	// 번역용 텍스트 소스
	obs_data_t *ts2 = obs_data_create();
	obs_data_set_string(ts2, "text", "");
#ifdef _WIN32
	data->text_source_trans = obs_source_create_private("text_gdiplus", "speechmatics_text_trans", ts2);
#else
	data->text_source_trans = obs_source_create_private("text_ft2_source_v2", "speechmatics_text_trans", ts2);
#endif
	obs_data_release(ts2);

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
	if (data->text_source_trans)
		obs_source_release(data->text_source_trans);
	delete data;
}

static void speechmatics_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);

	// 폰트 (obs_data_t 오브젝트)
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->eou_silence = (float)obs_data_get_double(settings, "eou_silence");

	// Latency / silence segmentation

	// 텍스트 스타일
	data->color1 = (uint32_t)obs_data_get_int(settings, "color1");
	data->color2 = (uint32_t)obs_data_get_int(settings, "color2");
	data->outline = obs_data_get_bool(settings, "outline");
	data->drop_shadow = obs_data_get_bool(settings, "drop_shadow");
	data->custom_width = (int)obs_data_get_int(settings, "custom_width");
	data->word_wrap = obs_data_get_bool(settings, "word_wrap");

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

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->eou_silence = (float)obs_data_get_double(settings, "eou_silence");
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
		obs_property_set_description(property, "Start Caption");
	} else {
		start_captioning(data);
		obs_property_set_description(property, "Stop Caption");
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
	obs_property_t *p_api =
		obs_properties_add_text(props, "api_key", "Speechmatics API Key", OBS_TEXT_PASSWORD);
	obs_property_set_long_description(
		p_api,
		"Your Speechmatics API key.\n"
		"Get one at https://portal.speechmatics.com → API Keys.\n"
		"The free tier includes monthly real-time minutes; paid plans unlock higher usage.");

	// 오디오 소스 선택
	obs_property_t *audio_list =
		obs_properties_add_list(props, "audio_source", "Audio Source", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);
	obs_property_set_long_description(
		audio_list,
		"OBS audio source to transcribe (microphone, desktop audio, media source, etc.).\n"
		"The source must be active in your current scene collection.\n"
		"Audio is downsampled from 48 kHz float to 16 kHz PCM16 and streamed to\n"
		"Speechmatics over WebSocket.");

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
	obs_property_set_long_description(
		lang,
		"Source language for transcription.\n"
		"Speechmatics uses ISO 639 codes — note that Mandarin Chinese is 'cmn' (not 'zh').\n"
		"Setting the correct language is critical: Speechmatics RT does not auto-detect.");

	// 표시 모드 (Original / Translation / Both)
	obs_property_t *mode =
		obs_properties_add_list(props, "display_mode", "Display Mode", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "Original", "original");
	obs_property_list_add_string(mode, "Translation", "translation");
	obs_property_list_add_string(mode, "Both (Original + Translation)", "both");
	obs_property_set_long_description(
		mode,
		"How captions are displayed:\n"
		"• Original: Show only the source-language transcript.\n"
		"• Translation: Show only the translated text.\n"
		"• Both: Show original (top) and translation (bottom) stacked vertically.\n"
		"\n"
		"Note: Translation and Both modes use extra API quota.");
	obs_property_set_modified_callback(mode, [](obs_properties_t *ps, obs_property_t *,
						    obs_data_t *s) -> bool {
		const char *dm = obs_data_get_string(s, "display_mode");
		bool needs_trans = dm && strcmp(dm, "original") != 0;
		obs_property_set_visible(obs_properties_get(ps, "target_lang"), needs_trans);
		return true;
	});

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
	obs_property_set_long_description(
		target,
		"Target language for translation.\n"
		"Choose a different language than the source — translating to the same language\n"
		"is rejected by the API.");

	// 초기 가시성: display_mode가 "original"이 아닐 때 target_lang 표시
	if (data) {
		obs_property_set_visible(target, data->display_mode != "original");
	}

	// ─── Utterance Detection ───

	obs_property_t *p_eou = obs_properties_add_float(
		props, "eou_silence", "End-of-Utterance Silence (sec)", 0.0, 2.0, 0.1);
	obs_property_set_long_description(
		p_eou,
		"Silence duration (seconds) that triggers end of utterance.\n"
		"0 = disabled (API default segmentation).\n"
		"\n"
		"Recommended:\n"
		"• 0.5-0.8s: Voice AI, live captions — responsive segment breaks\n"
		"• 0.8-1.2s: Dictation — segments at sentence boundaries\n"
		"\n"
		"Range: 0-2.0 seconds (Speechmatics RT API limit).");

	// ─── 텍스트 스타일 ───

	// 폰트 선택 (시스템 폰트 다이얼로그)
	obs_property_t *p_font = obs_properties_add_font(props, "font", "Font");
	obs_property_set_long_description(
		p_font,
		"System font picker for the caption text.\n"
		"For CJK languages, choose a font that includes the needed glyphs\n"
		"(e.g. Apple SD Gothic Neo on macOS, Malgun Gothic on Windows,\n"
		"Noto Sans CJK on Linux).");

	// 텍스트 색상
	obs_property_t *p_color1 = obs_properties_add_color(props, "color1", "Text Color");
	obs_property_set_long_description(p_color1, "Primary text color for captions.");

	obs_property_t *p_color2 =
		obs_properties_add_color(props, "color2", "Text Color 2 (Gradient)");
	obs_property_set_long_description(
		p_color2,
		"Secondary color used for a vertical gradient (macOS/Linux FreeType renderer only).\n"
		"Set to the same value as Text Color for a solid fill.\n"
		"Ignored on Windows (GDI+ renderer does not support gradients).");

	// 텍스트 효과
	obs_property_t *p_outline = obs_properties_add_bool(props, "outline", "Outline");
	obs_property_set_long_description(
		p_outline,
		"Draws a black outline around each glyph for readability over busy backgrounds.\n"
		"Strongly recommended for streaming over gameplay or video footage.");

	obs_property_t *p_shadow = obs_properties_add_bool(props, "drop_shadow", "Drop Shadow");
	obs_property_set_long_description(
		p_shadow,
		"Adds a drop shadow behind the text for extra contrast.\n"
		"Can be combined with Outline. Windows GDI+ renderer ignores this option.");

	// 텍스트 레이아웃
	obs_property_t *p_width = obs_properties_add_int(
		props, "custom_width", "Custom Text Width (0=auto)", 0, 4096, 1);
	obs_property_set_long_description(
		p_width,
		"Maximum width of the text box in pixels (0 = auto-size to text).\n"
		"Set to your stream width (e.g. 1920) and enable Word Wrap to keep\n"
		"long captions from extending off-screen.");

	obs_property_t *p_wrap = obs_properties_add_bool(props, "word_wrap", "Word Wrap");
	obs_property_set_long_description(
		p_wrap,
		"Wraps long caption lines to the next row when they exceed Custom Text Width.\n"
		"Requires Custom Text Width > 0 to take effect.");

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
	obs_data_set_default_string(settings, "display_mode", "original");
	obs_data_set_default_string(settings, "target_lang", "en");
	obs_data_set_default_double(settings, "eou_silence", 0.5);

	// 폰트 기본값 (obs_data_t 오브젝트)
	obs_data_t *font_obj = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font_obj, "face", "Malgun Gothic");
#else
	obs_data_set_default_string(font_obj, "face", "Apple SD Gothic Neo");
#endif
	obs_data_set_default_string(font_obj, "style", "Regular");
	obs_data_set_default_int(font_obj, "size", 48);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	// 텍스트 스타일 기본값
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color2", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_int(settings, "custom_width", 0);
	obs_data_set_default_bool(settings, "word_wrap", false);
}

static uint32_t speechmatics_caption_get_width(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	uint32_t w1 = data->text_source ? obs_source_get_width(data->text_source) : 0;
	if (data->display_mode == "both" && data->text_source_trans) {
		uint32_t w2 = obs_source_get_width(data->text_source_trans);
		return w1 > w2 ? w1 : w2;
	}
	return w1;
}

static uint32_t speechmatics_caption_get_height(void *private_data)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	uint32_t h1 = data->text_source ? obs_source_get_height(data->text_source) : 0;
	if (data->display_mode == "both" && data->text_source_trans) {
		uint32_t h2 = obs_source_get_height(data->text_source_trans);
		return h1 + h2;
	}
	return h1;
}

static void speechmatics_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<speechmatics_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
	if (data->display_mode == "both" && data->text_source_trans) {
		uint32_t h1 = data->text_source ? obs_source_get_height(data->text_source) : 0;
		gs_matrix_push();
		gs_matrix_translate3f(0.0f, (float)h1, 0.0f);
		obs_source_video_render(data->text_source_trans);
		gs_matrix_pop();
	}
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
