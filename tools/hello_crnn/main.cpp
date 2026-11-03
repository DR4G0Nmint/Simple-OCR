// Step 1 smoke test for the OCR pipeline.
//
// Purpose: prove that NeoML on this machine can (a) construct a CNN with the
// layer types the CRNN needs, (b) run a forward pass, (c) serialize the network
// to a file and (d) load it back into a fresh CDnn. Nothing here is part of the
// final deliverables; the real model is built by the training program.
//
// Build:  scripts/build_neoml.sh && cmake -S . -B build && cmake --build build
// Run:    ./build/tools/hello_crnn/hello_crnn /tmp/hello_crnn.model

#include <NeoML/Dnn/Dnn.h>
#include <NeoML/Dnn/DnnBlob.h>
#include <NeoML/Dnn/Layers/ActivationLayers.h>
#include <NeoML/Dnn/Layers/ConvLayer.h>
#include <NeoML/Dnn/Layers/DataLayer.h>
#include <NeoML/Dnn/Layers/FullyConnectedLayer.h>
#include <NeoML/Dnn/Layers/PoolingLayer.h>
#include <NeoML/Dnn/Layers/SinkLayer.h>
#include <NeoML/Dnn/Layers/SoftmaxLayer.h>
#include <NeoML/ArchiveFile.h>
#include <NeoML/Random.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace NeoML;

static const int Height = 32;
static const int Width = 128;
static const int NumClasses = 10;

// Builds  input(1x32x128) -> conv16/3x3/pad1 -> relu -> maxpool2x2 -> fc10 -> softmax
static void buildNet( CDnn& dnn, CPtr<CDnnBlob>& inputBlob )
{
	IMathEngine& mathEngine = dnn.GetMathEngine();

	CPtr<CDataLayer> data = new CDataLayer( mathEngine );
	data->SetName( "data" );
	inputBlob = CDnnBlob::Create2DImageBlob( mathEngine, CT_Float, 1, 1, Height, Width, 1 );
	data->SetBlob( inputBlob );
	dnn.AddLayer( *data );

	CPtr<CConvLayer> conv = new CConvLayer( mathEngine );
	conv->SetName( "conv0" );
	conv->SetFilterCount( 16 );
	conv->SetFilterHeight( 3 );
	conv->SetFilterWidth( 3 );
	conv->SetPaddingHeight( 1 );
	conv->SetPaddingWidth( 1 );
	dnn.AddLayer( *conv );
	conv->Connect( 0, *data );

	CPtr<CReLULayer> relu = new CReLULayer( mathEngine );
	relu->SetName( "relu0" );
	dnn.AddLayer( *relu );
	relu->Connect( 0, *conv );

	CPtr<CPoolingLayer> pool = new CMaxPoolingLayer( mathEngine );
	pool->SetName( "pool0" );
	pool->SetFilterHeight( 2 );
	pool->SetFilterWidth( 2 );
	pool->SetStrideHeight( 2 );
	pool->SetStrideWidth( 2 );
	dnn.AddLayer( *pool );
	pool->Connect( 0, *relu );

	CPtr<CFullyConnectedLayer> fc = new CFullyConnectedLayer( mathEngine );
	fc->SetName( "fc0" );
	fc->SetNumberOfElements( NumClasses );
	dnn.AddLayer( *fc );
	fc->Connect( 0, *pool );

	CPtr<CSoftmaxLayer> softmax = new CSoftmaxLayer( mathEngine );
	softmax->SetName( "softmax0" );
	dnn.AddLayer( *softmax );
	softmax->Connect( 0, *fc );

	CPtr<CSinkLayer> sink = new CSinkLayer( mathEngine );
	sink->SetName( "sink" );
	dnn.AddLayer( *sink );
	sink->Connect( 0, *softmax );
}

// The CArchive buffers writes and flushes in its destructor, so the file must
// outlive it: do NOT close the file explicitly while the archive is alive.
static bool saveNet( CDnn& dnn, const char* path )
{
	CArchiveFile file( path, CArchive::SD_Storing );
	if( !file.IsOpen() ) {
		std::fprintf( stderr, "cannot open %s for writing\n", path );
		return false;
	}
	{
		CArchive archive( &file, CArchive::SD_Storing );
		archive.Serialize( dnn );
	}
	return true;
}

