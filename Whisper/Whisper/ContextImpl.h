#pragma once
#include "../API/iContext.cl.h"
#include "../ComLightLib/comLightServer.h"
#include "WhisperContext.h"
#include "Spectrogram.h"
#include "TranscribeResult.h"
#include "sTokenData.h"

namespace Whisper
{
	class ContextImpl : public ComLight::ObjectRoot<iContext>
	{
		const WhisperModel& model;
		ComLight::CComPtr<iModel> modelPtr;
		DirectCompute::WhisperContext context;
		Spectrogram spectrogram;
		int64_t mediaTimeOffset = 0;
		iSpectrogram* currentSpectrogram = nullptr;
		class CurrentSpectrogramRaii;
		ProfileCollection profiler;

		HRESULT COMLIGHTCALL getModel( iModel** pp ) override final;
		HRESULT COMLIGHTCALL timingsPrint() override final;
		HRESULT COMLIGHTCALL timingsReset() override final;
		HRESULT COMLIGHTCALL fullDefaultParams( eSamplingStrategy strategy, sFullParams* rdi ) override final;
		HRESULT COMLIGHTCALL runFullImpl( const sFullParams& params, const sProgressSink& progress, iSpectrogram& mel );
		HRESULT COMLIGHTCALL runFull( const sFullParams& params, const iAudioBuffer* buffer ) override final;
		HRESULT COMLIGHTCALL runStreamed( const sFullParams& params, const sProgressSink& progress, const iAudioReader* reader ) override final;
		HRESULT COMLIGHTCALL runCapture( const sFullParams& params, const sCaptureCallbacks& callbacks, const iAudioCapture* reader ) override final;

		struct Segment
		{
			int64_t t0;
			int64_t t1;
			std::string text;
			std::vector<sTokenData> tokens;
			size_t memoryUsage() const;
		};
		std::vector<Segment> result_all;

		struct Context {
			std::vector<whisper_token> prompt_past;
			std::vector<float> probs;
			std::vector<std::pair<double, Vocabulary::id>> probs_id;
			int seek_delta;
			bool has_ts;
			int n_past = 0;
			// These are cleared on every frame of audio processed.
			struct AudioFrameContext {
				std::vector<whisper_token> prompt;
				std::vector<sTokenData> tokens_cur;

				int result_len = 0;
			};
			struct BeamSearchContext {
				// Each beam picks the N most likely tokens and accumulates
				// them here.
				std::vector<sTokenData> best_tokens;
				// Some beams may finish earlier than others, in which case
				// we'd like to avoid re-running inference.
				std::vector<float> probs_prev;
				// Joint log-probability of every token leading up to the
				// current context.
				float joint_logprob;
				bool beam_done;
			};
			AudioFrameContext loop_ctx;
			BeamSearchContext beam_ctx;
		};
		std::vector<Context> ctx_;

		// [EXPERIMENTAL] token-level timestamps data
		int64_t t_beg = 0;
		int64_t t_last = 0;
		whisper_token tid_last = 0;
		std::vector<float> energy; // PCM signal energy

		// [EXPERIMENTAL] speed-up techniques
		int32_t exp_n_audio_ctx = 0; // 0 - use default

		HRESULT encode( iSpectrogram& mel, int seek );
		HRESULT decode( const int* tokens, size_t length, int n_past, int threads, int nth );
		std::vector<sTokenData> sampleBestN( const float* probs, bool force_timestamp, bool is_initial, int nth, int n_best );
		std::vector<sTokenData> sampleBestN(int nth, int n_best);
		std::vector<sTokenData> sampleTimestampN( bool initial, int nth, int n_best );
		int wrapSegment( int max_len );
		void expComputeTokenLevelTimestamps( int i_segment, float thold_pt, float thold_ptsum );

		// Return the (nth beam, nth best) pair with the lowest joint probability.
		std::pair<int, int> beamGetMinJointProb() const;
		// Return the (nth beam, nth best) pair with the highest joint probability.
		int beamGetMaxJointProb() const;

		mutable TranscribeResultStatic results;

		HRESULT COMLIGHTCALL makeResults( eResultFlags flags, TranscribeResult& res ) const noexcept;

		HRESULT COMLIGHTCALL getResults( eResultFlags flags, iTranscribeResult** pp ) const noexcept override final;
		HRESULT COMLIGHTCALL detectSpeaker( const sTimeInterval& time, eSpeakerChannel& result ) const noexcept override final;

		int defaultThreadsCount() const;

		__m128i getMemoryUse() const;
		mutable std::vector<StereoSample> diarizeBuffer;

	public:

		ContextImpl( const WhisperModel& modelData, iModel* modelPointer );
	};
}