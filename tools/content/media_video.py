"""Video selection, compatibility checks, preparation, and output receipts."""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
import threading
import time
import uuid
from .guard import guard_volume as _volume_guard, guarded_path as _plain, hash_file as _hash
from .host_state import (
    cache_data_directory,
    cache_root,
    cache_volume_scope,
    file_stamp as _receipt_stamp,
    save_json as _save,
)
from .inventory import file_records as _file_records, source_folders
from .media_imports import _planning_records, _same_file_bytes
from .media_options import (
    Cancelled,
    _RECENT_OUTPUT_NS,
    native_video_extensions,
    target_args,
    video_source_extensions,
)
from .media_outputs import _outputs_match
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def _video_is_native(source, identity, *, ffprobe=None, cancelled=None, details=None):
    """Check the actual container and codecs before treating a suffix as native."""
    if source.suffix.lower() not in native_video_extensions(identity):
        return False
    profile = identity['media_profile']
    if profile.get('native_video', {}).get('normalize', False):
        return False
    try:
        if profile['video_backend'] in ('bcm-mp4', 'native-mp4'):
            from .native_video import inspect_file
            inspected = inspect_file(source, profile=profile, ffprobe=ffprobe, cancelled=cancelled)
            if details is not None:
                details.update(inspected)
        else:
            from .ipvf_validate import inspect_file
            inspect_file(source, cancelled=cancelled, expected_width=profile['video_width'],
                         expected_height=profile['video_height'],
                         max_record_sectors=profile['max_read_sectors'])
        return True
    except (OSError, ValueError):
        return False


def _native_check_reusable(job, source, profile):
    proof = job.get('_native_check')
    if (not isinstance(proof, tuple) or len(proof) != 3 or
            not isinstance(proof[2], tuple) or len(proof[2]) != 5 or
            not all(type(value) is int for value in proof[2]) or
            proof[0] != str(source) or proof[1] != json.dumps(profile, sort_keys=True) or
            (os.name == 'nt' and proof[2][3] > time.time_ns() - _RECENT_OUTPUT_NS)):
        return False
    try:
        return (source.resolve(strict=True) == source and
                _receipt_stamp(source.stat()) == proof[2])
    except OSError:
        return False


def _video_duration(source, ffprobe, *, cancelled=None):
    def check():
        if cancelled and cancelled():
            raise Cancelled('Stopped while checking video duration.')

    check()
    probe = ffprobe or shutil.which('ffprobe')
    if not probe:
        raise ValueError('FFprobe is required to estimate video conversion space.')
    command = [str(probe), '-v', 'error', '-show_entries',
               'stream=duration:format=duration', '-of', 'json', str(source)]
    try:
        with tempfile.TemporaryFile() as output:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output,
                                       stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            started = time.monotonic()
            try:
                while process.poll() is None:
                    check()
                    remaining = 60 - (time.monotonic() - started)
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, 60)
                    try:
                        process.wait(timeout=min(.1, remaining))
                    except subprocess.TimeoutExpired:
                        pass
                check()
                if process.returncode:
                    raise subprocess.CalledProcessError(process.returncode, command)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            output.seek(0)
            data = json.loads(output.read(1024 * 1024))
        check()
        from .native_video import _probe_duration
        duration = _probe_duration(data)
        if duration:
            return duration
    except (OSError, ValueError, subprocess.SubprocessError, TypeError, AttributeError):
        pass
    check()
    raise ValueError(f'Could not determine video duration for space planning: {source.name}')


