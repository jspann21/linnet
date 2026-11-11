"""Cancellable, validated source-video conversion using the IPVF encoder."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import traceback
from collections import deque
from contextlib import contextmanager

from .ipvf_validate import inspect_file

NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


@contextmanager
def _scratch_directory(parent, progress):
    """Remove only this conversion's scratch and identify any cleanup failure."""
    parent = Path(parent).resolve()
    directory = Path(tempfile.mkdtemp(prefix="ipvf-encode-", dir=parent))
    failure = None
    try:
        yield directory
    except BaseException as error:
        failure = error
        raise
    finally:
        try:
            # Do not follow a directory replaced with a link outside our cache.
            if directory.resolve().parent != parent or directory.is_symlink():
                raise OSError("Temporary directory changed location")
            shutil.rmtree(directory)
        except OSError as cleanup_error:
            message = (f"Could not remove video temporary files: {directory}. "
                       f"Cleanup error: {cleanup_error}")
            try:
                progress(message)
            except Exception:
                pass
            error = RuntimeError(f"{failure}. {message}" if failure else message)
            error.cleanup_paths = [str(directory)]
            raise error from (failure or cleanup_error)
        else:
            # Reporting must not turn successful cleanup into a conversion failure.
            try:
                progress(f"Removed video temporary files: {directory}")
            except Exception:
                pass


def _duration_text(seconds):
    seconds = max(0, math.ceil(seconds))
    if seconds >= 3600:
        return f"{seconds // 3600}h {(seconds % 3600) // 60}m"
    if seconds >= 60:
        return f"{seconds // 60}m {seconds % 60}s"
    return f"{seconds}s"


class _StageProgress:
    def __init__(self, stage, name, progress, detail):
        self.stage, self.name = stage, name
        self.progress, self.detail = progress, detail
        self.started = time.monotonic()
        self.samples = deque()
        self.frames, self.total = 0, None

    def emit(self, frames=None, total=None, *, complete=False):
        now = time.monotonic()
        elapsed = now - self.started
        if frames is not None:
            self.frames = frames
        if total is not None:
            self.total = total
        self.samples.append((now, self.frames))
        while len(self.samples) > 2 and now - self.samples[1][0] > 15:
            self.samples.popleft()
        rate = self.frames / elapsed if elapsed > 0 else 0
        recent_time = now - self.samples[0][0]
        if recent_time >= 2:
            recent = (self.frames - self.samples[0][1]) / recent_time
            rate = 0.5 * rate + 0.5 * recent
        percent = (min(99, 100 * self.frames / self.total)
                   if self.total else None)
        eta = ((self.total - self.frames) / rate
               if self.total and self.frames < self.total and elapsed >= 3 and rate > 0
               else None)
        label = "Converting" if self.stage == "convert" else "Checking IPVF"
        if complete:
            percent, eta = 100, 0
            status = "100% · complete"
        else:
            status = f"{percent:.0f}%" if percent is not None else f"{self.frames} frames"
            if eta is not None:
                status += f" · about {_duration_text(eta)} left in this stage"
            elif self.total and self.frames >= self.total:
                status += (" · finishing checks" if self.stage == "verify"
                           else " · duration estimate reached; finishing conversion")
            else:
                status += " · estimating time remaining"
        message = f"{label} {self.name}: {status} · {_duration_text(elapsed)} elapsed"
        if self.total:
            message += f" · {self.frames:,}/{self.total:,} frames"
        if rate > 0:
            message += f" · {rate:.1f} fps"
        self.progress(message)
        if self.detail:
            self.detail({"stage": self.stage, "name": self.name, "percent": percent, "message": message,
                         "frames": self.frames, "total_frames": self.total,
                         "eta_seconds": eta, "elapsed_seconds": elapsed,
                         "frames_per_second": rate})


def _stop(process):
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=subprocess.CREATE_NO_WINDOW, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass  # The worker can exit between poll() and killpg().
    if process.poll() is None:
        process.kill()
    process.wait()


