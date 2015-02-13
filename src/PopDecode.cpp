#include "PopDecode.h"
#include <TParameters.h>
#include <SoyDebug.h>
#include <TProtocolCli.h>
#include <TProtocolHttp.h>
#include <SoyApp.h>
#include <PopMain.h>
#include <TJobRelay.h>
#include <SoyPixels.h>
#include <SoyString.h>
#include <TFeatureBinRing.h>
#include <SortArray.h>
#include <TChannelLiteral.h>



bool TH264Decoder::Iteration()
{
	//	pop latest packet
	auto Packet = mPacketQueue.Pop();
	if ( !Packet )
		return true;
	
	//	gr: pass pixels and timecode here so allocator can dictate pixel storage?
	if ( !DecodeNalPacket( GetArrayBridge(*Packet) ) )
	{
		std::Debug << "DecodeNalPacket failed... abort?" << std::endl;
		Soy_AssertTodo();
		return false;
	}
	
	return true;
}


TVideoH264::TVideoH264(std::shared_ptr<TBinaryStream> Stream,std::shared_ptr<TH264Decoder> Decoder,std::stringstream& Error) :
	TVideoDevice		( TVideoDeviceMeta(), Error ),
	mDecoder			( Decoder ),
	mStream				( Stream )
{
	//	subscribe to stream's data event
	if ( !Soy::Assert( mStream != nullptr, "Expected stream" ) )
	{
		Error << __func__ << ": Missing stream";
		return;
	}
	if ( !Soy::Assert( mDecoder != nullptr, "Expected decoder" ) )
	{
		Error << __func__ << ": Missing decoder";
		return;
	}
	
	mStream->mStream.mOnDataPushed.AddListener( *this, &TVideoH264::OnData );
	mDecoder->mOnDecodedFrame.AddListener( *this, &TVideoH264::OnFrame );
}
	
TVideoDeviceMeta TVideoH264::GetMeta() const
{
	TVideoDeviceMeta Meta;
	Meta.mSerial = "DecodedVideo";
	Meta.mVideo = true;
	return Meta;
}

void TVideoH264::OnData(bool& Dummy)
{
	while ( PopNextNal() )
	{
	}
}

bool TVideoH264::PopNextNal()
{
	BufferArray<char,4> NalMarker;
	NalMarker.PushBack(0);
	NalMarker.PushBack(0);
	NalMarker.PushBack(0);
	NalMarker.PushBack(1);

	//	make a pool for these packet buffers
	std::shared_ptr<Array<char>> NalPacket( new Array<char>() );
	bool KeepMarker = false;
	if ( !mStream->mStream.Pop( GetArrayBridge(NalMarker), GetArrayBridge(*NalPacket), KeepMarker ) )
		return false;

	//	push onto decoding queue
	mDecoder->QueueNalPacket( NalPacket );
	
	return true;
}

// example helper function that wraps a time into a dictionary
static CFDictionaryRef MakeDictionaryWithDisplayTime(int64_t inFrameDisplayTime)
{
	CFStringRef key = CFSTR("MyFrameDisplayTimeKey");
	CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &inFrameDisplayTime);
	
	return CFDictionaryCreate(kCFAllocatorDefault,
							  (const void **)&key,
							  (const void **)&value,
							  1,
							  &kCFTypeDictionaryKeyCallBacks,
							  &kCFTypeDictionaryValueCallBacks);
}

bool TH264Decoder_VDA::DecodeNalPacket(const ArrayBridge<char>&& NalPacket)
{
	//OSStatus DecodeAFrame(VDADecoder inDecoder, CFDataRef inCompressedFrame, int64_t inFrameDisplayTime)
	auto& inDecoder = mDecoder;
	CFDataRef inCompressedFrame;	//	NalPacket
	int64_t inFrameDisplayTime;		//	display time according to packet

	CFDictionaryRef frameInfo = NULL;
	
	// create a dictionary containg some information about the frame being decoded
	// in this case, we pass in the display time aquired from the stream
	frameInfo = MakeDictionaryWithDisplayTime(inFrameDisplayTime);
	
	// ask the hardware to decode our frame, frameInfo will be retained and pased back to us
	// in the output callback for this frame
	auto Result = VDADecoderDecode( inDecoder, 0, inCompressedFrame, frameInfo);

	// the dictionary passed into decode is retained by the framework so
	// make sure to release it here
	CFRelease(frameInfo);

	if ( Result != kVDADecoderNoErr )
	{
		std::Debug << "VDADecoderDecode failed. err: " << Result << std::endl;
		return false;
	}
	
	return true;
}

