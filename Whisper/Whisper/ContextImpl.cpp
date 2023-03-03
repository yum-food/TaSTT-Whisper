#include "stdafx.h"
#include "ContextImpl.h"
#include "Languages.h"
#include "../Utils/Trace/tracing.h"
using namespace Whisper;

ContextImpl::ContextImpl( const WhisperModel& modelData, iModel* modelPointer ) :
	model( modelData ),
	modelPtr( modelPointer ),
	context( modelData, profiler ),
	profiler( modelData )
{ }

#define WHISPER_CHUNK_SIZE  30

HRESULT ContextImpl::encode( iSpectrogram& mel, int seek )
{
	auto prof = profiler.cpuBlock( eCpuBlock::Encode );
	// whisper_encode
	using namespace DirectCompute;

	sEncodeParams ep;
	ep.n_ctx = ( exp_n_audio_ctx > 0 ) ? exp_n_audio_ctx : model.parameters.n_audio_ctx;
	ep.n_mels = model.parameters.n_mels;
	ep.mel_offset = seek;
	ep.layersCount = model.parameters.n_audio_layer;
	ep.n_state = model.parameters.n_audio_state;
	ep.n_head = model.parameters.n_audio_head;
	ep.n_audio_ctx = model.parameters.n_audio_ctx;
	ep.n_text_state = model.parameters.n_text_state;
	ep.n_text_layer = model.parameters.n_text_layer;
	ep.n_text_ctx = model.parameters.n_text_ctx;
	try
	{
		auto cur = context.encode( mel, ep );
		Tracing::tensor( "encode-out", cur );
		return S_OK;
	}
	catch( HRESULT hr )
	{
		return hr;
	}
}

HRESULT ContextImpl::decode( const int* tokens, size_t length, int n_past, int threads, int nth )
{
	// whisper_decode
	using namespace DirectCompute;
	sDecodeParams dp;
	dp.n_state = model.parameters.n_audio_state;
	dp.n_head = model.parameters.n_audio_head;
	dp.n_ctx = model.parameters.n_text_ctx;
	dp.n_past = n_past;
	dp.M = exp_n_audio_ctx > 0 ? exp_n_audio_ctx : model.parameters.n_audio_ctx;
	dp.n_text_layer = model.parameters.n_text_layer;
	dp.n_vocab = model.parameters.n_vocab;

	try
	{
		context.decode( tokens, (int)length, dp, ctx_[nth].probs, threads);
		return S_OK;
	}
	catch( HRESULT hr )
	{
		return hr;
	}
}

std::pair<int, int> ContextImpl::beamGetMinJointProb() const
{
	float min_p = 1.1;
	int min_p_beam;
	int min_p_sample;
	for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
		for (int nth_best = 0; nth_best < ctx_[nth_beam].beam_ctx.best_tokens.size(); nth_best++) {
			const float cur_p = ctx_[nth_beam].beam_ctx.joint_prob *
				ctx_[nth_beam].beam_ctx.best_tokens[nth_best].p;
			if (cur_p < min_p) {
				min_p = cur_p;
				min_p_beam = nth_beam;
				min_p_sample = nth_best;
			}
		}
	}
	return { min_p_beam, min_p_sample };
}

int ContextImpl::beamGetMaxJointProb() const
{
	float max_p = -0.1;
	int max_p_beam;
	for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
		const float cur_p = ctx_[nth_beam].beam_ctx.joint_prob;
		if (cur_p > max_p) {
			max_p = cur_p;
			max_p_beam = nth_beam;
		}
	}
	return max_p_beam;
}

