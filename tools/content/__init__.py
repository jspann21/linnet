"""Linnet deterministic host content compilers."""


class SourceError(ValueError):
    def __init__(self, message, source):
        super().__init__(message)
        self.source = source
