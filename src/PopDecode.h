#pragma once
#include <ofxSoylent.h>
#include <SoyApp.h>
#include <TJob.h>
#include <TChannel.h>
#include <SoyVideoDevice.h>
#include <fstream>

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


class TVideo264 : public TVideoDevice
{
public:
	TVideo264(std::shared_ptr<TBinaryStream> Stream,std::stringstream& Error);
	
	virtual TVideoDeviceMeta	GetMeta() const;
	virtual bool	DecodeNal(const ArrayBridge<char>&& NalPacket)=0;

private:
	void			OnData(bool& Dummy);
	bool			PopNextNal();
	
public:
	std::shared_ptr<TBinaryStream>	mStream;
};



class CoreVideo264 : public TVideo264
{
public:
	CoreVideo264(std::shared_ptr<TBinaryStream> Stream,std::stringstream& Error);

	virtual bool	DecodeNal(const ArrayBridge<char>&& NalPacket) override;
	
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