bool TH264Decoder_VDA::Init(int Width,int Height,SoyPixelsFormat::Type Format,std::stringstream& Error)
{
	OSType inSourceFormat = "avc1";
	//	CFDataRef inAVCCData
	/*
	OSStatus CreateDecoder(SInt32 inHeight, SInt32 inWidth,
						   OSType inSourceFormat, CFDataRef inAVCCData,
						   VDADecoder *decoderOut)
	*/
	OSStatus status;
	
	CFMutableDictionaryRef decoderConfiguration = NULL;
	CFMutableDictionaryRef destinationImageBufferAttributes = NULL;
	CFDictionaryRef emptyDictionary;
	
	CFNumberRef height = NULL;
	CFNumberRef width= NULL;
	CFNumberRef sourceFormat = NULL;
	CFNumberRef pixelFormat = NULL;

	
	// the avcC data chunk from the bitstream must be present
	if (inAVCCData == NULL) {
		fprintf(stderr, "avc1 decoder configuration data cannot be NULL!\n");
		return paramErr;
	}
	
	// create a CFDictionary describing the source material for decoder configuration
	decoderConfiguration = CFDictionaryCreateMutable(kCFAllocatorDefault,
													 4,
													 &kCFTypeDictionaryKeyCallBacks,
													 &kCFTypeDictionaryValueCallBacks);
	
	height = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &inHeight);
	width = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &inWidth);
	sourceFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &inSourceFormat);
	
	CFDictionarySetValue(decoderConfiguration, kVDADecoderConfiguration_Height, height);
	CFDictionarySetValue(decoderConfiguration, kVDADecoderConfiguration_Width, width);
	CFDictionarySetValue(decoderConfiguration, kVDADecoderConfiguration_SourceFormat, sourceFormat);
	CFDictionarySetValue(decoderConfiguration, kVDADecoderConfiguration_avcCData, inAVCCData);
	
	// create a CFDictionary describing the wanted destination image buffer
	destinationImageBufferAttributes = CFDictionaryCreateMutable(kCFAllocatorDefault,
																 2,
																 &kCFTypeDictionaryKeyCallBacks,
																 &kCFTypeDictionaryValueCallBacks);
	
	OSType cvPixelFormatType = kCVPixelFormatType_422YpCbCr8;
	pixelFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &cvPixelFormatType);
	emptyDictionary = CFDictionaryCreate(kCFAllocatorDefault, // our empty IOSurface properties dictionary
										 NULL,
										 NULL,
										 0,
										 &kCFTypeDictionaryKeyCallBacks,
										 &kCFTypeDictionaryValueCallBacks);
	
	CFDictionarySetValue(destinationImageBufferAttributes, kCVPixelBufferPixelFormatTypeKey, pixelFormat);
	CFDictionarySetValue(destinationImageBufferAttributes,
						 kCVPixelBufferIOSurfacePropertiesKey,
						 emptyDictionary);
	
	// create the hardware decoder object
	status = VDADecoderCreate(decoderConfiguration,
							  destinationImageBufferAttributes,
							  (VDADecoderOutputCallback*)myDecoderOutputCallback,
							  (void *)myUserData,
							  decoderOut);
	
	if (kVDADecoderNoErr != status) {
		fprintf(stderr, "VDADecoderCreate failed. err: %d\n", status);
	}
	
	if (decoderConfiguration) CFRelease(decoderConfiguration);
	if (destinationImageBufferAttributes) CFRelease(destinationImageBufferAttributes);
	if (emptyDictionary) CFRelease(emptyDictionary);
	
	return status;
}


