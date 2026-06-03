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

	/**
	 * Sends EOS to every appsrc in the pipeline, then waits up to TimeoutMs for EOS to reach
	 * the bus (i.e. propagate through encoder/muxer/sink so muxer containers are finalized).
	 * Returns 1 = EOS observed, 0 = timeout, -1 = error / not running.
	 * Call BEFORE tearing down upstream wrappers (UGstAppSrcComponent::CbPipelineStop).
	 */
	virtual int SendEosAndWaitDrain(int TimeoutMs) = 0;

	virtual const char* GetName() const = 0;
	virtual void* GetGPipeline() = 0;
	virtual void* GetGBus() = 0;

protected:
	IGstPipeline() {}
	virtual ~IGstPipeline() {}
};