static bool loadNet( CDnn& dnn, const char* path )
{
	CArchiveFile file( path, CArchive::SD_Loading );
	if( !file.IsOpen() ) {
		std::fprintf( stderr, "cannot open %s for reading\n", path );
		return false;
	}
	{
		CArchive archive( &file, CArchive::SD_Loading );
		archive.Serialize( dnn );
	}
	return true;
}

int main( int argc, char** argv )
{
	const char* path = ( argc > 1 ) ? argv[1] : "/tmp/hello_crnn.model";

	IMathEngine& mathEngine = GetDefaultCpuMathEngine();
	CRandom random( 0x5eed );

	// --- build, fill input with a deterministic pattern, run forward ---
	CDnn dnn( random, mathEngine );
	CPtr<CDnnBlob> inputBlob;
	buildNet( dnn, inputBlob );

	const int inputSize = Height * Width;
	{
		CDnnBlobBuffer<float> buf( *inputBlob, TDnnBlobBufferAccess::Write );
		for( int i = 0; i < inputSize; ++i ) {
			buf[i] = ( ( i * 37 ) % 255 ) / 127.5f - 1.0f;
		}
	}

	dnn.RunOnce();

	CPtr<CSinkLayer> sink = CheckCast<CSinkLayer>( dnn.GetLayer( "sink" ) );
	CPtr<CDnnBlob> out = sink->GetBlob();

	float firstOut = 0.f;
	float outSum = 0.f;
	{
		CDnnBlobBuffer<float> buf( *out, TDnnBlobBufferAccess::Read );
		firstOut = buf[0];
		for( int i = 0; i < buf.Size(); ++i ) {
			outSum += buf[i];
		}
	}

	std::printf( "layers=%d\n", dnn.GetLayerCount() );
	std::printf( "output_size=%d first=%f softmax_sum=%f\n",
		static_cast<int>( out->GetDataSize() ), firstOut, outSum );

	// --- save ---
	if( !saveNet( dnn, path ) ) {
		return 1;
	}
	std::printf( "saved=%s\n", path );

	// --- reload into a fresh network ---
	CDnn restored( random, mathEngine );
	if( !loadNet( restored, path ) ) {
		return 1;
	}
	std::printf( "restored_layers=%d has_conv0=%d\n",
		restored.GetLayerCount(), restored.HasLayer( "conv0" ) ? 1 : 0 );

	// The restored net must produce the same output for the same input.
	CPtr<CDataLayer> restoredData = CheckCast<CDataLayer>( restored.GetLayer( "data" ) );
	CPtr<CDnnBlob> restoredInput = restoredData->GetBlob();
	{
		CDnnBlobBuffer<float> dst( *restoredInput, TDnnBlobBufferAccess::Write );
		for( int i = 0; i < inputSize; ++i ) {
			dst[i] = ( ( i * 37 ) % 255 ) / 127.5f - 1.0f;
		}
	}
	restored.RunOnce();

	CPtr<CSinkLayer> restoredSink = CheckCast<CSinkLayer>( restored.GetLayer( "sink" ) );
	CPtr<CDnnBlob> restoredOut = restoredSink->GetBlob();

	float maxDiff = 0.f;
	{
		CDnnBlobBuffer<float> a( *out, TDnnBlobBufferAccess::Read );
		CDnnBlobBuffer<float> b( *restoredOut, TDnnBlobBufferAccess::Read );
		for( int i = 0; i < a.Size(); ++i ) {
			maxDiff = std::max( maxDiff, std::fabs( a[i] - b[i] ) );
		}
	}
	std::printf( "max_abs_diff_after_reload=%.9f\n", maxDiff );

	const bool ok = dnn.GetLayerCount() == restored.GetLayerCount() && maxDiff < 1e-6f;
	std::printf( "RESULT=%s\n", ok ? "PASS" : "FAIL" );
	return ok ? 0 : 1;
}
