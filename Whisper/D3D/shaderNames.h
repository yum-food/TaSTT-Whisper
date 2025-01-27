// This header is generated by a tool
#pragma once
#include <stdint.h>

namespace DirectCompute
{
	enum struct eComputeShader: uint16_t
	{
		add = 0,
		addInPlace = 1,
		addRepeat = 2,
		addRepeatEx = 3,
		addRepeatGelu = 4,
		addRepeatScale = 5,
		addRows = 6,
		convolutionMain = 7,
		convolutionMain2 = 8,
		convolutionMain2Fixed = 9,
		convolutionPrep1 = 10,
		convolutionPrep2 = 11,
		copyConvert = 12,
		copyTranspose = 13,
		diagMaskInf = 14,
		flashAttention = 15,
		flashAttentionCompat1 = 16,
		flashAttentionCompat2 = 17,
		flashAttentionCompat3 = 18,
		fmaRepeat1 = 19,
		fmaRepeat2 = 20,
		matReshapePanels = 21,
		mulMatByRow = 22,
		mulMatByRowTiled = 23,
		mulMatByRowTiledEx = 24,
		mulMatByScalar = 25,
		mulMatDotMain = 26,
		mulMatDotReshape = 27,
		mulMatMadMain = 28,
		mulMatTiled = 29,
		mulMatTiledEx = 30,
		norm = 31,
		normCompat = 32,
		normFixed = 33,
		scaleInPlace = 34,
		softMax = 35,
		softMaxCompat = 36,
		softMaxFixed = 37,
		softMaxLong = 38,
		zeroMemory = 39,
	};

	const char* computeShaderName( eComputeShader cs );
}