"""Cancellable host helpers for Apple media preparation."""
import os
import subprocess
import time


def checkpoint(cancelled):
    if cancelled():
        from .media_manager import Cancelled
        raise Cancelled('Stopped preparing Apple OS media.')


def run(arguments, *, timeout, cancelled=lambda: False):
    checkpoint(cancelled)
    command = [str(argument) for argument in arguments]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace',
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    deadline = time.monotonic() + timeout
    try:
        while True:
            checkpoint(cancelled)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(command, timeout)
            try:
                stdout, stderr = process.communicate(timeout=min(0.1, remaining))
                checkpoint(cancelled)
                return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
            except subprocess.TimeoutExpired:
                continue
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate()
