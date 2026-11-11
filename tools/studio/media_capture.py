"""Read-only screen capture for an explicitly selected running iPod."""
from tools.link.identity import decode
from tools.link.platform import png


IDENTITY_FIELDS = ('target_id', 'image_kind', 'profile', 'build_id', 'image_sha256', 'generation')


def available(connection):
    identity = (connection or {}).get('identity', {})
    return bool(connection and connection.get('serial') and identity.get('image_kind') == 'os'
                and identity.get('profile') in ('dev', 'trace'))


def capture(connection):
    """Verify the selected boot before and after a CRC-checked framebuffer read."""
    from tools.link.campaign import connect

    if not available(connection):
        raise ValueError('Screenshots need a Developer or Trace build running through Linnet Link. '
                         'They are unavailable in Disk Mode and recovery.')
    session, _, current = connect(serial_number=connection['serial'], deadline_seconds=10)
    try:
        def verify(observed):
            if any(observed[key] != connection['identity'][key] for key in IDENTITY_FIELDS):
                raise ValueError('The connected software changed. Choose Refresh before taking a screenshot.')

        verify(current)
        descriptor, pixels = session.capture_screen(current['generation'])
        verify(decode(session.status()))
        return dict(connection=connection, capture=descriptor,
                    png=png(pixels, descriptor['width'], descriptor['height']))
    finally:
        session.close()