def video_jobs(root, pending=(), selection=None, skipped=(), *, identity, snapshots=None,
               ffprobe=None, cancelled=None, replacements=None):
    """Resolve selected conversions and this device's supported on-disk videos."""
    root = Path(root).resolve(strict=True)
    folder = root / "Videos"

    def check():
        if cancelled and cancelled():
            raise Cancelled('Stopped while checking queued videos.')

    check()
    allowed = video_source_extensions(identity)
    native_extensions = native_video_extensions(identity)
    scan_allowed = native_extensions if selection is not None else allowed
    excluded = {Path(value).as_posix().casefold() for value in skipped}
    generated = set()
    try:
        saved = json.loads((cache_root(identity) / 'state.json').read_text(encoding='utf-8'))
        for name in saved.get('video_generated', []):
            path = Path(name)
            if (path.parts and path.parts[0].casefold() == 'videos' and
                    not path.is_absolute() and '..' not in path.parts):
                generated.add(str(root / path).casefold())
        for relative, item in saved.get('video_files', {}).items():
            for output in item.get('outputs', []):
                name = output[0]
                path = Path(name)
                if (path.parts and path.parts[0].casefold() == 'videos' and
                        not path.is_absolute() and '..' not in path.parts and
                        name.casefold() != ('Videos/' + relative).casefold()):
                    generated.add(str(root / path).casefold())
    except (OSError, ValueError, UnicodeError, TypeError, AttributeError, IndexError):
        pass
    available = [str(path) for source in source_folders(root, "Videos")
                 for path, _ in _file_records(source, allowed=scan_allowed, checkpoint=check)
                 if str(path).casefold() not in generated and
                 (not excluded or path.relative_to(root).as_posix().casefold() not in excluded)]
    check()
    from .video_catalog import read_file
    catalog = root / ".LINNET" / "VIDEOS" / "INDEX.VID"
    _plain(catalog, root)
    entries = read_file(catalog)[1] if catalog.exists() else []
    published = {}
    for catalog_key, name, title in entries:
        path = root / name.lstrip('/')
        _plain(path, root)
        published[path.resolve()] = title
    native = [name for name in available if Path(name).suffix.lower() in native_extensions]
    chosen = available if selection is None else selection
    chosen = list(dict.fromkeys([*native, *chosen]))
    source_roots = [path.resolve() for path in source_folders(root, 'Videos')]
    jobs = {}
    queued_relatives = set()

    def include(source, relative):
        check()
        _plain(source, Path(source.anchor))
        source = source.resolve(strict=True)
        if not source.is_file() or source.suffix.lower() not in allowed:
            raise ValueError(f"Unsupported video: {source}")
        key = relative.casefold()
        if key in jobs and jobs[key]["source"] != str(source):
            raise ValueError(f"Two videos have the same library name: {relative}. Rename one first.")
        info = source.stat()
        jobs[key] = dict(source=str(source), relative=relative, bytes=info.st_size, mtime_ns=info.st_mtime_ns,
                         catalog_title=published.get(source))

    for name in chosen:
        check()
        source = Path(name).absolute()
        _plain(source, root)
        source = source.resolve(strict=True)
        if not any(source.is_relative_to(folder) for folder in source_roots):
            raise ValueError(f"Selected video is outside this iPod's video sources: {source}")
        include(source, source.relative_to(folder).as_posix() if source.is_relative_to(folder)
                else source.relative_to(root).as_posix())
    for kind, name in pending:
        check()
        if kind != "Videos":
            continue
        source = Path(name).absolute()
        _plain(source, Path(source.anchor))
        source = source.resolve(strict=True)
        if source.is_relative_to(root):
            raise ValueError("Use Choose videos and select this file under Videos on this iPod.")
        is_folder = source.is_dir()
        records = _planning_records(source, snapshots, checkpoint=check)
        for path, info in (records if not is_folder else
                        ((path, info) for path, info in records if path.suffix.lower() in allowed)):
            check()
            relative = (Path(source.name) / path.relative_to(source)).as_posix() if is_folder else source.name
            if relative.casefold() in queued_relatives:
                raise ValueError(f"Two queued videos have the same library name: {relative}.")
            queued_relatives.add(relative.casefold())
            existing = folder / relative
            _plain(existing, root)
            if existing.exists():
                if not existing.is_file():
                    raise ValueError(f"A different video already exists: {relative}. Rename the new file first.")
                old = existing.stat()
                if old.st_size != info.st_size or not _same_file_bytes(path, existing, info.st_size, checkpoint=check):
                    if replacements is None:
                        raise ValueError(f"A different video already exists: {relative}. Review its replacement before preparing.")
                    replacements.append(dict(action='replace', kind='Videos', source=str(path),
                        destination=str(existing), old_bytes=old.st_size,
                        old_mtime_ns=old.st_mtime_ns, new_bytes=info.st_size,
                        new_mtime_ns=info.st_mtime_ns,
                        old_sha256=_hash(existing, progress=lambda _current, _total: check()),
                        new_sha256=_hash(path, progress=lambda _current, _total: check())))
                    jobs.pop(relative.casefold(), None)
                else:
                    path = existing
            include(path, relative)
    if len(jobs) > 256:
        raise ValueError("Choose at most 256 videos per update.")
    check()
    if selection is not None and not selection:
        if (identity['media_profile']['video_backend'] in ('bcm-mp4', 'native-mp4')
                and not (ffprobe or shutil.which('ffprobe')) and jobs):
            raise ValueError('FFprobe is required to check native video compatibility.')
        # No conversions includes only videos whose content is playable as-is.
        # A queued source must be called out instead of silently discarded.
        def check_native(job, stop=None):
            source = Path(job['source'])
            proof_before = None
            if source.suffix.lower() in native_extensions:
                try:
                    proof_before = _receipt_stamp(source.stat())
                except OSError:
                    pass
            details = {}
            should_stop = (lambda: stop.is_set() or bool(cancelled and cancelled())) if stop else cancelled
            native = _video_is_native(source, identity, ffprobe=ffprobe, cancelled=should_stop,
                                      details=details)
            proof_after = None
            if native and proof_before is not None:
                try:
                    proof_after = _receipt_stamp(source.stat())
                except OSError:
                    pass
            reusable = (proof_before is not None and proof_before == proof_after and
                        (os.name != 'nt' or
                         proof_after[3] <= time.time_ns() - _RECENT_OUTPUT_NS))
            return native, details, proof_after if reusable else None

        def collect(items, futures=None):
            ready = []
            for job in items:
                source = Path(job['source'])
                native, details, proof = (
                    futures[id(job)].result() if futures and id(job) in futures else check_native(job))
                if native:
                    if proof is not None:
                        job['_native_check'] = (str(source),
                            json.dumps(identity['media_profile'], sort_keys=True), proof)
                        if details:
                            job['_native_details'] = details
                    ready.append(job)
                elif not source.is_relative_to(root) or job['relative'].casefold() in queued_relatives:
                    raise ValueError(f"{source.name} needs conversion or is not a playable native video. Choose a conversion mode or remove it from the queue.")
            return ready

        all_jobs = list(jobs.values())
        host_checks = [job for job in all_jobs
                       if (identity['media_profile']['video_backend'] in ('bcm-mp4', 'native-mp4') and
                           Path(job['source']).suffix.lower() in native_extensions and
                           not Path(job['source']).is_relative_to(root))]
        host_ids = {id(job) for job in host_checks}
        first_host = next((index for index, job in enumerate(all_jobs) if id(job) in host_ids), len(all_jobs))
        # The device's own videos precede queued host files. Complete those
        # reads before starting several FFprobe processes on host sources.
        if len(host_checks) > 1 and all(id(job) in host_ids for job in all_jobs[first_host:]):
            ready = collect(all_jobs[:first_host])
            stop = threading.Event()
            with ThreadPoolExecutor(max_workers=min(4, len(host_checks))) as pool:
                try:
                    futures = {id(job): pool.submit(check_native, job, stop) for job in host_checks}
                    return ready + collect(all_jobs[first_host:], futures)
                finally:
                    stop.set()
        return collect(all_jobs)
    return list(jobs.values())


