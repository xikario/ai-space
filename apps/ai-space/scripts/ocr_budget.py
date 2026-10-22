"""Bounded, low-priority OCR children; no work or timer when idle."""
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time


class ResourcePressureError(InterruptedError):
    pass


def available_memory():
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        pass
    return None


def check_memory_pressure(stage, starting=True):
    """Physical reserve is independent of the worker's virtual address cap."""
    required_mib = 384 if starting else 256
    free = available_memory()
    if free is not None and free < required_mib * 1024 ** 2:
        raise ResourcePressureError(
            "系统可用内存约 %d MiB，%s需要至少保留 %d MiB；任务已暂停，已提交进度保留，请释放内存后继续"
            % (max(0, free // (1024 ** 2)), stage, required_mib)
        )


def run_ocr(arguments, stdout=None, timeout=120):
    percent = max(5, min(100, int(os.environ.get("AI_SPACE_OCR_CPU_PERCENT", "25"))))
    wall_timeout = max(1, float(timeout)) * 100.0 / percent + 5
    memory = max(512, min(8192, int(os.environ.get("AI_SPACE_OCR_MEMORY_MB", "1024")))) * 1024 ** 2
    check_memory_pressure("OCR 启动")
    environment = dict(
        os.environ, OMP_THREAD_LIMIT="1", OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1"
    )

    def limits():
        try:
            os.nice(19)
        except OSError:
            pass
        # Preserve the user's allowed CPU set. Single-threaded OCR and the
        # duty-cycle limit control resource use without pinning to a LITTLE core.
        resource.setrlimit(resource.RLIMIT_AS, (memory, memory))
        resource.setrlimit(resource.RLIMIT_FSIZE, (256 * 1024 ** 2, 256 * 1024 ** 2))
        resource.setrlimit(resource.RLIMIT_CPU, (max(1, int(timeout)), max(2, int(timeout) + 1)))

    # All children inherit the pipeline process group, so pausing/closing the
    # application also terminates a stopped OCR child. Continue before cleanup.
    with tempfile.TemporaryFile() as errors:
        child = subprocess.Popen(
            arguments, stdout=stdout, stderr=errors, env=environment, preexec_fn=limits
        )
        started = time.monotonic()
        old_handler = signal.getsignal(signal.SIGTERM)

        def cancel(signum, frame):
            raise InterruptedError("OCR task paused; completed documents retained")

        signal.signal(signal.SIGTERM, cancel)
        try:
            while child.poll() is None:
                if time.monotonic() - started > wall_timeout:
                    raise subprocess.TimeoutExpired(arguments, wall_timeout)
                check_memory_pressure("OCR 运行", starting=False)
                directory = os.environ.get("AI_SPACE_STORAGE_DIRECTORY")
                if directory:
                    from knowledge_library import storage_plan

                    storage_plan(
                        directory,
                        0,
                        float(os.environ.get("AI_SPACE_RESERVE_GIB", "2")),
                        float(os.environ.get("AI_SPACE_MAX_GIB", "32")),
                    )
                time.sleep(0.05)
                if percent < 100 and child.poll() is None:
                    try:
                        os.kill(child.pid, signal.SIGSTOP)
                    except ProcessLookupError:
                        continue
                    time.sleep(0.05 * (100 - percent) / percent)
                    try:
                        os.kill(child.pid, signal.SIGCONT)
                    except ProcessLookupError:
                        pass
            errors.seek(0)
            return subprocess.CompletedProcess(
                arguments, child.returncode, stderr=errors.read(4096)
            )
        finally:
            signal.signal(signal.SIGTERM, old_handler)
            if child.poll() is None:
                child.kill()
                try:
                    os.kill(child.pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            child.wait()
