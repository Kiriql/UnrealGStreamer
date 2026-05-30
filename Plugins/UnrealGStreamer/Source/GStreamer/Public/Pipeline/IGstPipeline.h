#pragma once

#include <cstddef>

class IGstPipeline
{
public:
	static IGstPipeline* CreateInstance();
	virtual void Destroy() = 0;

	virtual bool Init(const char* Name, const char* Config, char* OutErrBuf, size_t ErrBufSize) = 0;
	virtual void Shutdown() = 0;
	virtual bool Start() = 0;
	virtual void Stop() = 0;

	virtual const char* GetName() const = 0;
	virtual void* GetGPipeline() = 0;
	virtual void* GetGBus() = 0;

protected:
	IGstPipeline() {}
	virtual ~IGstPipeline() {}
};