// the most basic sampling scheme - select the top token
std::vector<sTokenData> ContextImpl::sampleBestN( const float* probs,
	bool force_timestamp, bool is_initial, int nth, int n_best )
{
	// whisper_sample_best
	const Vocabulary& vocab = model.vocab;
	std::vector<sTokenData> result_vec(n_best, { 0 });

	size_t n_logits = vocab.size();

	ctx_[nth].probs_id.clear();
	ctx_[nth].probs_id.reserve(n_logits);

	for( size_t i = 0; i < n_logits; i++ )
		ctx_[nth].probs_id.emplace_back(probs[i], (int)i);
	{
		double sum_ts = 0.0;
		double max_ts = -1.0;
		double max_tx = -1.0;

		for( int i = 0; i < vocab.token_beg; i++ )
			max_tx = std::max( max_tx, ctx_[ nth ].probs_id[ i ].first );

		const int i0 = is_initial ? vocab.token_beg + 101 : vocab.token_beg;
		const int i1 = is_initial ? vocab.token_beg + 101 : (int)n_logits;

		// the initial timestamp cannot be larger than 100
		// ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L426-L429
		if( is_initial )
		{
			for( int i = i0; i < n_logits; i++ )
				ctx_[nth].probs_id[i].first = -INFINITY;
		}

		for( int i = vocab.token_beg; i < i1; i++ )
		{
			sum_ts += ctx_[nth].probs_id[i].first;
			if (ctx_[nth].probs_id[i].first > max_ts)
			{
				max_ts = ctx_[nth].probs_id[i].first;
				for (auto& result : result_vec) {
					result.tid = ctx_[nth].probs_id[i].second;
				}
			}
		}

		// if the probability sum of all timestamp tokens is higher than the max probability of the text tokens - sample a
		// timestamp token
		if( sum_ts > max_tx || force_timestamp )
		{
			// ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L430-L438
			for( int i = 0; i < vocab.token_beg; i++ )
				ctx_[nth].probs_id[ i ].first = -INFINITY;
		}

		for (auto& result : result_vec) {
			result.pt = (float)(max_ts / (sum_ts + 1e-10));
			result.ptsum = (float)sum_ts;
		}
	}

	// find the top K tokens
	const int top_k = 4 + n_best - 1;

	std::partial_sort(
		ctx_[nth].probs_id.begin(),
		ctx_[nth].probs_id.begin() + top_k, ctx_[nth].probs_id.end(),
		[]( const std::pair<double, Vocabulary::id>& a, const std::pair<double, Vocabulary::id>& b ) {
			return a.first > b.first;
		} );

	ctx_[nth].probs_id.resize(top_k);

	//printf("\n");
	//for (int i = 0; i < (int) probs_id_vec[nth].size(); i++) {
	//    printf("%d: '%s' %f, %d\n", i,
	//        vocab.id_to_token.at(probs_id_vec[nth][i].second).c_str(),
	//        probs_id_vec[nth][i].first, probs_id_vec[nth][i].second);
	//}

	std::vector<int> res_vec(n_best, 0);
	int i = 0;
	for (int j = 0; j < n_best; j++) {
		// Scan past unwanted tokens.
		while ((ctx_[nth].probs_id[i].second == vocab.token_sot ||
			ctx_[nth].probs_id[i].second == vocab.token_solm ||
			ctx_[nth].probs_id[i].second == vocab.token_not) &&
			i < (int)ctx_[nth].probs_id.size() - 1)
		{
			i++;
		}
		res_vec[j] = i;
	}

	assert(result_vec.size() == res_vec.size());
	for (int i = 0; i < res_vec.size(); i++) {
		result_vec[i].id = ctx_[nth].probs_id[res_vec[i]].second;
		result_vec[i].p = (float)ctx_[nth].probs_id[res_vec[i]].first;
	}

	return result_vec;
}

std::vector<sTokenData> ContextImpl::sampleBestN(int nth, int n_best)
{
	const int n_vocab = model.vocab.n_vocab;
	return sampleBestN(
		ctx_[nth].probs.data() + (ctx_[nth].probs.size() - n_vocab),
		false, false, nth, /*n_best=*/n_best);
}

std::vector<sTokenData> ContextImpl::sampleTimestampN(bool initial, int nth,
	int n_best)
{
	const int n_vocab = model.vocab.n_vocab;
	auto ts = sampleBestN(
		ctx_[nth].probs.data() + (ctx_[nth].probs.size() - n_vocab),
		true, initial, nth, 1)[0];
	return std::vector<sTokenData>(n_best, ts);
}

// a cost-function / heuristic that is high for text that takes longer to pronounce
// Obviously, can be improved
static float voice_length( const char* text )
{
	if( nullptr == text )
		return 0;

	float res = 0.0f;
	while( true )
	{
		const char c = *text;
		if( c == '\0' )
			return res;
		text++;

		// Figure out the increment
		float inc;
		if( c >= '0' && c <= '9' )
			inc = 3.0f;
		else
		{
			switch( c )
			{
			case ' ': inc = 0.01f; break;
			case ',': inc = 2.00f; break;
			case '.':
			case '!':
			case '?':
				inc = 3.00f; break;
			default:
				inc = 1.0f;
			}
		}

		res += inc;
	}
}

static int timestamp_to_sample( int64_t t, int n_samples )
{
	return std::max( 0, std::min( (int)n_samples - 1, (int)( ( t * SAMPLE_RATE ) / 100 ) ) );
}

