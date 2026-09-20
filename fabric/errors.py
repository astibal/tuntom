"""Shared API errors for both CLI entry points and the Unix collector client."""


class APIError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
