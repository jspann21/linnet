"""Bind a precompiled developer image to the connected iPod without a compiler."""
import re

from tools.inspect.image import inspect, pack

TEMPLATE_SERIAL = '0000000000000000'
SERIAL_BYTES = 33


def _serial_bytes(serial):
    if not isinstance(serial, str) or not re.fullmatch(r'[A-Za-z0-9-]{1,32}', serial):
        raise ValueError('Invalid developer firmware USB identity')
    return serial.encode('ascii').ljust(SERIAL_BYTES, b'\0')


def validate_binding(data, report, serial):
    image = inspect(data)
    binding = report.get('serial_binding')
    if (image['profile'] != 'dev' or report.get('image') != image or
            report.get('build_id') != image['build_id'] or report.get('device_serial') != serial or
            not isinstance(binding, dict) or set(binding) != {'format', 'offset', 'bytes'} or
            type(binding['format']) is not int or binding['format'] != 1 or
            type(binding['bytes']) is not int or binding['bytes'] != SERIAL_BYTES or
            type(binding['offset']) is not int or
            not 0 <= binding['offset'] <= image['payload_bytes'] - SERIAL_BYTES):
        raise ValueError('Invalid developer firmware binding record')
    at = 128 + binding['offset']
    if data[at:at + SERIAL_BYTES] != _serial_bytes(serial):
        raise ValueError('Developer firmware identity differs from its binding record')
    return at


def describe_binding(data, symbols, serial):
    image = inspect(data)
    matches = re.findall(r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[RrDdTt]\s+ln_bundle_serial$', symbols, re.M)
    if len(matches) != 1 or int(matches[0][1], 16) != SERIAL_BYTES:
        raise ValueError('Developer firmware identity symbol is missing or has the wrong size')
    binding = dict(format=1, offset=int(matches[0][0], 16) - image['load_address'], bytes=SERIAL_BYTES)
    validate_binding(data, dict(image=image, build_id=image['build_id'], device_serial=serial,
                               serial_binding=binding), serial)
    return binding


def bind(data, report, serial):
    if not isinstance(serial, str) or not re.fullmatch(r'[0-9A-F]{16}', serial) or serial == TEMPLATE_SERIAL:
        raise ValueError('Developer firmware needs the connected iPod USB identity')
    at = validate_binding(data, report, TEMPLATE_SERIAL)
    image = inspect(data)
    payload = bytearray(data[128:])
    payload[at - 128:at - 128 + SERIAL_BYTES] = _serial_bytes(serial)
    bound = pack(payload, 'dev', image['build_id'], image['load_address'], image['entry_address'], image['target_id'])
    updated = dict(report, device_serial=serial, image=inspect(bound), template_sha256=image['sha256'])
    validate_binding(bound, updated, serial)
    return bound, updated
