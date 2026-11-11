"""Music, video, and photo CLI commands."""
from pathlib import Path

from tools.cli.arguments import _source_output_arguments, _publication_arguments, _publication_options
from tools.studio.targets import TARGETS, target_id


def register_media(commands):
    music=commands.add_parser('music').add_subparsers(dest='action',required=True)
    music_compile=music.add_parser('compile')
    _source_output_arguments(music_compile)
    music_compile.add_argument('--target', choices=sorted(TARGETS), help='Prepare artwork for this display')
    music_compile.add_argument('--ffprobe',help='Optional executable for compressed-source metadata')
    music_compile.add_argument('--device-root',type=Path,help='Index existing files on this mounted iPod without copying audio')
    music_compile.add_argument('--include-root',type=Path,action='append',help='Limit an existing-device import to this root (repeatable)')
    music_compile.add_argument('--cache-path',type=Path,help='Reuse an existing music metadata cache outside the iPod')
    music_compile.add_argument('--verification',choices=('metadata','full'),help='Existing music defaults to fast metadata checks; full reads complete audio files')
    music_compile.add_argument('--refresh-metadata',action='store_true',help='Ignore cached metadata for this import')
    music_check=music.add_parser('inspect')
    music_check.add_argument('database',type=Path)
    music_publish=music.add_parser('publish',help='Publish verified staging to an explicitly identified mounted USB iPod')
    music_publish.add_argument('--stage',type=Path,required=True)
    _publication_arguments(music_publish, drive_help='Optional expected Windows drive letter')
    video=commands.add_parser('video').add_subparsers(dest='action',required=True)
    video_compile=video.add_parser('compile')
    _source_output_arguments(video_compile)
    video_compile.add_argument('--target',default='photo4g-p98',type=target_id)
    video_compile.add_argument('--fps',type=int,choices=range(4,31),help='Defaults to the target video frame rate')
    video_compile.add_argument('--ffmpeg')
    video_compile.add_argument('--ffprobe')
    video_inspect=video.add_parser('inspect')
    video_inspect.add_argument('package',type=Path)
    video_inspect.add_argument('--target', type=target_id, help='Required for native MP4 validation')
    video_publish=video.add_parser('publish')
    video_publish.add_argument('--package',type=Path,required=True)
    _publication_arguments(video_publish)
    video_publish.add_argument('--library-key',help='Stable source-relative identity for a video in the browsable library')
    video_publish.add_argument('--title',help='Video library display title')
    photos=commands.add_parser('photos').add_subparsers(dest='action',required=True)
    photo_compile=photos.add_parser('compile')
    _source_output_arguments(photo_compile)
    photo_compile.add_argument('--target',default='photo4g-p98',type=target_id)
    photo_compile.add_argument('--max-dimension',type=int,default=640,
        help='Largest prepared zoom dimension (default: 640); originals are unchanged')
    photo_inspect=photos.add_parser('inspect')
    photo_inspect.add_argument('package',type=Path)
    photo_publish=photos.add_parser('publish')
    photo_publish.add_argument('--package',type=Path,required=True)
    photo_publish.add_argument('--grid',type=Path,help='Prepared native thumbnail grid paired with this package')
    _publication_arguments(photo_publish)


def run_media(args, emit):
    if args.command=='music':
        from tools.content.music import compile_music,validate_database
        if args.action=='publish':
            from tools.content.publish import publish_music
            emit(publish_music(args.stage, **_publication_options(args)))
        else:emit(compile_music(args.source,args.output,ffprobe=args.ffprobe,device_root=args.device_root,
                               verification=args.verification,bypass_cache=args.refresh_metadata,target=args.target,
                               include_roots=args.include_root,cache_path=args.cache_path) if args.action=='compile' else validate_database(args.database.read_bytes()))
    elif args.command=='video':
        if args.action=='compile':
            from tools.content.ipvf_encode import encode_ipvf
            spec=TARGETS[args.target]
            if not spec['capabilities'].get('video',True):
                raise ValueError('video is disabled for the selected target')
            if spec['media_video_backend'] in ('bcm-mp4', 'native-mp4'):
                from tools.content.native_video import prepare
                from tools.studio.targets import media_profile
                profile = media_profile(args.target)
                if args.output.suffix.lower() not in spec['media_video_extensions']:
                    raise ValueError('native video output must use ' + ', '.join(spec['media_video_extensions']))
                if args.fps:
                    if args.fps > profile['native_video']['max_fps']:
                        raise ValueError('frame rate exceeds the target limit')
                    profile['video_fps'] = args.fps
                emit(prepare(args.source, args.output, profile=profile,
                             ffmpeg=args.ffmpeg, ffprobe=args.ffprobe))
            else:
                if args.output.suffix.lower() not in ('.ipvf','.ipv'):
                    raise ValueError('video output must use .ipvf or .ipv')
                emit(encode_ipvf(args.source,args.output,fps=args.fps or spec['media_video_fps'],
                    width=spec['media_video_width'],height=spec['media_video_height'],
                    color_depth=spec['media_video_color'],
                    max_record_sectors=spec['storage_max_read_sectors'],
                    ffmpeg=args.ffmpeg,ffprobe=args.ffprobe))
        elif args.action=='inspect':
            if args.package.suffix.lower() in ('.mp4', '.m4v', '.mov'):
                from tools.content.native_video import inspect_file
                from tools.studio.targets import media_profile
                if not args.target:
                    raise ValueError('Specify --target for native MP4 validation')
                emit(inspect_file(args.package, profile=media_profile(args.target)))
            else:
                from tools.content.ipvf_validate import inspect_file
                emit(inspect_file(args.package))
        else:
            from tools.content.publish_video import publish_video
            emit(publish_video(args.package, **_publication_options(args),
                 library_key=args.library_key,title=args.title))
    elif args.command=='photos':
        from tools.content.photos import compile_photos,inspect as inspect_photos
        if args.action=='publish':
            from tools.content.publish_photos import publish_photos
            emit(publish_photos(args.package, grid=args.grid, **_publication_options(args)))
        else:
            if args.action=='compile':
                spec=TARGETS[args.target]
                emit(compile_photos(args.source,args.output,max_dimension=args.max_dimension,
                    viewport_width=spec['media_photo_width'],viewport_height=spec['media_photo_height'],target=args.target))
            else:emit(inspect_photos(args.package))
    return 0
