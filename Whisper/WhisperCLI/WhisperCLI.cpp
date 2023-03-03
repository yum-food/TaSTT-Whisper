#define WIN32_LEAN_AND_MEAN

#include <Unknwn.h>
#include <windows.h>

#include "Whisper/API/whisperWindows.h"

#include <iostream>
#include <locale>
#include <set>
#include <string>
#include <string_view>

using std::cout;
using std::cerr;
using std::endl;
using namespace Whisper;

struct Config {
	std::wstring audio_path = L"input.wav";
	std::wstring model_path = L"model.bin";
	eSamplingStrategy decode_method = eSamplingStrategy::BeamSearch;
};

bool hasArg(int argc, int shift, char* arg) {
	if (shift + 1 >= argc) {
		cerr << "Error: " << arg << " is missing argument" << endl;
		return false;
	}
	return true;
}

std::wstring cstrToWstr(char* c_str) {
	int length = MultiByteToWideChar(CP_UTF8, 0, c_str, -1, NULL, 0);
	std::wstring result(length, 0);
	MultiByteToWideChar(CP_UTF8, 0, c_str, -1, result.data(), result.size());
	return result;
}


bool parseArgs(int argc, char* argv[], Config& c) {
	int shift = 1;
	while (shift < argc) {
		if (std::string_view(argv[shift]) == "--audio_path") {
			if (!hasArg(argc, shift, argv[shift])) {
				return false;
			}
			c.audio_path = cstrToWstr(argv[shift + 1]);
			shift += 2;
			continue;
		}
		if (std::string_view(argv[shift]) == "--model_path") {
			if (!hasArg(argc, shift, argv[shift])) {
				return false;
			}
			c.model_path = cstrToWstr(argv[shift + 1]);
			shift += 2;
			continue;
		}
		if (std::string_view(argv[shift]) == "--decode_method") {
			if (!hasArg(argc, shift, argv[shift])) {
				return false;
			}
			std::string_view decode_method(argv[shift + 1]);
			if (decode_method == "greedy") {
				cerr << "Using greedy decode " << endl;
				c.decode_method = eSamplingStrategy::Greedy;
			}
			else if (decode_method == "beam") {
				cerr << "Using beam decode " << endl;
				c.decode_method = eSamplingStrategy::BeamSearch;
			}
			else {
				cerr << "Unsupported decode method: " << decode_method << endl;
				return false;
			}
			shift += 2;
			continue;
		}
		cerr << "Unrecognized argument: \"" << argv[shift] << '"' << endl;
		return false;
	}
	return true;
}

int main(int argc, char* argv[])
{
	Config c;
	if (!parseArgs(argc, argv, c)) {
		cerr << "Failed to parse args";
		return 1;
	}

	iMediaFoundation* f = nullptr;
	HRESULT err = initMediaFoundation(&f);
	if (FAILED(err)) {
		cerr << "Failed to init media foundation: " << err << endl;
		return 1;
	}

	Whisper::iAudioBuffer* buffer = nullptr;
	err = f->loadAudioFile(c.audio_path.c_str(), /*stereo=*/false, &buffer);
	if (FAILED(err)) {
		cerr << "Failed to load audio file 'input.wav': " << err << endl;
		return 1;
	}

	Whisper::iModel* model = nullptr;
	err = Whisper::loadModel(c.model_path.c_str(), eModelImplementation::GPU, /*flags=*/0, /*callbacks=*/nullptr, &model);
	if (FAILED(err)) {
		cerr << "Failed to open model 'model.bin': " << err << endl;
		return 1;
	}

	Whisper::iContext* context = nullptr;
	err = model->createContext(&context);
	if (FAILED(err)) {
		cerr << "Failed to create context: " << err << endl;
		return 1;
	}

	Whisper::sFullParams wparams{};
	context->fullDefaultParams(c.decode_method, &wparams);
	if (c.decode_method == eSamplingStrategy::BeamSearch) {
		wparams.beam_search.beam_width = 5;
		wparams.beam_search.n_best = 5;
	}
	wparams.language = Whisper::makeLanguageKey("en");
	wparams.n_max_text_ctx = 100;

	err = context->runFull(wparams, buffer);
	if (FAILED(err)) {
		cerr << "Failed to transcribe: " << err << endl;
		return 1;
	}

	Whisper::iTranscribeResult* result = nullptr;
	err = context->getResults(eResultFlags::Tokens, &result);
	if (FAILED(err)) {
		cerr << "Failed to get transcription results: " << err << endl;
		return 1;
	}

	std::set<int> special_tokens;
	{
		Whisper::SpecialTokens st;
		err = model->getSpecialTokens(st);
		if (FAILED(err)) {
			cerr << "Failed to get special tokens: " << err << endl;
		}
		special_tokens.insert(st.Not);
		special_tokens.insert(st.PreviousWord);
		special_tokens.insert(st.SentenceStart);
		special_tokens.insert(st.TaskTranscribe);
		special_tokens.insert(st.TaskTranslate);
		special_tokens.insert(st.TranscriptionBegin);
		special_tokens.insert(st.TranscriptionEnd);
		special_tokens.insert(st.TranscriptionStart);
	}

	sTranscribeLength length;
	err = result->getSize(length);
	if (FAILED(err)) {
		cerr << "Failed to get transcription length: " << err << endl;
	}
	auto* segments = result->getSegments();
	auto* tokens = result->getTokens();
	bool is_metadata = false;
	for (int i = 0; i < length.countSegments; i++) {
		auto& segment = segments[i];
		for (int j = 0; j < segment.countTokens; j++) {
			const sToken& tok = tokens[segment.firstToken + j];
			if (special_tokens.contains(tok.id)) {
				continue;
			}
			std::string_view tok_str(tok.text);
			if (tok_str.starts_with("[") ||
				tok_str.starts_with(" [")) {
				if (tok_str.ends_with("]")) {
					continue;
				}
				is_metadata = true;
				continue;
			}
			if (is_metadata &&
				tok_str.ends_with("]")) {
				is_metadata = false;
				continue;
			}
			cout << tok.text;
		}
	}
	cout << endl;

	return 0;
}