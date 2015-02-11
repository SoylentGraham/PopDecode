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




TVideo264::TVideo264(std::shared_ptr<TBinaryStream> Stream,std::stringstream& Error) :
	TVideoDevice		( TVideoDeviceMeta(), Error )
{
	//	subscribe to stream's data event
	if ( !Soy::Assert( Stream != nullptr, "Expected stream" ) )
		return;
	
	Stream->mStream.mOnDataPushed.AddListener( *this, &TVideo264::OnData );
}
	
TVideoDeviceMeta TVideo264::GetMeta() const
{
	TVideoDeviceMeta Meta;
	Meta.mSerial = "DecodedVideo";
	Meta.mVideo = true;
	return Meta;
}

void TVideo264::OnData(bool& Dummy)
{
	
}

bool TVideo264::PopNextNal()
{
	BufferArray<char,4> NalMarker;
	NalMarker.PushBack(0);
	NalMarker.PushBack(0);
	NalMarker.PushBack(0);
	NalMarker.PushBack(1);

	Array<char> NalPacket;
	bool KeepMarker = false;
	if ( !mStream->mStream.Pop( GetArrayBridge(NalMarker), GetArrayBridge(NalPacket), KeepMarker ) )
		return false;
	
	if ( !DecodeNal( GetArrayBridge(NalPacket) ) )
		return false;
	
	return true;
}
	

CoreVideo264::CoreVideo264(std::shared_ptr<TBinaryStream> Stream,std::stringstream& Error) :
	TVideo264	( Stream, Error )
{
	
}
	
bool CoreVideo264::DecodeNal(const ArrayBridge<char>&& NalPacket)
{
	std::Debug << __func__ << std::endl;
	return false;
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