TFileStream::TFileStream(const std::string& Filename,std::stringstream& Error) :
	SoyWorkerThread	( Soy::StreamToString( std::stringstream() << "TFileStream:" << Filename ), SoyWorkerWaitMode::NoWait )
{
	//	gr: would be nice to have an array! MemFileArray maybe, if it can be cross paltform...
	std::shared_ptr<std::ifstream> FileStream( new std::ifstream( Filename, std::ios::binary|std::ios::in ) );
	if ( !FileStream->is_open() )
	{
		Error << "Failed to open " << Filename << " (" << Soy::Platform::GetLastErrorString() << ")";
		return;
	}
	
	mFileStream = FileStream;

	//	start thread
	Start();
}

TFileStream::~TFileStream()
{
	WaitToFinish();
	
	if ( mFileStream )
	{
		mFileStream->close();
		mFileStream.reset();
	}
}

bool TFileStream::Iteration()
{
	if ( !mFileStream )
		return false;
	
	//	read chunks of the file and push into stream
	BufferArray<char,5*1024> Buffer;
	auto BufferBridge = GetArrayBridge( Buffer );
	if ( !Soy::ReadStreamChunk( BufferBridge, *mFileStream ) )
	{
		//	need proper error handling for streams
		std::Debug << "Error reading stream" << std::endl;
		return false;
	}

	mStream.Push( BufferBridge );
	
	return true;
}




TPopDecode::TPopDecode()
{
	AddJobHandler("exit", TParameterTraits(), *this, &TPopDecode::OnExit );

	
	TParameterTraits DecodeParameterTraits;
	DecodeParameterTraits.mAssumedKeys.PushBack("ref");
	DecodeParameterTraits.mAssumedKeys.PushBack("filename");
	AddJobHandler("decode", DecodeParameterTraits, *this, &TPopDecode::OnStartDecode );
}

void TPopDecode::AddChannel(std::shared_ptr<TChannel> Channel)
{
	TChannelManager::AddChannel( Channel );
	if ( !Channel )
		return;
	TJobHandler::BindToChannel( *Channel );
}


void TPopDecode::OnExit(TJobAndChannel& JobAndChannel)
{
	mConsoleApp.Exit();
	
	//	should probably still send a reply
	TJobReply Reply( JobAndChannel );
	Reply.mParams.AddDefaultParam(std::string("exiting..."));
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}


void TPopDecode::OnStartDecode(TJobAndChannel& JobAndChannel)
{
	auto Job = JobAndChannel.GetJob();
	TJobReply Reply( JobAndChannel );
	
	auto Ref = Job.mParams.GetParamAs<std::string>("ref");
	auto Filename = Job.mParams.GetParamAs<std::string>("filename");

	auto& Video = mVideos[Ref];
	if ( Video != nullptr )
	{
		std::stringstream Error;
		Error << "Video with ref " << Ref << " already exists";
		Reply.mParams.AddErrorParam( Error.str() );
		
		TChannel& Channel = JobAndChannel;
		Channel.OnJobCompleted( Reply );

		return;
	}

	std::stringstream Error;

	//	make stream object
	std::shared_ptr<TBinaryStream> Stream( new TFileStream(Filename,Error) );

	//	make video
	Video.reset( new CoreVideo264( Stream, Error ) );
	
	if ( !Error.str().empty() )
		Reply.mParams.AddErrorParam( Error.str() );

	std::stringstream Output;
	Output << "Created video " << Ref;
	Reply.mParams.AddDefaultParam( Output.str() );
	
	TChannel& Channel = JobAndChannel;
	Channel.OnJobCompleted( Reply );
}


//	horrible global for lambda
std::shared_ptr<TChannel> gStdioChannel;
std::shared_ptr<TChannel> gCaptureChannel;



