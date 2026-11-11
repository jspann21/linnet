"""Background operation values and Qt event signals."""
import threading
from dataclasses import dataclass

from PySide6.QtCore import QObject, Signal


@dataclass(frozen=True)
class OperationContext:
    id: int
    tag: str
    identity: dict | None
    cancel_event: threading.Event | None

    @property
    def can_cancel(self):
        return self.cancel_event is not None

@dataclass(frozen=True)
class OperationOutcome:
    context: OperationContext
    value: object = None
    status: str = 'succeeded'
    error: str = ''
    error_type: str = ''
    cancel_requested: bool = False

def operation_status(value):
    if isinstance(value, dict):
        if value.get('stopped'):
            return 'cancelled'
        if value.get('complete') is False or value.get('needs_preparation'):
            return 'partial'
    return 'succeeded'

class Events(QObject):
    done = Signal(object)
    progress = Signal(str)
    stage = Signal(object)
    conversion = Signal(object)
    artwork = Signal(int, str, object)
    tools_checked = Signal(int, object)
    setup_instruction = Signal(object)
