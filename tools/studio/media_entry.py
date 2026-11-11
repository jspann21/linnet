"""Entry point for the downloadable Windows Media Manager and installer worker."""
import sys


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--media-read':
        from tools.studio.media_reader import main
        raise SystemExit(main(sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == '--encode-ipvf':
        from tools.content.ipvf_encode import main
        raise SystemExit(main(sys.argv[2:]))
    if len(sys.argv) > 1:
        from tools.studio.installer import main
        raise SystemExit(main())
    from tools.studio.media_app import launch
    launch()


if __name__ == "__main__":
    from tools.studio.media_app import _installer_guard, SetupInProgressError
    try:
        # Cover every packaged process, including elevated workers and child
        # processes dispatched directly by PyInstaller's freeze_support.
        with _installer_guard():
            from multiprocessing import freeze_support
            freeze_support()
            main()
    except SetupInProgressError as error:
        if len(sys.argv) == 1:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, str(error), 'Linnet', 0x40)
        elif sys.stderr is not None:
            print(str(error), file=sys.stderr)
        raise SystemExit(1)
    except Exception as error:
        if sys.platform == 'win32' and len(sys.argv) == 1 and sys.stderr is None:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, 'Linnet could not start.\n\n' + str(error), 'Linnet', 0x10)
            raise SystemExit(1)
        raise