TPopAppError::Type PopMain(TJobParams& Params)
{
	std::cout << Params << std::endl;
	
	TPopDecode App;

	auto CommandLineChannel = std::shared_ptr<TChan<TChannelLiteral,TProtocolCli>>( new TChan<TChannelLiteral,TProtocolCli>( SoyRef("cmdline") ) );
	
	//	create stdio channel for commandline output
	gStdioChannel = CreateChannelFromInputString("std:", SoyRef("stdio") );
	auto HttpChannel = CreateChannelFromInputString("http:8080-8090", SoyRef("http") );
	auto WebSocketChannel = CreateChannelFromInputString("ws:json:9090-9099", SoyRef("websock") );
//	auto WebSocketChannel = CreateChannelFromInputString("ws:cli:9090-9099", SoyRef("websock") );
	auto SocksChannel = CreateChannelFromInputString("cli:7090-7099", SoyRef("socks") );
	
	
	App.AddChannel( CommandLineChannel );
	App.AddChannel( gStdioChannel );
	App.AddChannel( HttpChannel );
	App.AddChannel( WebSocketChannel );
	App.AddChannel( SocksChannel );

	//	when the commandline SENDs a command (a reply), send it to stdout
	auto RelayFunc = [](TJobAndChannel& JobAndChannel)
	{
		if ( !gStdioChannel )
			return;
		TJob Job = JobAndChannel;
		Job.mChannelMeta.mChannelRef = gStdioChannel->GetChannelRef();
		Job.mChannelMeta.mClientRef = SoyRef();
		gStdioChannel->SendCommand( Job );
	};
	CommandLineChannel->mOnJobSent.AddListener( RelayFunc );
	
	//	connect to another app, and subscribe to frames
	bool CreateCaptureChannel = false;
	if ( CreateCaptureChannel )
	{
		auto CaptureChannel = CreateChannelFromInputString("cli://localhost:7070", SoyRef("capture") );
		gCaptureChannel = CaptureChannel;
		CaptureChannel->mOnJobRecieved.AddListener( RelayFunc );
		App.AddChannel( CaptureChannel );
		
		//	send commands from stdio to new channel
		auto SendToCaptureFunc = [](TJobAndChannel& JobAndChannel)
		{
			TJob Job = JobAndChannel;
			Job.mChannelMeta.mChannelRef = gStdioChannel->GetChannelRef();
			Job.mChannelMeta.mClientRef = SoyRef();
			gCaptureChannel->SendCommand( Job );
		};
		gStdioChannel->mOnJobRecieved.AddListener( SendToCaptureFunc );
		
		auto StartSubscription = [](TChannel& Channel)
		{
			TJob GetFrameJob;
			GetFrameJob.mChannelMeta.mChannelRef = Channel.GetChannelRef();
			//GetFrameJob.mParams.mCommand = "subscribenewframe";
			//GetFrameJob.mParams.AddParam("serial", "isight" );
			GetFrameJob.mParams.mCommand = "getframe";
			GetFrameJob.mParams.AddParam("serial", "isight" );
			GetFrameJob.mParams.AddParam("memfile", "1" );
			Channel.SendCommand( GetFrameJob );
		};
		
		CaptureChannel->mOnConnected.AddListener( StartSubscription );
	}
	
	
	/*
	std::string TestFilename = "/users/grahamr/Desktop/ringo.png";
	
	//	gr: bootup commands
	auto BootupGet = [TestFilename](TChannel& Channel)
	{
		TJob GetFrameJob;
		GetFrameJob.mChannelMeta.mChannelRef = Channel.GetChannelRef();
		GetFrameJob.mParams.mCommand = "getfeature";
		GetFrameJob.mParams.AddParam("x", 120 );
		GetFrameJob.mParams.AddParam("y", 120 );
		GetFrameJob.mParams.AddParam("image", TestFilename, TJobFormat("text/file/png") );
		Channel.OnJobRecieved( GetFrameJob );
	};
	
	auto BootupMatch = [TestFilename](TChannel& Channel)
	{
		TJob GetFrameJob;
		GetFrameJob.mChannelMeta.mChannelRef = Channel.GetChannelRef();
		GetFrameJob.mParams.mCommand = "findfeature";
		GetFrameJob.mParams.AddParam("feature", "01011000000000001100100100000000" );
		GetFrameJob.mParams.AddParam("image", TestFilename, TJobFormat("text/file/png") );
		Channel.OnJobRecieved( GetFrameJob );
	};
	

	//	auto BootupFunc = BootupMatch;
	//auto BootupFunc = BootupGet;
	auto BootupFunc = BootupMatch;
	if ( CommandLineChannel->IsConnected() )
		BootupFunc( *CommandLineChannel );
	else
		CommandLineChannel->mOnConnected.AddListener( BootupFunc );
*/
	
	
	//	run
	App.mConsoleApp.WaitForExit();

	gStdioChannel.reset();
	return TPopAppError::Success;
}