def _video_inventory(jobs):
    rows = []
    for job in jobs:
        info = Path(job["source"]).stat()
        rows.append((job["relative"], job["source"], *_receipt_stamp(info)))
    return dict(count=len(rows), files=[job["source"] for job in jobs],
                fingerprint=hashlib.sha256(json.dumps(rows).encode()).hexdigest())


def _video_outputs_from_publication(root, identity, output_paths, proofs):
    """Reuse active-package readbacks, hashing outputs without publication proofs."""
    outputs = []
    for name in output_paths:
        _volume_guard(identity)
        path = root / name
        _plain(path, root)
        proof = proofs.get(name)
        if proof is None:
            before = path.stat()
            digest = _hash(path)
            if _receipt_stamp(path.stat()) != _receipt_stamp(before):
                raise ValueError(f"video output changed during final verification: {path}")
        else:
            if (not isinstance(proof, dict) or not isinstance(proof.get('path'), str) or
                    Path(proof['path']) != path or
                    not isinstance(proof.get('sha256'), str) or
                    re.fullmatch('[0-9a-f]{64}', proof['sha256']) is None or
                    not isinstance(proof.get('stamp'), tuple) or len(proof['stamp']) != 5 or
                    not all(type(value) is int for value in proof['stamp'])):
                raise ValueError(f"invalid video publication proof: {name}")
            info = path.stat()
            if not stat.S_ISREG(info.st_mode) or _receipt_stamp(info) != proof['stamp']:
                raise ValueError(f"video output changed after publication: {path}")
            digest = proof['sha256']
        outputs.append((name, digest))
    return outputs