def encode_ipvf(source, destination, *, title=None, cancelled=None,
                progress=None, progress_detail=None, ffmpeg=None, ffprobe=None,
                fps=24, color_depth="rgb565", width=220, height=176,
                max_record_sectors=192):
    """Create target-native IPVF with 44.1-kHz adaptive IMA audio.

    Callbacks are ``cancelled() -> bool``, ``progress(message)``, and optional
    ``progress_detail(dict)`` (stage, percent, message, frames, total_frames,
    eta_seconds). Percent and ETA can be None; ETA covers only the named stage. The
    destination is replaced atomically only after complete strict validation.
    Cancellation raises media_manager.Cancelled (also a RuntimeError); conversion failures raise ValueError.
    FFmpeg and FFprobe are required; liblz4 is optional (built-in fallback).
    Long conversions may spawn validation workers. Script callers must use an
    if __name__ == "__main__" guard; frozen entry points must call
    multiprocessing.freeze_support() before dispatch.
    """
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if type(fps) is not int or not 4 <= fps <= 30:
        raise ValueError("IPVF media profile fps must be an integer from 4 through 30")
    if color_depth not in ("rgb565", "gray4"):
        raise ValueError("IPVF media profile color depth is unsupported")
    if (type(width) is not int or type(height) is not int or
            not 1 <= width <= 1024 or not 1 <= height <= 1024 or
            width*height > 131072 or width*height%2 or
            type(max_record_sectors) is not int or not 8 <= max_record_sectors <= 1024):
        raise ValueError("IPVF media profile geometry is unsupported")
    cancelled = cancelled or (lambda: False)
    from .media_manager import Cancelled
    progress = progress or (lambda message: None)
    if cancelled():
        raise Cancelled("Update cancelled")
    if not source.is_file():
        raise ValueError(f"Video source does not exist: {source}")
    if source == destination:
        raise ValueError("IPVF output must differ from the source video")
    executable = str(ffmpeg or shutil.which("ffmpeg") or "")
    probe = str(ffprobe or "")
    if not probe and executable:
        sibling = Path(executable).with_name("ffprobe" + Path(executable).suffix)
        probe = str(sibling) if sibling.is_file() else (shutil.which("ffprobe") or "")
    if not executable or not probe:
        raise ValueError("FFmpeg and FFprobe are required to convert videos to IPVF")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with _scratch_directory(destination.parent, progress) as temporary:
        directory = Path(temporary)
        encoded, log_path = directory / "video.ipvf", directory / "encode.log"
        request = directory / "request.json"
        request.write_text(json.dumps({"source": str(source), "output": str(encoded),
                                      "ffmpeg": executable, "ffprobe": probe,
                                      "title": title, "fps": fps,
                                      "color_depth": color_depth,
                                      "width": width, "height": height,
                                      "max_record_sectors": max_record_sectors}), encoding="utf-8")
        conversion = _StageProgress("convert", source.name, progress, progress_detail)
        conversion.emit()
        options = ({"creationflags": subprocess.CREATE_NO_WINDOW}
                   if os.name == "nt" else {"start_new_session": True})
        command = ([sys.executable, "--encode-ipvf", str(request)]
                   if getattr(sys, "frozen", False) else
                   [sys.executable, "-X", "utf8", "-u", "-m", "tools.content.ipvf_encode", str(request)])
        with log_path.open("wb") as log, log_path.open("rb") as updates:
            process = subprocess.Popen(command, cwd=Path(__file__).resolve().parents[2],
                                       stdout=log, stderr=log, **options)
            try:
                pending = b""
                last_refresh = time.monotonic()
                while True:
                    if cancelled():
                        raise Cancelled("Update cancelled")
                    pending += updates.read(65536)
                    lines = pending.split(b"\n")
                    pending = lines.pop()[-4096:]
                    for line in lines:
                        if line.startswith(b"IPVF_PROGRESS:"):
                            conversion.emit(int(line.partition(b":")[2].strip()))
                            last_refresh = time.monotonic()
                        elif line.startswith(b"IPVF_METADATA:"):
                            metadata = json.loads(line.partition(b":")[2])
                            conversion.emit(total=metadata.get("total_frames"))
                            last_refresh = time.monotonic()
                    if process.poll() is not None:
                        # Drain every remaining line, including short conversions.
                        tail = pending + updates.read()
                        for line in tail.splitlines():
                            if line.startswith(b"IPVF_PROGRESS:"):
                                conversion.emit(int(line.partition(b":")[2].strip()))
                            elif line.startswith(b"IPVF_METADATA:"):
                                conversion.emit(total=json.loads(line.partition(b":")[2]).get("total_frames"))
                        break
                    if time.monotonic() - last_refresh >= 5:
                        conversion.emit()
                        last_refresh = time.monotonic()
                    try:
                        process.wait(timeout=0.25)
                    except subprocess.TimeoutExpired:
                        pass
            finally:
                _stop(process)
        if cancelled():
            raise Cancelled("Update cancelled")
        if process.returncode:
            with log_path.open("rb") as log:
                log.seek(max(0, log_path.stat().st_size - 2000))
                detail = log.read().decode("utf-8", "replace").strip()
            raise ValueError("IPVF conversion failed: " + detail)
        conversion.emit(complete=True)
        verification = _StageProgress("verify", source.name, progress, progress_detail)
        verification.emit()
        last_update = time.monotonic()
        def checked(frame, count, pixels, audio):
            nonlocal last_update
            now = time.monotonic()
            if frame == 0 or frame + 1 == count or now - last_update >= 1:
                verification.emit(frame + 1, count)
                last_update = now
        result = inspect_file(encoded, cancelled=cancelled, on_frame=checked,
                              expected_width=width, expected_height=height,
                              max_record_sectors=max_record_sectors,
                              worker_limit=4 if (os.cpu_count() or 1) >= 8 else 1)
        if cancelled():
            raise Cancelled("Update cancelled")
        os.replace(encoded, destination)
        verification.emit(result["frames"], result["frames"], complete=True)
        result["path"] = str(destination)
        return result


