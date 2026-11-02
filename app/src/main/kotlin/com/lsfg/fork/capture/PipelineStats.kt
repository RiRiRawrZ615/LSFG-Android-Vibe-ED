package com.lsfg.fork.capture

import java.util.concurrent.atomic.AtomicLong

/**
 * Process-wide pipeline telemetry. Every counter reflects an actual code
 * event, never a configured target or assumed multiplier:
 *
 *  - acquired:  source frames captured by the frame source (root/shizuku/
 *               mediaprojection/test pattern).
 *  - submitted: frames actually handed to the engine sink / presenter.
 *  - generated: frames produced by the framegen pipeline.
 *  - presented: read live from the WSI presenter (framesPresented), not
 *               counted here.
 *  - dropped:   frames lost before reaching the presenter (with a reason
 *               logged at the drop site).
 *  - errors:    capture / push exceptions.
 *
 * SessionManager starts a reporter thread that logs rates ~once per second
 * while a session is active and resets the counters at session start.
 */
object PipelineStats {
    val acquired = AtomicLong()
    val submitted = AtomicLong()
    val generated = AtomicLong()
    val dropped = AtomicLong()
    val errors = AtomicLong()

    fun reset() {
        acquired.set(0)
        submitted.set(0)
        generated.set(0)
        dropped.set(0)
        errors.set(0)
    }

    fun incAcquired() = acquired.incrementAndGet()
    fun incSubmitted() = submitted.incrementAndGet()
    fun incGenerated(n: Long = 1) = generated.addAndGet(n)
    fun incDropped() = dropped.incrementAndGet()
    fun incErrors() = errors.incrementAndGet()
}