def _prepare_videos(root, cache, identity, media_profile, state, state_path, videos, item, *,
                    ffmpeg, ffprobe, force, cancelled, checkpoint, progress, emit_stage, unit,
                    record, report, verified_outputs, owned_temporaries, progress_detail,
                    mark_publication, no_conversions=False):
    cache = cache_data_directory(cache)
    kind = "Videos"
    published = []
    video_proofs = {}
    from .ipvf_encode import encode_ipvf
    from .ipvf_validate import inspect_file
    from .publish_video import publish_video
    from .video_sources import video_title, catalog_contains
    output_paths = [".LINNET/VIDEOS/INDEX.VID"]
    video_state = state.get("video_files", {})
    if not isinstance(video_state, dict):
        video_state = {}
    state["video_files"] = video_state
    volume_scope = cache_volume_scope(root)
    failed = 0
    retail_titles = None
    catalog_snapshot = {}
    def remember_generated(source, outputs):
        if media_profile['video_backend'] != 'bcm-mp4':
            return
        original = source.relative_to(root).as_posix().casefold() if source.is_relative_to(root) else None
        generated = set(state.get('video_generated', []))
        generated.update(name for name, digest in outputs if name.casefold() != original)
        state['video_generated'] = sorted(generated)
    for index, job in enumerate(videos):
        emit_stage('Preparing Videos', unit, index / len(videos))
        checkpoint()
        source = Path(job["source"])
        relative = job["relative"]
        source_key = (f'{volume_scope}:{source.relative_to(root).as_posix()}'
                      if source.is_relative_to(root) else str(source))
        file_publication_started = False
        try:
            key = hashlib.sha256(relative.encode()).hexdigest()[:8].upper()
            source_info = source.stat()
            source_stamp = _receipt_stamp(source_info)
            label = job.get('catalog_title')
            if not label and source.is_relative_to(root):
                if retail_titles is None:
                    from .retail_media import available_tracks
                    retail_titles = available_tracks(root)
                label = retail_titles.get(str(source).casefold(), {}).get('title')
            label = video_title(label or relative)
            if media_profile["video_backend"] in ("bcm-mp4", "native-mp4"):
                from .native_video import inspect_file as inspect_native, prepare as prepare_native
                from .publish_video import device_video_path, index_video
                native_version = 5 if media_profile["video_backend"] == "native-mp4" else 4
                normalize = media_profile['native_video'].get('normalize', False)
                native_mode = ''
                if normalize:
                    native_mode = ':normalized'
                elif media_profile['video_backend'] == 'bcm-mp4':
                    native_mode = ':chunked' if not no_conversions and (ffmpeg or shutil.which('ffmpeg')) else ':copy'
                fingerprint = hashlib.sha256(f"native-mp4-v{native_version}{native_mode}:{json.dumps(media_profile, sort_keys=True)}:{source_key}:{source_stamp}".encode()).hexdigest()
                cached = video_state.get(relative, {})
                if not isinstance(cached, dict):
                    cached = {}
                outputs = cached.get('outputs', [])
                remember_generated(source, outputs)
                cached_output_ready = False
                if (not force and cached.get("signature") == fingerprint and len(outputs) == 1 and
                        _outputs_match(root, outputs, verified=verified_outputs)):
                    try:
                        inspect_native(root / outputs[0][0], profile=media_profile,
                                       ffprobe=ffprobe, cancelled=cancelled)
                        cached_output_ready = True
                    except (OSError, ValueError):
                        pass
                if cached_output_ready:
                    if _receipt_stamp(source.stat()) != source_stamp:
                        raise ValueError("Video changed while checking its prepared copy; scan again")
                    if catalog_contains(root, relative, label, path=False,
                                        snapshot=catalog_snapshot):
                        output_paths.extend(name for name, digest in outputs)
                        report["steps"].append(dict(kind=kind, file=relative, status="unchanged"))
                        continue
                    if len(outputs) == 1:
                        published_path = root / outputs[0][0]
                        if catalog_contains(root, relative, None,
                                            path=device_video_path(published_path, root),
                                            snapshot=catalog_snapshot):
                            emit_stage('Publishing Videos', unit,
                                       (index + 0.5) / len(videos), can_stop=False)
                            mark_publication()
                            file_publication_started = True
                            result = index_video(published_path, title=label,
                                                 library_key=relative, ffprobe=ffprobe,
                                                 **target_args(identity))
                            catalog_snapshot.clear()
                            output_paths.append(outputs[0][0])
                            report["steps"].append(dict(kind=kind, file=relative,
                                                        status="catalog-updated", publication=result))
                            record()
                            continue
                if normalize and no_conversions:
                    raise ValueError('This iPod needs prepared video. Choose a video conversion mode.')
                compatible = False
                # Native preparation checks conversion-enabled host sources.
                # No-conversion planning supplies a short-lived proof for host
                # sources; the copied package is still validated below. Device
                # sources need this check to decide on indexing in place.
                if (source.suffix.lower() in native_video_extensions(identity) and
                        (source.is_relative_to(root) or
                         (no_conversions and not _native_check_reusable(job, source, media_profile)))):
                    try:
                        native_info = inspect_native(source, ffprobe=ffprobe, profile=media_profile, cancelled=cancelled)
                        compatible = not normalize and (no_conversions or
                                      (native_info['fast_start'] and media_profile['video_backend'] != 'bcm-mp4') or
                                      not (ffmpeg or shutil.which('ffmpeg')))
                    except ValueError:
                        pass
                progress(f"Preparing native video {index+1}/{item['count']}: {relative}")
                if compatible and source.is_relative_to(root):
                    emit_stage('Publishing Videos', unit,
                               (index + 0.5) / len(videos), can_stop=False)
                    mark_publication()
                    file_publication_started = True
                    result = index_video(source, title=label, library_key=relative, ffprobe=ffprobe, **target_args(identity))
                    outputs = [(source.relative_to(root).as_posix(), _hash(source))]
                    output_paths.append(source.relative_to(root).as_posix())
                else:
                    package = cache / "videos" / (key + "-" + fingerprint[:16] + ".mp4")
                    reusable = package.exists() and not force and not no_conversions
                    if reusable:
                        try:
                            inspect_native(package, ffprobe=ffprobe, profile=media_profile,
                                           cancelled=cancelled)
                        except (OSError, ValueError):
                            reusable = False
                    if not reusable:
                        if no_conversions:
                            checkpoint()
                            package.parent.mkdir(parents=True, exist_ok=True)
                            temporary = package.with_name(package.stem + '.' + uuid.uuid4().hex + '.mp4')
                            owned_temporaries.append((temporary, cache))
                            with source.open('rb') as reader, temporary.open('xb') as writer:
                                while chunk := reader.read(1024 * 1024):
                                    checkpoint()
                                    writer.write(chunk)
                                writer.flush()
                                os.fsync(writer.fileno())
                            inspect_native(temporary, ffprobe=ffprobe, profile=media_profile,
                                           cancelled=cancelled)
                            os.replace(temporary, package)
                            owned_temporaries.remove((temporary, cache))
                        else:
                            prepare_native(source, package, profile=media_profile, ffmpeg=ffmpeg,
                                           ffprobe=ffprobe, progress=progress, cancelled=cancelled,
                                           progress_detail=(lambda event: progress_detail(dict(event,
                                               video_index=index+1, video_total=item['count'])))
                                               if progress_detail else None)
                    checkpoint()
                    after = source.stat()
                    if _receipt_stamp(after) != source_stamp:
                        raise ValueError("Video changed during preparation; scan again")
                    emit_stage('Publishing Videos', unit,
                               (index + 0.5) / len(videos), can_stop=False)
                    mark_publication()
                    file_publication_started = True
                    readback_proof = {}
                    result = publish_video(package, title=label, library_key=relative,
                                           ffprobe=ffprobe,
                                           source_to_preserve=source if source.is_relative_to(root) else None,
                                           readback_proof=readback_proof,
                                           **target_args(identity))
                    output_name = Path(result['package']).relative_to(root).as_posix()
                    if readback_proof:
                        video_proofs[output_name] = readback_proof
                    outputs = [(output_name, result["candidate_sha256"])]
                    output_paths.append(output_name)
                after = source.stat()
                if _receipt_stamp(after) != source_stamp:
                    raise ValueError("Video changed during publication; scan again")
                published.append(result)
                catalog_snapshot.clear()
                remember_generated(source, outputs)
                video_state[relative] = dict(signature=fingerprint, outputs=outputs, source_stamp=source_stamp)
                _save(state_path, state)
                report["steps"].append(dict(kind=kind, file=relative, status="published", publication=result))
                record()
                continue
            profile_key = (f"{media_profile['video_width']}x{media_profile['video_height']}-"
                           f"{media_profile['video_fps']}-{media_profile['video_color']}-realtime-native-key1s")
            fingerprint = hashlib.sha256(f"ipvf-{profile_key}:{source_key}:{source_stamp}".encode()).hexdigest()
            cached = video_state.get(relative, {})
            if not isinstance(cached, dict):
                cached = {}
            cached_outputs = cached.get('outputs')
            first_output = cached_outputs[0] if isinstance(cached_outputs, list) and cached_outputs else None
            output_name = (first_output[0] if isinstance(first_output, (list, tuple))
                           and len(first_output) == 2 and isinstance(first_output[0], str) else "")
            native_ipvf = source.suffix.lower() in native_video_extensions(identity)
            index_in_place = native_ipvf and source.is_relative_to(root)
            if (not force and cached.get("signature") == fingerprint
                    and catalog_contains(root, relative, label, path=False,
                                         snapshot=catalog_snapshot)
                    and ((index_in_place and output_name.casefold() ==
                          source.relative_to(root).as_posix().casefold()) or
                         output_name.casefold().startswith("videos/ipvf/"))
                    and _outputs_match(root, cached.get("outputs", []), verified=verified_outputs)):
                if _receipt_stamp(source.stat()) != source_stamp:
                    raise ValueError("Video changed while checking its prepared copy; scan again")
                if not index_in_place:
                    output_paths.append(output_name)
                progress(f"Video {index+1}/{item['count']} already ready: {relative}")
                report["steps"].append(dict(kind=kind, file=relative, status="unchanged"))
                continue
            package = cache / "videos" / (key + "-" + fingerprint[:16] + ".ipvf")
            progress(f"Preparing video {index+1}/{item['count']}: {relative}")
            if index_in_place:
                from .publish_video import index_video
                emit_stage('Publishing Videos', unit,
                           (index + 0.5) / len(videos), can_stop=False)
                mark_publication()
                file_publication_started = True
                result = index_video(source, title=label, library_key=relative, **target_args(identity))
                if _receipt_stamp(source.stat()) != source_stamp:
                    raise ValueError("Video changed during publication; scan again before updating")
                published.append(result)
                catalog_snapshot.clear()
                video_state[relative] = dict(signature=fingerprint,
                    outputs=[(source.relative_to(root).as_posix(), _hash(source))],
                    source_stamp=source_stamp)
                _save(state_path, state)
                report["steps"].append(dict(kind=kind, file=relative, status="published", publication=result))
                record()
                continue
            reusable = package.exists() and not force
            if reusable:
                try:
                    inspect_file(package,expected_width=media_profile["video_width"],
                                 expected_height=media_profile["video_height"],
                                 max_record_sectors=media_profile["max_read_sectors"])
                except (OSError, ValueError):
                    reusable = False
            if not reusable:
                if native_ipvf:
                    # No-conversion planning already validated this exact
                    # source. The copied temporary is checked again below.
                    if not (no_conversions and _native_check_reusable(job, source, media_profile)):
                        inspect_file(source,expected_width=media_profile["video_width"],
                                     expected_height=media_profile["video_height"],
                                     max_record_sectors=media_profile["max_read_sectors"])
                    checkpoint()
                    package.parent.mkdir(parents=True, exist_ok=True)
                    temporary = package.with_name(package.name + "." + uuid.uuid4().hex + ".tmp")
                    with source.open("rb") as reader, temporary.open("xb") as writer:
                        owned_temporaries.append((temporary, cache))
                        while chunk := reader.read(1024 * 1024):
                            checkpoint()
                            writer.write(chunk)
                        writer.flush()
                        os.fsync(writer.fileno())
                    inspect_file(temporary,expected_width=media_profile["video_width"],
                                 expected_height=media_profile["video_height"],
                                 max_record_sectors=media_profile["max_read_sectors"])
                    os.replace(temporary, package)
                else:
                    encode_ipvf(source, package, title=label,
                                ffmpeg=ffmpeg or shutil.which("ffmpeg"),
                                ffprobe=ffprobe or shutil.which("ffprobe"),
                                fps=media_profile["video_fps"],
                                color_depth=media_profile["video_color"],
                                width=media_profile["video_width"],
                                height=media_profile["video_height"],
                                max_record_sectors=media_profile["max_read_sectors"],
                                progress=progress, cancelled=cancelled,
                                progress_detail=(lambda event: progress_detail(dict(event,
                                    video_index=index+1, video_total=item['count']))) if progress_detail else None)
            checkpoint()
            after = source.stat()
            if _receipt_stamp(after) != source_stamp:
                raise ValueError("Video changed during preparation; scan again before updating")
            progress(f"Publishing video {index+1}/{item['count']}: {relative}")
            emit_stage('Publishing Videos', unit,
                       (index + 0.5) / len(videos), can_stop=False)
            mark_publication()
            file_publication_started = True
            readback_proof = {}
            result = publish_video(package, title=label, library_key=relative,
                                   source_to_preserve=source if source.is_relative_to(root) else None,
                                   readback_proof=readback_proof,
                                   **target_args(identity))
            catalog_snapshot.clear()
            output_name = Path(result['package']).relative_to(root).as_posix()
            if readback_proof:
                video_proofs[output_name] = readback_proof
            published.append(result)
            output_paths.append(output_name)
            after = source.stat()
            if _receipt_stamp(after) != source_stamp:
                raise ValueError("Video changed during publication; scan again before updating")
            video_state[relative] = dict(signature=fingerprint, source_stamp=source_stamp,
                outputs=[(output_name, result["candidate_sha256"])])
            _save(state_path, state)
            report["steps"].append(dict(kind=kind, file=relative, status="published", publication=result))
            record()
        except Cancelled:
            raise
        except Exception as error:
            catalog_snapshot.clear()
            checkpoint()
            failed += 1
            video_state.pop(relative, None)
            state.pop(kind, None)
            _save(state_path, state)
            failure = dict(kind=kind, file=relative, message=str(error))
            if file_publication_started:
                failure["status"] = "publication-uncertain"
            else:
                failure["source"] = str(source)
            report["errors"].append(failure)
            progress(f"Video could not finish, continuing with the next file: {relative}: {error}")
            record()
    return output_paths, published, failed, video_proofs
