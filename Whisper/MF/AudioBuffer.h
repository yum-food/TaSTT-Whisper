#pragma once
#include <algorithm>
#include <vector>

namespace Whisper
{
	struct AudioBuffer
	{
		std::vector<float> mono;
		std::vector<float> stereo;

		void appendMono( const float* rsi, size_t countFloats );
		void appendDownmixedStereo( const float* rsi, size_t countFloats );
		void appendStereo( const float* rsi, size_t countFloats );

		using pfnAppendSamples = void( AudioBuffer::* )( const float* rsi, size_t countFloats );

		inline static pfnAppendSamples appendSamplesFunc( bool sourceMono, bool wantStereo )
		{
			if( sourceMono )
				return &AudioBuffer::appendMono;
			else if( !wantStereo )
				return &AudioBuffer::appendDownmixedStereo;
			else
				return &AudioBuffer::appendStereo;
		}

		void clear()
		{
			mono.clear();
			stereo.clear();
		}

		void swap( AudioBuffer& that )
		{
			mono.swap( that.mono );
			stereo.swap( that.stereo );
		}

		void resize( size_t len )
		{
			assert( len <= mono.size() );
			mono.resize( len );
			if( !stereo.empty() )
				stereo.resize( len * 2 );
		}

		void dropFirst(size_t len)
		{
			if (len >= mono.size()) {
				mono.clear();
				return;
			}
			size_t remainder = mono.size() - len;
			auto tmp = std::vector<float>(remainder);
			memcpy(tmp.data(), mono.data() + len, remainder);
			mono = std::move(tmp);
		}

		void retainLast(size_t len)
		{
			if (len >= mono.size()) {
				return;
			}
			size_t prefix_len = mono.size() - len;
			auto tmp = std::vector<float>(len);
			memcpy(tmp.data(), mono.data() + prefix_len, len);
			mono = std::move(tmp);
		}

		void normalize()
		{
			const auto &min = *std::min_element(mono.begin(), mono.end());
			const auto &max = *std::max_element(mono.begin(), mono.end());

			for (auto& elm : mono) {
				elm -= min;
				elm /= (max - min) + 1;
				elm *= 255.0;
			}
		}
	};
}