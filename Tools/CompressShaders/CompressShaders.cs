﻿using System.Runtime.CompilerServices;
namespace CompressShaders;

record struct sShaderBinary
{
	public string name;
	public byte[] data;

	public sShaderBinary( string path )
	{
		name = Path.GetFileNameWithoutExtension( path );
		data = File.ReadAllBytes( path );
	}

	public bool wave64 => name.EndsWith( "64" );
	public string uniqueName => wave64 ? name.Substring( 0, name.Length - 2 ) : name;
}

sealed class FoundShaders
{
	public readonly sShaderBinary[] binaries;
	public readonly string[] names;
	public readonly int[] wave32, wave64;

	public FoundShaders( IEnumerable<sShaderBinary> found )
	{
		binaries = found
			.OrderBy( b => b.name )
			.ToArray();

		names = binaries
			.Select( b => b.uniqueName )
			.Distinct()
			.ToArray();

		wave32 = new int[ names.Length ];
		wave64 = new int[ names.Length ];
		for( int i = 0; i < names.Length; i++ )
		{
			int i32 = findIndex( names[ i ], false );
			int i64 = findIndex( names[ i ], true );
			if( i32 >= 0 && i64 >= 0 )
			{
				wave32[ i ] = i32;
				wave64[ i ] = i64;
				continue;
			}
			if( i32 >= 0 )
			{
				wave32[ i ] = wave64[ i ] = i32;
				continue;
			}
			throw new ApplicationException( $"Wave64 shader {names[ i ]} doesn't have the corresponding Wave32 one" );
		}
	}

	int findIndex( string name, bool wave64 )
	{
		for( int i = 0; i < binaries.Length; i++ )
		{
			sShaderBinary sb = binaries[ i ];
			if( sb.uniqueName != name )
				continue;
			if( sb.wave64 == wave64 )
				return i;
		}
		return -1;
	}
}

class Program
{
	static string getSolutionRoot( [CallerFilePath] string? path = null )
	{
		string? dir = Path.GetDirectoryName( path );
		dir = Path.GetDirectoryName( dir );
		dir = Path.GetDirectoryName( dir );
		return dir ?? throw new ApplicationException();
	}

#if DEBUG
	const string config = "Debug";
#else
	const string config = "Release";
#endif

	static string shadersBinDir( string root )
	{
		return Path.Combine( root, "ComputeShaders", "x64", config );
	}

	static IEnumerable<sShaderBinary> readShaders( string root )
	{
		string dir = shadersBinDir( root );
		foreach( string path in Directory.EnumerateFiles( dir, "*.cso" ) )
			yield return new sShaderBinary( path );
	}

	static void writeHeader( string root, IEnumerable<string> names )
	{
		string path = Path.Combine( root, "Whisper", "D3D", "shaderNames.h" );
		using var stream = File.CreateText( path );
		stream.WriteLine( @"// This header is generated by a tool
#pragma once
#include <stdint.h>

namespace DirectCompute
{
	enum struct eComputeShader: uint16_t
	{" );

		int id = 0;
		foreach( string name in names )
		{
			stream.WriteLine( "\t\t{0} = {1},", name, id );
			id++;
		}
		stream.Write( @"	};

	const char* computeShaderName( eComputeShader cs );
}" );
	}

	static void writeCpp( string root, IEnumerable<string> names )
	{
		string path = Path.Combine( root, "Whisper", "D3D", "shaderNames.cpp" );
		ShaderNames.write( path, names );
	}

	static void writePayloadIDs( StreamWriter stream, string varName, int[] ids )
	{
		stream.Write( @"
static const std::array<uint8_t, {0}> {1} = {{", ids.Length, varName );

		for( int i = 0; i < ids.Length; i++ )
		{
			if( 0 == i % 16 )
				stream.Write( "\r\n\t" );
			else
				stream.Write( ' ' );
			stream.Write( "{0},", ids[ i ] );
		}
		stream.Write( @"
};" );
	}

	static void writePayload( string root, FoundShaders shaders, out int cbSource, out int cbCompressed )
	{
		MemoryStream ms = new MemoryStream();
		List<int> offsets = new List<int>();
		foreach( var bin in shaders.binaries )
		{
			offsets.Add( (int)ms.Length );
			ms.Write( bin.data );
		}
		offsets.Add( (int)ms.Length );

		byte[] dxbc = ms.ToArray();
		byte[] compressed = Cabinet.compressBuffer( dxbc );
		cbSource = dxbc.Length;
		cbCompressed = compressed.Length;

		string path = Path.Combine( root, "Whisper", "D3D", $"shaderData-{config}.inl" );
		using var stream = File.CreateText( path );
		stream.Write( @"// This source file is generated by a tool

// This array contains concatenated and compressed DXBC binaries for all compiled compute shaders
static const std::array<uint8_t, {0}> s_compressedShaders =
{{", compressed.Length );

		for( int i = 0; i < compressed.Length; i++ )
		{
			if( 0 == i % 16 )
				stream.Write( "\r\n\t" );
			else
				stream.Write( ' ' );
			stream.Write( "0x{0:X02},", compressed[ i ] );
		}

		stream.Write( @"
}};

// This array contains start offsets of shader binaries in the decompressed DXBC blob.
// It includes one more entry for the end of the complete decompressed blob.
static const std::array<uint32_t, {0}> s_shaderOffsets = {{", offsets.Count );

		for( int i = 0; i < offsets.Count; i++ )
		{
			if( 0 == i % 16 )
				stream.Write( "\r\n\t" );
			else
				stream.Write( ' ' );
			stream.Write( "{0},", offsets[ i ] );
		}
		stream.Write( @"
};" );

		stream.Write( @"
// Index = eComputeShader enum value, value = index of the shader binary to use on nVidia and Intel GPUs" );
		writePayloadIDs( stream, "s_shaderBlobs32", shaders.wave32 );
		stream.Write( @"
// Index = eComputeShader enum value, value = index of the shader binary to use on AMD GPUs" );
		writePayloadIDs( stream, "s_shaderBlobs64", shaders.wave64 );

		ulong fp64Flags = 0;
		for( int i = 0; i < shaders.binaries.Length; i++ )
		{
			bool fp64 = DetectFp64.usesFp64( shaders.binaries[ i ].data );
			if( fp64 )
				fp64Flags |= (ulong)1 << i;
		}

		stream.Write( @"
// Bitmap of the shader binaries which use FP64 arithmetic instructions
constexpr uint64_t fp64ShadersBitmap = 0x{0:X}ull;", fp64Flags );
	}

	static void mainImpl()
	{
		string root = getSolutionRoot();
		LanguageCodes.produce( root );

		FoundShaders shaders = new FoundShaders( readShaders( root ) );

		writeHeader( root, shaders.names );
		writeCpp( root, shaders.names );
		writePayload( root, shaders, out int cbIn, out int cbOut );
		Console.WriteLine( "Compressed {0} compute shaders, {1:F1} kb -> {2:F1} kb", shaders.binaries.Length, cbIn / 1024.0, cbOut / 1024.0 );
	}

	static int Main( string[] args )
	{
		try
		{
			mainImpl();
			return 0;
		}
		catch( Exception ex )
		{
			Console.WriteLine( ex.Message );
			return ex.HResult;
		}
	}
}