def _worker(request):
    from . import ipvf_encoder as creator
    data = json.loads(Path(request).read_text(encoding="utf-8"))
    source, output = Path(data["source"]), Path(data["output"])
    source_name = source.stem
    probe = subprocess.run([data["ffprobe"], "-v", "error", "-show_entries",
                            "format=duration:stream=codec_type,duration:"
                            "format_tags=title,artist,album:stream_tags=title,artist,album",
                            "-of", "json", str(source)],
                           capture_output=True, text=True, encoding="utf-8", check=True,
                           creationflags=NO_WINDOW)
    information = json.loads(probe.stdout)
    def finite_duration(value):
        try:
            value = float(value)
            return value if math.isfinite(value) and value > 0 else None
        except (TypeError, ValueError):
            return None
    video = next((stream for stream in information.get("streams", [])
                  if stream.get("codec_type") == "video"), {})
    format_duration = finite_duration(information.get("format", {}).get("duration"))
    duration = finite_duration(video.get("duration")) or format_duration
    if duration is None:
        raise ValueError("Video must have a known finite duration")
    fps = data.get("fps", 24)
    color_depth = data.get("color_depth", "rgb565")
    if type(fps) is not int or not 4 <= fps <= 30 or color_depth not in ("rgb565", "gray4"):
        raise ValueError("Invalid IPVF media profile")
    creator.configure_geometry(data.get("width", 220), data.get("height", 176),
                               data.get("max_record_sectors", 192))
    print("IPVF_METADATA:" + json.dumps({"total_frames": math.ceil(duration * fps)}), flush=True)
    # Both decoded streams are piped; only the bounded encoded file is staged.
    needed = min(
        creator.DEVICE_FILE_SIZE_MAX,
        math.ceil(duration * fps * creator.MAX_RECORD_SECTORS * 512)) + 32 * 1024**2
    if shutil.disk_usage(output.parent).free < needed:
        raise ValueError(f"IPVF conversion needs about {needed / 1024**3:.1f} GB of free cache space")
    metadata = creator.metadata_from_probe(information)
    metadata["title"] = data["title"] or metadata.get("title") or source_name
    # Format metadata has byte limits; truncate on UTF-8 character boundaries.
    metadata = {key: value.encode("utf-8")[:120].decode("utf-8", "ignore")
                for key, value in metadata.items() if value}
    frames, cpus = math.ceil(duration * fps), os.cpu_count() or 1
    workers = 8 if frames >= 720 and cpus >= 12 else 4
    if frames >= 4320 and cpus >= 20:
        from .ipvf_host import available
        if available():
            workers = 12
    creator.encode(source, output, fps, fps, data["ffmpeg"], video_mode="realtime",
                   worker_limit=workers,
                   color_depth=color_depth,
                   metadata=metadata,
                   source_has_audio=any(stream.get("codec_type") == "audio"
                                        for stream in information.get("streams", [])))
    with output.open("rb") as encoded:
        header = encoded.read(20)
    print(f"IPVF_PROGRESS:{struct.unpack_from('<I', header, 16)[0]}", flush=True)


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    if len(arguments) != 1:
        return 2
    try:
        _worker(arguments[0])
    except Exception:
        traceback.print_exc()
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
