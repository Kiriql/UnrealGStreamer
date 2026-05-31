#pragma once

#include <cstddef>

#include "Core/GstMemoryHandle.h"

class IGstPipeline;

class IGstAppSrcBuffer
{
public:
	virtual void Release() = 0;
	virtual void* GetDataPtr() = 0;
	virtual size_t GetDataSize() = 0;

protected:
	virtual ~IGstAppSrcBuffer() {}
};

class IGstAppSrc
{
public:
	static IGstAppSrc* CreateInstance();
	virtual void Destroy() = 0;

	virtual bool Connect(IGstPipeline* Pipeline, const char* ElementName) = 0;
	virtual void Disconnect() = 0;
	virtual bool SetCaps(const char* CapsString) = 0;
	virtual bool PushBuffer(IGstAppSrcBuffer* Buffer) = 0;
	virtual bool PushSharedBuffer(FGstMemoryHandle* Memory) = 0;

protected:
	IGstAppSrc() {}
	virtual ~IGstAppSrc() {}
};
