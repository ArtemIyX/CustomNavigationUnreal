// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include <atomic>

class FRunnableThread;
class FEvent;

/**
 * The dedicated navmesh build thread ("CustomNavBuild"), shared by every world.
 *
 * Jobs run one after another in FIFO order. The thread sleeps on an event while the queue is empty, so it costs no CPU
 * when nothing is being built. It is created on the first job and stopped in ShutdownModule.
 *
 * A job is an opaque function that must not touch UObjects: it builds from its own snapshot and pushes the result into
 * its world's completion queue, which the subsystem drains on the game thread (no callbacks into the game thread).
 *
 * Without multithreading support (some commandlets, -nothreading) jobs run inline on the calling thread.
 */
class FCustomNavBuildThread final : public FRunnable
{
public:
	static const TCHAR* ThreadName;

	/** Queues a job, starting the thread if needed. Game thread. */
	static void Enqueue(TUniqueFunction<void()>&& Job);

	/**
	 * Stops the thread: the running job finishes (it should already be cancelled), queued jobs are dropped unrun.
	 * Blocks until the thread has exited. Called from ShutdownModule; a later Enqueue starts a new thread.
	 */
	static void Shutdown();

	virtual ~FCustomNavBuildThread() override;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FCustomNavBuildThread();
	bool Start();

	TQueue<TUniqueFunction<void()>, EQueueMode::Mpsc> Jobs;
	FEvent* WakeEvent = nullptr;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopRequested{false};
};
