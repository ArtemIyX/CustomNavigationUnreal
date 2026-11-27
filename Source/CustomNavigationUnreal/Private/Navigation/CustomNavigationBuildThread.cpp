// Created by Stalker7274

#include "Navigation/CustomNavigationBuildThread.h"
#include "Navigation/CustomNavigationTypes.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace CustomNavigationPrivate
{
	/** Guards GBuildThread: Enqueue (game thread) and Shutdown (module unload) may in theory race. */
	static FCriticalSection GBuildThreadLock;
	static TUniquePtr<FCustomNavBuildThread> GBuildThread;

	/** Recast keeps its work on the heap; 1 MB leaves plenty of room for its few recursive helpers. */
	static constexpr uint32 BuildThreadStackSize = 1024 * 1024;
}

const TCHAR* FCustomNavBuildThread::ThreadName = TEXT("CustomNavBuild");

FCustomNavBuildThread::FCustomNavBuildThread()
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
}

FCustomNavBuildThread::~FCustomNavBuildThread()
{
	if (Thread)
	{
		Stop();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	// Jobs that never ran are destroyed here, releasing their snapshots and completion queues.
	Jobs.Empty();
	FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
	WakeEvent = nullptr;
}

bool FCustomNavBuildThread::Start()
{
	Thread = FRunnableThread::Create(this, ThreadName, CustomNavigationPrivate::BuildThreadStackSize, TPri_BelowNormal);
	return Thread != nullptr;
}

void FCustomNavBuildThread::Enqueue(TUniqueFunction<void()>&& Job)
{
	using namespace CustomNavigationPrivate;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		Job();
		return;
	}

	FScopeLock Lock(&GBuildThreadLock);
	if (!GBuildThread.IsValid())
	{
		TUniquePtr<FCustomNavBuildThread> NewThread(new FCustomNavBuildThread());
		if (!NewThread->Start())
		{
			UE_LOG(LogCustomNavigation, Error, TEXT("Could not create the %s thread; building on the calling thread"), ThreadName);
			Job();
			return;
		}
		GBuildThread = MoveTemp(NewThread);
	}
	GBuildThread->Jobs.Enqueue(MoveTemp(Job));
	GBuildThread->WakeEvent->Trigger();
}

void FCustomNavBuildThread::Shutdown()
{
	using namespace CustomNavigationPrivate;

	TUniquePtr<FCustomNavBuildThread> ToDestroy;
	{
		FScopeLock Lock(&GBuildThreadLock);
		ToDestroy = MoveTemp(GBuildThread);
	}
	// The destructor stops the thread and waits for the running job outside the lock.
	ToDestroy.Reset();
}

uint32 FCustomNavBuildThread::Run()
{
	while (!bStopRequested.load())
	{
		TUniqueFunction<void()> Job;
		while (!bStopRequested.load() && Jobs.Dequeue(Job))
		{
			Job();
			Job.Reset(); // release the snapshot and the completion queue before waiting for the next job
		}
		if (!bStopRequested.load())
		{
			// Auto-reset event: a Trigger that arrived while we were working makes this return at once.
			WakeEvent->Wait();
		}
	}
	return 0;
}

void FCustomNavBuildThread::Stop()
{
	bStopRequested.store(true);
	WakeEvent->Trigger();
}
