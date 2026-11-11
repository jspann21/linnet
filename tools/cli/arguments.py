"""Argument groups shared by CLI domains."""
from pathlib import Path


def _source_output_arguments(entry):
    entry.add_argument('--source', type=Path, required=True)
    entry.add_argument('--output', type=Path, required=True)


def _publication_arguments(entry, drive_help=None):
    entry.add_argument('--disk-number', type=int, required=True)
    entry.add_argument('--disk-serial', required=True)
    entry.add_argument('--volume-serial', required=True)
    entry.add_argument('--drive', help=drive_help)


def _publication_options(args):
    return dict(disk_number=args.disk_number, expected_disk_serial=args.disk_serial,
                expected_volume_serial=args.volume_serial, expected_drive=args.drive)


def _record_arguments(entry):
    entry.add_argument('--mount', type=Path, required=True)
    entry.add_argument('--disk-serial', required=True)
    entry.add_argument('--disk-bytes', type=int, required=True)
    entry.add_argument('--confirmed-build', required=True)
