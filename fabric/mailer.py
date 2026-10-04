"""Server-only email abstraction. Importing/configuring this module never sends mail."""
from __future__ import annotations

from dataclasses import dataclass, field
from email.utils import parseaddr
from http.client import HTTPException
import json
import math
import os
from typing import Mapping, Protocol
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener


class MailError(Exception):
    """Sanitized failure; uncertain means the provider may have accepted the email."""
    def __init__(self, code: str, *, status: int | None = None,
                 retryable: bool = False, uncertain: bool = False):
        super().__init__(f'Email send failed: {code}')
        self.code, self.status = code, status
        self.retryable, self.uncertain = retryable, uncertain


@dataclass(frozen=True)
class EmailMessage:
    sender: str
    recipients: tuple[str, ...]
    subject: str
    text: str | None = field(default=None, repr=False)
    html: str | None = field(default=None, repr=False)


@dataclass(frozen=True)
class SendReceipt:
    message_id: str
    provider: str


class Mailer(Protocol):
    def send(self, message: EmailMessage, *, idempotency_key: str | None = None) -> SendReceipt: ...


def _address(value):
    if not isinstance(value, str) or not value or len(value) > 320 or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise MailError('invalid_message')
    address = parseaddr(value)[1]
    if address.count('@') != 1 or not all(address.split('@')):
        raise MailError('invalid_message')
    return value


def _payload(message):
    if not isinstance(message, EmailMessage):
        raise MailError('invalid_message')
    if not isinstance(message.recipients, (tuple, list)) or not 1 <= len(message.recipients) <= 50:
        raise MailError('invalid_message')
    if not isinstance(message.subject, str) or not message.subject.strip() or len(message.subject) > 998 or any(ord(c) < 32 for c in message.subject):
        raise MailError('invalid_message')
    data = {'from': _address(message.sender), 'to': [_address(x) for x in message.recipients], 'subject': message.subject}
    for name in ('text', 'html'):
        value = getattr(message, name)
        if value is not None:
            if not isinstance(value, str): raise MailError('invalid_message')
            data[name] = value
    if not (message.text or message.html): raise MailError('invalid_message')
    try:
        body = json.dumps(data, ensure_ascii=False).encode('utf-8')
    except UnicodeError:
        raise MailError('invalid_message') from None
    if len(body) > 1024 * 1024: raise MailError('message_too_large')
    return body


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class ResendMailer:
    """One request per send. No implicit retries, queues, logging or delivery claims."""
    def __init__(self, api_key: str, *, timeout: float = 10):
        if not isinstance(api_key, str) or not api_key or len(api_key) > 512 or any(ord(c) <= 32 or ord(c) > 126 for c in api_key):
            raise MailError('invalid_configuration')
        if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or not math.isfinite(timeout) or not 0 < timeout <= 60:
            raise MailError('invalid_configuration')
        self._api_key, self._timeout = api_key, timeout
        self._opener = build_opener(_NoRedirect())

    def send(self, message: EmailMessage, *, idempotency_key: str | None = None) -> SendReceipt:
        body = _payload(message)
        headers = {'Authorization': 'Bearer ' + self._api_key, 'Content-Type': 'application/json',
                   'Accept': 'application/json', 'User-Agent': 'Tuntom-Fabric/1'}
        if idempotency_key is not None:
            if not isinstance(idempotency_key, str) or not 1 <= len(idempotency_key) <= 256 or any(ord(c) < 33 or ord(c) > 126 for c in idempotency_key):
                raise MailError('invalid_idempotency_key')
            headers['Idempotency-Key'] = idempotency_key
        request = Request('https://api.resend.com/emails', data=body, headers=headers, method='POST')
        try:
            with self._opener.open(request, timeout=self._timeout) as response:
                status = response.status
                raw = response.read(65537)
        except HTTPError as error:
            status = error.code
            error.close()
            code = {401: 'authentication', 403: 'forbidden', 429: 'rate_limited',
                    400: 'rejected', 422: 'rejected', 409: 'conflict'}.get(status, 'provider_error')
            raise MailError(code, status=status, retryable=status == 429 or status >= 500,
                            uncertain=status >= 500) from None
        except (URLError, OSError, TimeoutError, HTTPException):
            raise MailError('transport', retryable=True, uncertain=True) from None
        if not 200 <= status < 300:
            raise MailError('provider_error', status=status, uncertain=True)
        try:
            if len(raw) > 65536: raise ValueError
            data = json.loads(raw)
            message_id = data.get('id') if isinstance(data, dict) else None
            if not isinstance(message_id, str) or not 1 <= len(message_id) <= 256 or any(ord(c) < 33 or ord(c) > 126 for c in message_id):
                raise ValueError
        except (ValueError, UnicodeError):
            raise MailError('invalid_response', uncertain=True) from None
        return SendReceipt(message_id, 'resend')


def mailer_from_env(env: Mapping[str, str] | None = None) -> Mailer | None:
    """Explicit opt-in for future server consumers; not wired into Fabric startup."""
    env = os.environ if env is None else env
    provider = env.get('TUNTOM_MAIL_PROVIDER', '').strip().lower()
    if not provider or provider == 'disabled': return None
    if provider != 'resend': raise MailError('invalid_configuration')
    return ResendMailer(env.get('TUNTOM_RESEND_API_KEY', ''))