static int64_t sample_to_timestamp( int i_sample )
{
	return ( 100 * i_sample ) / SAMPLE_RATE;
}

void ContextImpl::expComputeTokenLevelTimestamps( int i_segment, float thold_pt, float thold_ptsum )
{
	// whisper_exp_compute_token_level_timestamps

	auto& segment = result_all[ i_segment ];
	auto& tokens = segment.tokens;

	const int n_samples = energy.size();

	if( n_samples == 0 )
	{
		logWarning( u8"%s: no signal data available", __func__ );
		return;
	}

	const int64_t t0 = segment.t0;
	const int64_t t1 = segment.t1;
	const int n = tokens.size();

	if( n == 0 )
		return;

	if( n == 1 )
	{
		tokens[ 0 ].t0 = t0;
		tokens[ 0 ].t1 = t1;
		return;
	}

	auto& t_beg = this->t_beg;
	auto& t_last = this->t_last;
	auto& tid_last = this->tid_last;

	for( int j = 0; j < n; ++j )
	{
		auto& token = tokens[ j ];

		if( j == 0 )
		{
			if( token.id == model.vocab.token_beg )
			{
				tokens[ j ].t0 = t0;
				tokens[ j ].t1 = t0;
				tokens[ j + 1 ].t0 = t0;

				t_beg = t0;
				t_last = t0;
				tid_last = model.vocab.token_beg;
			}
			else
			{
				tokens[ j ].t0 = t_last;
			}
		}

		const int64_t tt = t_beg + 2 * ( token.tid - model.vocab.token_beg );

		tokens[ j ].id = token.id;
		tokens[ j ].tid = token.tid;
		tokens[ j ].p = token.p;
		tokens[ j ].pt = token.pt;
		tokens[ j ].ptsum = token.ptsum;
		tokens[ j ].vlen = voice_length( model.vocab.string( token.id ) );

		if( token.pt > thold_pt && token.ptsum > thold_ptsum && token.tid > tid_last && tt <= t1 )
		{
			if( j > 0 )
				tokens[ j - 1 ].t1 = tt;
			tokens[ j ].t0 = tt;
			tid_last = token.tid;
		}
	}

	tokens[ n - 2 ].t1 = t1;
	tokens[ n - 1 ].t0 = t1;
	tokens[ n - 1 ].t1 = t1;
	t_last = t1;

	// find intervals of tokens with unknown timestamps
	// fill the timestamps by proportionally splitting the interval based on the token voice lengths
	{
		int p0 = 0;
		int p1 = 0;

		while( true )
		{
			while( p1 < n && tokens[ p1 ].t1 < 0 )
				p1++;

			if( p1 >= n )
				p1--;

			if( p1 > p0 )
			{
				double psum = 0.0;
				for( int j = p0; j <= p1; j++ )
					psum += tokens[ j ].vlen;

				//printf("analyzing %d - %d, psum = %f\n", p0, p1, psum);
				const double dt = tokens[ p1 ].t1 - tokens[ p0 ].t0;

				// split the time proportionally to the voice length
				for( int j = p0 + 1; j <= p1; j++ )
				{
					const double ct = tokens[ j - 1 ].t0 + dt * tokens[ j - 1 ].vlen / psum;
					tokens[ j - 1 ].t1 = ct;
					tokens[ j ].t0 = ct;
				}
			}

			p1++;
			p0 = p1;
			if( p1 >= n )
				break;
		}
	}

	// fix up (just in case)
	for( int j = 0; j < n - 1; j++ )
	{
		if( tokens[ j ].t1 < 0 )
			tokens[ j + 1 ].t0 = tokens[ j ].t1;

		if( j > 0 )
		{
			if( tokens[ j - 1 ].t1 > tokens[ j ].t0 ) {
				tokens[ j ].t0 = tokens[ j - 1 ].t1;
				tokens[ j ].t1 = std::max( tokens[ j ].t0, tokens[ j ].t1 );
			}
		}
	}

	// VAD
	// expand or contract tokens based on voice activity
	{
		constexpr int hw = SAMPLE_RATE / 8;

		for( int j = 0; j < n; j++ )
		{
			if( tokens[ j ].id >= model.vocab.token_eot )
				continue;

			int s0 = timestamp_to_sample( tokens[ j ].t0, n_samples );
			int s1 = timestamp_to_sample( tokens[ j ].t1, n_samples );

			const int ss0 = std::max( s0 - hw, 0 );
			const int ss1 = std::min( s1 + hw, n_samples );

			const int ns = ss1 - ss0;

			float sum = 0.0f;
			for( int k = ss0; k < ss1; k++ )
				sum += this->energy[ k ];

			const float thold = 0.5 * sum / ns;

			{
				int k = s0;
				if( this->energy[ k ] > thold && j > 0 )
				{
					while( k > 0 && this->energy[ k ] > thold )
						k--;
					tokens[ j ].t0 = sample_to_timestamp( k );
					if( tokens[ j ].t0 < tokens[ j - 1 ].t1 )
						tokens[ j ].t0 = tokens[ j - 1 ].t1;
					else
						s0 = k;
				}
				else
				{
					while( this->energy[ k ] < thold && k < s1 )
						k++;
					s0 = k;
					tokens[ j ].t0 = sample_to_timestamp( k );
				}
			}

			{
				int k = s1;
				if( this->energy[ k ] > thold )
				{
					while( k < n_samples - 1 && this->energy[ k ] > thold )
						k++;
					tokens[ j ].t1 = sample_to_timestamp( k );
					if( j < ns - 1 && tokens[ j ].t1 > tokens[ j + 1 ].t0 )
						tokens[ j ].t1 = tokens[ j + 1 ].t0;
					else
						s1 = k;
				}
				else
				{
					while( this->energy[ k ] < thold && k > s0 )
						k--;
					s1 = k;
					tokens[ j ].t1 = sample_to_timestamp( k );
				}
			}
		}
	}
}

