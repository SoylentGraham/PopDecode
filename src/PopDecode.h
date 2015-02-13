#pragma once
#include <ofxSoylent.h>
#include <SoyApp.h>
#include <TJob.h>
#include <TChannel.h>
#include <SoyVideoDevice.h>
#include <fstream>


class TBinaryStream;
class TH264Decoder;



class TBinaryStream
{
public:
	TChannelStream	mStream;
};

class TFileStream : public TBinaryStream, public SoyWorkerThread
{
public:
	TFileStream(const std::string& Filename,std::stringstream& Error);
	~TFileStream();
	
	virtual bool	Iteration() override;
	
private:
	std::shared_ptr<std::ifstream>	mFileStream;
};


class TVideoH264 : public TVideoDevice
{
public:
	TVideoH264(std::shared_ptr<TBinaryStream> Stream,std::shared_ptr<TH264Decoder> Decoder,std::stringstream& Error);
	
	virtual TVideoDeviceMeta	GetMeta() const;

private:
	void			OnData(bool& Dummy);
	void			OnFrame(std::tuple<const SoyPixelsImpl&,SoyTime>& Frame);
	bool			PopNextNal();
	
public:
	std::shared_ptr<TH264Decoder>	mDecoder;
	std::shared_ptr<TBinaryStream>	mStream;
};


class TH264Decoder : public SoyWorkerThread
{
public:
	TH264Decoder(const std::string& DecoderName) :
		SoyWorkerThread	( DecoderName, SoyWorkerWaitMode::Wake )
	{
		WakeOnEvent( mPacketQueue.mOnQueueAdded );
		Start();
	}

	void				QueueNalPacket(std::shared_ptr<Array<char>>& NalPacket)
	{
		mPacketQueue.Push( NalPacket );
	}
	virtual bool		DecodeNalPacket(const ArrayBridge<char>&& NalPacket)=0;

protected:
	virtual bool		Iteration() final;
	void				OnDecodedFrame(const SoyPixelsImpl& Pixels,SoyTime Timecode)
	{
		std::tuple<const SoyPixelsImpl&,SoyTime> Frame( Pixels, Timecode );
		mOnDecodedFrame.OnTriggered( Frame );
	}
	
private:
	TLockQueue<std::shared_ptr<Array<char>>>	mPacketQueue;
	
public:
	SoyEvent<std::tuple<const SoyPixelsImpl&,SoyTime>>	mOnDecodedFrame;
};

//	OSX video decoder accelleration frameowkr
//	https://developer.apple.com/library/mac/technotes/tn2267/_index.html
//	http://stackoverflow.com/questions/23282958/h-264-data-packets-to-realtime-playback-preview-using-apples-videotoolbox
class TH264Decoder_VDA : public TH264Decoder
{
public:
	TH264Decoder_VDA() :
		TH264Decoder	( "VDA" )
	{
	}
	
	virtual bool		DecodeNalPacket(const ArrayBridge<char>&& NalPacket) override;
	
private:
	bool				Init(int Width,int Height,SoyPixelsFormat::Type Format,std::stringstream& Error);
	
private:
	VDADecoder			mDecoder;
};

class TH264Decoder_Libav : public TH264Decoder
{
public:
	TH264Decoder_Libav() :
		TH264Decoder	( "Libav" )
	{
	}
	
	virtual bool		DecodeNalPacket(const ArrayBridge<char>&& NalPacket) override;
};



class TPopDecode : public TJobHandler, public TChannelManager
{
public:
	TPopDecode();
	
	virtual void	AddChannel(std::shared_ptr<TChannel> Channel) override;

	void			OnExit(TJobAndChannel& JobAndChannel);
	void			OnStartDecode(TJobAndChannel& JobAndChannel);
	
public:
	Soy::Platform::TConsoleApp	mConsoleApp;
	
	std::map<std::string,std::shared_ptr<TVideo264>>	mVideos;	//	videos we're processing and their id's
};



