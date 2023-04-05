#pragma once
#include <algorithm>
#include <fstream>
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
			}
		}

		void save(const char* path, const int sample_rate) {
			const int n_samples = mono.size();
			const int bits_per_sample = sizeof(mono[0]) * 8;
			const int n_channels = 1;
			const int byte_rate = sample_rate * n_channels * bits_per_sample / 8;
			const int block_align = n_channels * bits_per_sample / 8;
			const int data_chunk_size = n_samples * n_channels * bits_per_sample / 8;
			const int file_size = 36 + data_chunk_size;

			std::ofstream ofs(path, std::ios::out | std::ios::binary);
			ofs.write("RIFF", 4);
			ofs.write((char*)&file_size, 4);
			ofs.write("WAVE", 4);

			ofs.write("fmt ", 4);
			const int fmt_chunk_size = 16;
			ofs.write((char*)&fmt_chunk_size, 4);
			const short audio_format = 1;  // PCM
			ofs.write((char*)&audio_format, 2);
			ofs.write((char*)&n_channels, 2);
			ofs.write((char*)&sample_rate, 4);
			ofs.write((char*)&byte_rate, 4);
			ofs.write((char*)&block_align, 2);
			ofs.write((char*)&bits_per_sample, 2);

			ofs.write("data", 4);
			ofs.write((char*)&data_chunk_size, 4);
			for (int i = 0; i < n_samples; i++) {
				short sample = (short)(mono[i] * 32767.0f);
				ofs.write((char*)&sample, 2);
			}
		};
	};
}