static std::string to_timestamp( int64_t t, bool comma = false )
{
	int64_t msec = t * 10;
	int64_t hr = msec / ( 1000 * 60 * 60 );
	msec = msec - hr * ( 1000 * 60 * 60 );
	int64_t min = msec / ( 1000 * 60 );
	msec = msec - min * ( 1000 * 60 );
	int64_t sec = msec / 1000;
	msec = msec - sec * 1000;

	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "%02d:%02d:%02d%s%03d", (int)hr, (int)min, (int)sec, comma ? "," : ".", (int)msec );

	return std::string( buf );
}

class ContextImpl::CurrentSpectrogramRaii
{
	ContextImpl* ctx;
public:
	CurrentSpectrogramRaii( ContextImpl* c, iSpectrogram& mel )
	{
		ctx = c;
		c->currentSpectrogram = &mel;
	}
	~CurrentSpectrogramRaii()
	{
		ctx->currentSpectrogram = nullptr;
	}
};

HRESULT COMLIGHTCALL ContextImpl::runFullImpl( const sFullParams& params, const sProgressSink& progress, iSpectrogram& mel )
{
	// Ported from whisper_full() function
	result_all.clear();
	if( params.flag( eFullParamsFlags::SpeedupAudio ) )
	{
		logError( u8"GPU model doesn't implement the SpeedupAudio flag" );
		return E_NOTIMPL;
	}

	CurrentSpectrogramRaii _cs( this, mel );
	const int seek_start = params.offset_ms / 10;
	const int seek_end = seek_start + ( params.duration_ms == 0 ? (int)mel.getLength() : params.duration_ms / 10 );

	// if length of spectrogram is less than 1s (100 samples), then return
	// basically don't process anything that is less than 1s
	// see issue #39: https://github.com/ggerganov/whisper.cpp/issues/39
	if( seek_end < 100 + seek_start )
		return S_FALSE;

	const int n_ctxt = params.strategy == Whisper::eSamplingStrategy::Greedy ? 1 : params.beam_search.beam_width;
	if (ctx_.size() != n_ctxt) {
		ctx_.assign(n_ctxt, Context());
	}

	// the accumulated text context so far
	if (params.flag(eFullParamsFlags::NoContext)) {
		for (auto& ctx : ctx_) {
			ctx.prompt_past.clear();
		}
	}

	// prepend the prompt tokens to the prompt_past
	if( params.prompt_tokens && params.prompt_n_tokens > 0 )
	{
		// parse tokens from the pointer
		for (auto& ctx : ctx_) {
			for (int i = 0; i < params.prompt_n_tokens; i++) {
				ctx.prompt_past.push_back(params.prompt_tokens[i]);
			}
			std::rotate(
				ctx.prompt_past.begin(),
				ctx.prompt_past.end() - params.prompt_n_tokens,
				ctx.prompt_past.end());
		}
	}

	for (auto& ctx : ctx_) {
		ctx.loop_ctx.prompt.reserve(model.parameters.n_text_ctx);
		ctx.loop_ctx.tokens_cur.reserve(model.parameters.n_text_ctx);
	}

	// overwrite audio_ctx
	exp_n_audio_ctx = params.audio_ctx;

	// these tokens determine the task that will be performed
	std::vector<whisper_token> prompt_init = { model.vocab.token_sot };
	if( model.vocab.is_multilingual() )
	{
		int langId = lookupLanguageId( params.language );
		if( langId < 0 )
		{
			char lang[ 5 ];
			*(uint32_t*)( &lang[ 0 ] ) = params.language;
			lang[ 4 ] = '\0';
			logError( u8"%s: unknown language '%s'", __func__, lang );
			return E_INVALIDARG;
		}

		prompt_init.push_back( model.vocab.token_sot + 1 + langId );
		if( params.flag( eFullParamsFlags::Translate ) )
			prompt_init.push_back( model.vocab.token_translate );
		else
			prompt_init.push_back( model.vocab.token_transcribe );
	}

	// int progress_prev = 0;
	// int progress_step = 5;

	// main loop
	int seek = seek_start;
	// Start measuring "Run" profiler value, both CPU and GPU times
	auto prof = context.completeProfiler();
	bool stoppedPrematurely = false;

	if( params.flag( eFullParamsFlags::NoContext ) )
	{
		CHECK( context.clearState() );
	}

	while( true )
	{
		if( nullptr != progress.pfn )
		{
			const int pos = seek - seek_start;
			const int total = seek_end - seek_start;
			const double percentage = (double)pos / (double)total;
			auto cb = profiler.cpuBlock( eCpuBlock::Callbacks );
			CHECK( progress.pfn( percentage, this, progress.pv ) );
		}

		if( seek + 100 >= seek_end )
			break;

		if( nullptr != params.encoder_begin_callback )
		{
			auto cb = profiler.cpuBlock( eCpuBlock::Callbacks );
			HRESULT hr = params.encoder_begin_callback( this, params.encoder_begin_callback_user_data );
			if( FAILED( hr ) )
				return hr;
			if( hr != S_OK )
			{
				stoppedPrematurely = true;
				break;
			}
		}

		// encode audio features starting at offset seek
		CHECK( encode( mel, seek ) );

		for (auto& ctx : ctx_) {
			// if we have already generated some text, use it as a prompt to condition the next generation
			auto& prompt_past = ctx.prompt_past;
			auto& prompt = ctx.loop_ctx.prompt;

			ctx.n_past = 0;
			prompt.clear();

			if (!prompt_past.empty())
			{
				int n_take = std::min(std::min(params.n_max_text_ctx, model.parameters.n_text_ctx / 2), int(prompt_past.size()));

				prompt = { model.vocab.token_prev };
				prompt.insert(prompt.begin() + 1, prompt_past.end() - n_take, prompt_past.end());

				prompt_past.clear();
				prompt_past.insert(prompt_past.end(), prompt.begin() + 1, prompt.end());
			}

			prompt.insert(prompt.end(), prompt_init.begin(), prompt_init.end());

			ctx.loop_ctx.tokens_cur.clear();
			ctx.loop_ctx.result_len = 0;
			ctx.beam_ctx.joint_prob = 1.0;
			ctx.beam_ctx.beam_done = false;
			ctx.seek_delta = 100 * WHISPER_CHUNK_SIZE;
			// have we already sampled a non-beg timestamp token for the current segment?
			ctx.has_ts = false;
		}

		// print the prompt
		//printf("\n\n");
		//for (int i = 0; i < prompt.size(); i++) {
		//    printf("%s: prompt[%d] = %s\n", __func__, i, ctx->vocab.id_to_token[prompt[i]].c_str());
		//}
		//printf("\n\n");

		bool failed = false;

		{
			// Measure "Decode" profiler value, both CPU and GPU times
			auto prof = context.decodeProfiler();
			for( int i = 0, n_max = model.parameters.n_text_ctx / 2 - 4; i < n_max; i++ )
			{
				if (params.strategy == Whisper::eSamplingStrategy::Greedy) {
					auto& has_ts = ctx_[0].has_ts;
					auto& n_past = ctx_[0].n_past;
					auto& prompt = ctx_[0].loop_ctx.prompt;
					auto& result_len = ctx_[0].loop_ctx.result_len;
					auto& seek_delta = ctx_[0].seek_delta;
					auto& tokens_cur = ctx_[0].loop_ctx.tokens_cur;

					CHECK(decode(prompt.data(), prompt.size(), n_past, params.cpuThreads, /*nth=*/0));

					n_past += (int)prompt.size();
					prompt.clear();

					// very basic greedy sampling strategy:
					//
					//   - always take the most probable token
					//
					// more sophisticated sampling strategies could be implemented here, but we keep it simple
					// feel free to experiment!
					//

					auto p = profiler.cpuBlock( eCpuBlock::Sample );
					const sTokenData token = ( i == 0 )
						? sampleTimestampN( true, /*nth=*/0, /*n_best=*/1)[0]
						: sampleBestN(/*nth=*/0, /*n_best=*/1)[0];

					// timestamp token - update sliding window
					if( token.id > model.vocab.token_beg )
					{
						const int seek_delta_new = 2 * ( token.id - model.vocab.token_beg );

						// do not allow to go back in time
						if( has_ts && seek_delta > seek_delta_new && result_len < i )
							break;

						seek_delta = seek_delta_new;
						result_len = i + 1;
						has_ts = true;
					}

					// add it to the context
					prompt.push_back( token.id );
					tokens_cur.push_back( token );

					//{
					//    const auto tt = token.pt > 0.10 ? ctx->vocab.id_to_token[token.tid] : "[?]";
					//    printf("%s: %10s %6d %6.3f '%s'\n", __func__, tt.c_str(), token.id, token.pt, ctx->vocab.id_to_token[token.id].c_str());
					//}

					// end of segment
					if( token.id == model.vocab.token_eot ||                  // end of text token
						( params.max_tokens > 0 && i >= params.max_tokens ) || // max tokens per segment reached
						( has_ts && seek + seek_delta + 100 >= seek_end )     // end of audio reached
						)
					{
						if( result_len == 0 )
						{
							if( seek + seek_delta + 100 >= seek_end )
								result_len = i + 1;
							else
							{
								failed = true;
								break;
							}
						}

						if( params.flag( eFullParamsFlags::SingleSegment ) )
						{
							result_len = i + 1;
							seek_delta = 100 * WHISPER_CHUNK_SIZE;
						}

						break;
					}

					// sometimes, the decoding can get stuck in a repetition loop
					// this is a simple strategy to avoid such cases - we simply flag the decoding as failed and advance
					// the sliding window by 1 second
					if (i == n_max - 1 && (result_len == 0 || seek_delta < 100 * WHISPER_CHUNK_SIZE / 2))
					{
						failed = true;
						break;
					}
				} else if (params.strategy == Whisper::eSamplingStrategy::BeamSearch) {
					// Get the most likely `beam_wd` tokens for each beam.
					const int beam_wd = params.beam_search.n_best;
					for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
						auto& n_past = ctx_[nth_beam].n_past;
						auto& probs = ctx_[nth_beam].probs;
						auto& probs_prev = ctx_[nth_beam].beam_ctx.probs_prev;
						auto& prompt = ctx_[nth_beam].loop_ctx.prompt;
						auto& tokens_cur = ctx_[nth_beam].loop_ctx.tokens_cur;

						if (prompt.empty()) {
							// The current beam is already complete.
							// TODO we can probably elide this assignment.
							probs = probs_prev;
						}
						else {
							CHECK(decode(prompt.data(), prompt.size(), n_past,
								params.cpuThreads, nth_beam));
							n_past += (int)prompt.size();
							prompt.clear();
							probs_prev = probs;
						}

						// For each beam, pick the top `beam_wd` most likely
						// tokens.
						auto p = profiler.cpuBlock(eCpuBlock::Sample);
						const std::vector<sTokenData> tokens = (i == 0)
							? sampleTimestampN(true, nth_beam, beam_wd)
							: sampleBestN(nth_beam, beam_wd);
						ctx_[nth_beam].beam_ctx.best_tokens = tokens;
					}

					// Of the (`beam_wd` * `n_beams`) selected tokens, identify
					// the `n_beams` tokens with the highest joint probability.
					std::vector<std::pair<int, int>> best_beams_and_tokens(n_ctxt);
					{
						std::pair<int, int> min_beam_sample = beamGetMinJointProb();

						for (int nth_beam = 0; nth_beam < best_beams_and_tokens.size(); nth_beam++) {
							// Initialize to simply pick the first token in each beam.
							// The only important thing is that each element is different.
							best_beams_and_tokens[nth_beam] = { nth_beam, 0 };
						}
						for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
							const auto& joint_prob = ctx_[nth_beam].beam_ctx.joint_prob;
							for (int nth_best = 0; nth_best < beam_wd; nth_best++) {
								// We want to see if this (beam, sample) combo is better than
								// something in the current list of possibilities. To do that,
								// find the lowest-joint-probability parse in the list, and compare
								// against it.
								const auto& min_p_beam = min_beam_sample.first;
								const auto& min_p_sample = min_beam_sample.second;
								const float min_p = ctx_[min_p_beam].beam_ctx.joint_prob *
									ctx_[min_p_beam].beam_ctx.best_tokens[min_p_sample].p;

								const auto& token = ctx_[nth_beam].beam_ctx.best_tokens[nth_best];
								const float cur_p = ctx_[nth_beam].beam_ctx.joint_prob * token.p;
								if (cur_p > min_p) {
									// Better parse found. Record.
									// TODO this could be done in constant time using a heap.
									for (auto& [cur_beam, cur_sample] : best_beams_and_tokens) {
										if (cur_beam == min_p_beam && cur_sample == min_p_sample) {
											cur_beam = nth_beam;
											cur_sample = nth_best;
										}
									}
									min_beam_sample = beamGetMinJointProb();
								}
							}
						}
					}

					// Sort parse in ascending order of beam.
					std::sort(best_beams_and_tokens.begin(),
						best_beams_and_tokens.end(),
						[](const std::pair<int, int> a, const std::pair<int, int> b) {
							return a.first < b.first;
						});

					// Extract tokens for new parse.
					std::vector<sTokenData> new_tokens;
					for (const auto& [nth_beam, nth_best] : best_beams_and_tokens) {
						new_tokens.push_back(
							ctx_[nth_beam].beam_ctx.best_tokens[nth_best]);
					}

					// Update context to reflect new parse. Because the beams are sorted
					// in ascending order, we're always copying from old or
					// identical data.
					for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
#if 0
						// Trivial optimization: only copy if beams differ.
						if (nth_beam == best_beams_and_tokens[nth_beam].first) {
							continue;
						}
#endif
						ctx_[nth_beam] = ctx_[best_beams_and_tokens[nth_beam].first];
					}

					// Emit.
					bool ts_went_backwards = false;
					for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
						auto& beam_done = ctx_[nth_beam].beam_ctx.beam_done;
						auto& joint_prob = ctx_[nth_beam].beam_ctx.joint_prob;
						auto& has_ts = ctx_[nth_beam].has_ts;
						auto& prompt = ctx_[nth_beam].loop_ctx.prompt;
						auto& result_len = ctx_[nth_beam].loop_ctx.result_len;
						auto& seek_delta = ctx_[nth_beam].seek_delta;
						auto& tokens_cur = ctx_[nth_beam].loop_ctx.tokens_cur;
						auto& token = new_tokens[nth_beam];

						if (!beam_done) {
							// Timestamp token: update sliding window.
							if (token.id > model.vocab.token_beg) {
								const int seek_delta_new = 2 * (token.id - model.vocab.token_beg);
								if (has_ts && seek_delta > seek_delta_new && result_len < i) {
									ts_went_backwards = true;
									break;
								}
								seek_delta = seek_delta_new;
								result_len = i + 1;
								has_ts = true;
							}

							prompt.push_back(token.id);
							tokens_cur.push_back(token);
							joint_prob *= token.p;
						}
						if (token.id == model.vocab.token_eot ||
							(params.max_tokens > 0 && i >= params.max_tokens) ||
							(has_ts && seek + seek_delta + 100 >= seek_end)) {
							if (result_len == 0)
							{
								if (seek + seek_delta + 100 >= seek_end)
									result_len = i + 1;
								else
								{
									failed = true;
									break;
								}
							}
							beam_done = true;
						}
					}

					bool all_done = true;
					for (int nth_beam = 0; nth_beam < ctx_.size(); nth_beam++) {
						if (!ctx_[nth_beam].beam_ctx.beam_done) {
							all_done = false;
							break;
						}
					}
					if (all_done || ts_went_backwards || failed) {
						break;
					}
				}
				else {
					logError(u8"%s: unsupported decoding strategy: %d", __func__, (int)params.strategy);
					failed = true;
					break;
				}
			}
		}
		if( failed )
		{
			logError( u8"%s: failed to generate timestamp token - skipping one second", __func__ );
			seek += 100;
			continue;
		}

		const int best_beam = beamGetMaxJointProb();
		auto& prompt_past = ctx_[best_beam].prompt_past;
		auto& result_len = ctx_[best_beam].loop_ctx.result_len;
		auto& seek_delta = ctx_[best_beam].seek_delta;
		auto& tokens_cur = ctx_[best_beam].loop_ctx.tokens_cur;

		// shrink down to result_len
		tokens_cur.resize(result_len);

		for( const auto& r : tokens_cur )
			prompt_past.push_back( r.id );

		// store the text from this iteration
		if( !tokens_cur.empty() )
		{
			int i0 = 0;
			int t0 = seek + 2 * ( tokens_cur.front().tid - model.vocab.token_beg );
			std::string text = "";

			for( int i = 0; i < (int)tokens_cur.size(); i++ )
			{
				//printf("%s: %18s %6.3f %18s %6.3f\n", __func__,
				//        ctx->vocab.id_to_token[tokens_cur[i].id].c_str(), tokens_cur[i].p,
				//        ctx->vocab.id_to_token[tokens_cur[i].tid].c_str(), tokens_cur[i].pt);
				if( params.flag( eFullParamsFlags::PrintSpecial ) || tokens_cur[ i ].id < model.vocab.token_eot )
					text += model.vocab.string( tokens_cur[ i ].id );

				if( tokens_cur[ i ].id > model.vocab.token_beg && !params.flag( eFullParamsFlags::SingleSegment ) )
				{
					const int t1 = seek + 2 * ( tokens_cur[ i ].tid - model.vocab.token_beg );
					if( !text.empty() )
					{
						const bool speedUp = params.flag( eFullParamsFlags::SpeedupAudio );
						const int tt0 = speedUp ? 2 * t0 : t0;
						const int tt1 = speedUp ? 2 * t1 : t1;

						if( params.flag( eFullParamsFlags::PrintRealtime ) )
						{
							if( params.flag( eFullParamsFlags::PrintTimestamps ) )
								logDebug( u8"[%s --> %s]  %s", to_timestamp( tt0 ).c_str(), to_timestamp( tt1 ).c_str(), text.c_str() );
							else
								logDebug( u8"%s", text.c_str() );
						}

						result_all.push_back( { tt0, tt1, text, {} } );
						for( int j = i0; j <= i; j++ )
							result_all.back().tokens.push_back( tokens_cur[ j ] );

						int n_new = 1;

						if( params.flag( eFullParamsFlags::TokenTimestamps ) )
						{
							expComputeTokenLevelTimestamps( (int)result_all.size() - 1, params.thold_pt, params.thold_ptsum );
							if( params.max_len > 0 )
								n_new = wrapSegment( params.max_len );
						}
						if( nullptr != params.new_segment_callback )
						{
							auto cb = profiler.cpuBlock( eCpuBlock::Callbacks );
							HRESULT hr = params.new_segment_callback( this, n_new, params.new_segment_callback_user_data );
							if( FAILED( hr ) )
								return hr;
						}
					}
					text = "";
					while( i < (int)tokens_cur.size() && tokens_cur[ i ].id > model.vocab.token_beg )
						i++;
					i--;
					t0 = t1;
					i0 = i + 1;
				}
			}

			if( !text.empty() )
			{
				const int t1 = seek + seek_delta;

				const bool speedUp = params.flag( eFullParamsFlags::SpeedupAudio );
				const int tt0 = speedUp ? 2 * t0 : t0;
				const int tt1 = speedUp ? 2 * t1 : t1;

				if( params.flag( eFullParamsFlags::PrintRealtime ) )
				{
					if( params.flag( eFullParamsFlags::PrintTimestamps ) )
						logDebug( u8"[%s --> %s]  %s", to_timestamp( tt0 ).c_str(), to_timestamp( tt1 ).c_str(), text.c_str() );
					else
						logDebug( u8"%s", text.c_str() );
				}

				result_all.push_back( { tt0, tt1, text, {} } );
				for( int j = i0; j < (int)tokens_cur.size(); j++ )
					result_all.back().tokens.push_back( tokens_cur[ j ] );

				int n_new = 1;
				if( params.flag( eFullParamsFlags::TokenTimestamps ) )
				{
					expComputeTokenLevelTimestamps( (int)result_all.size() - 1, params.thold_pt, params.thold_ptsum );
					if( params.max_len > 0 )
						n_new = wrapSegment( params.max_len );
				}
				if( nullptr != params.new_segment_callback )
				{
					auto cb = profiler.cpuBlock( eCpuBlock::Callbacks );
					HRESULT hr = params.new_segment_callback( this, n_new, params.new_segment_callback_user_data );
					if( FAILED( hr ) )
						return hr;
				}
			}
		}
		seek += seek_delta;
	}

	if( nullptr != progress.pfn && !stoppedPrematurely )
	{
		auto cb = profiler.cpuBlock( eCpuBlock::Callbacks );
		CHECK( progress.pfn( 1.0, this, progress.pv ) );
	}
	return S_OK;
}
