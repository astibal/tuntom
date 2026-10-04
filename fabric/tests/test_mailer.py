import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock
from urllib.error import HTTPError, URLError
from urllib.request import Request
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mailer import EmailMessage, ResendMailer, MailError, mailer_from_env, _NoRedirect


class MailerTests(unittest.TestCase):
    def setUp(self):
        self.mailer = ResendMailer('test-secret')
        self.mailer._opener = Mock()
        self.message = EmailMessage('Fabric <fabric@example.org>', ('admin@example.org',), 'Stav služby', text='V pořádku', html='<p>V pořádku</p>')

    def response(self, body=b'{"id":"message-123"}'):
        response = self.mailer._opener.open.return_value.__enter__.return_value = Mock()
        response.status = 200
        response.read.return_value = body

    def test_send_mapping_receipt_and_key(self):
        from unittest.mock import MagicMock
        self.mailer._opener.open.return_value = MagicMock()
        self.response()
        result = self.mailer.send(self.message, idempotency_key='event-123')
        self.assertEqual(result.message_id, 'message-123')
        self.assertEqual(result.provider, 'resend')
        request = self.mailer._opener.open.call_args.args[0]
        self.assertEqual(request.full_url, 'https://api.resend.com/emails')
        self.assertEqual(request.get_header('Idempotency-key'), 'event-123')
        self.assertEqual(request.get_header('Authorization'), 'Bearer test-secret')
        self.assertEqual(json.loads(request.data), {'from': self.message.sender, 'to': list(self.message.recipients), 'subject': self.message.subject, 'text': self.message.text, 'html': self.message.html})
        self.assertEqual(self.mailer._opener.open.call_args.kwargs['timeout'], 10)
        self.mailer._opener.open.assert_called_once()

    def test_error_mapping_without_provider_body_or_secrets(self):
        for status, code, retryable in [(401,'authentication',False),(403,'forbidden',False),(422,'rejected',False),(409,'conflict',False),(429,'rate_limited',True),(503,'provider_error',True),(302,'provider_error',False)]:
            self.mailer._opener.open.reset_mock()
            self.mailer._opener.open.side_effect = HTTPError('https://api.resend.com/emails', status, 'test-secret', {}, io.BytesIO(b'private content'))
            with self.assertRaises(MailError) as caught: self.mailer.send(self.message)
            self.assertEqual(caught.exception.code, code)
            self.assertEqual(caught.exception.retryable, retryable)
            self.assertNotIn('test-secret', str(caught.exception))
            self.mailer._opener.open.assert_called_once()

    def test_transport_is_uncertain_and_not_retried(self):
        self.mailer._opener.open.side_effect = URLError('test-secret')
        with self.assertRaises(MailError) as caught: self.mailer.send(self.message)
        self.assertTrue(caught.exception.uncertain)
        self.assertEqual(caught.exception.code, 'transport')
        self.mailer._opener.open.assert_called_once()

    def test_malformed_and_oversized_success_is_uncertain(self):
        from unittest.mock import MagicMock
        for raw in (b'{}', b'[]', b'broken', b'x'*65537):
            self.mailer._opener.open.return_value = MagicMock()
            self.response(raw)
            with self.assertRaises(MailError) as caught: self.mailer.send(self.message)
            self.assertTrue(caught.exception.uncertain)
            self.assertEqual(caught.exception.code, 'invalid_response')

    def test_invalid_inputs_never_reach_transport(self):
        for message in (EmailMessage('bad', ('a@example.org',), 'subject', text='text'),
                        EmailMessage('a@example.org', (), 'subject', text='text'),
                        EmailMessage('a@example.org', ('b@example.org',), 'subject'),
                        EmailMessage('a@example.org\r\nBcc: other@example.org', ('b@example.org',), 'subject', text='text')):
            with self.assertRaises(MailError): self.mailer.send(message)
        with self.assertRaises(MailError): self.mailer.send(self.message, idempotency_key='bad\nkey')
        self.mailer._opener.open.assert_not_called()

    def test_configuration_is_opt_in_and_redacted(self):
        self.assertIsNone(mailer_from_env({}))
        self.assertIsNone(mailer_from_env({'TUNTOM_MAIL_PROVIDER':'disabled'}))
        for env in ({'TUNTOM_MAIL_PROVIDER':'other'}, {'TUNTOM_MAIL_PROVIDER':'resend'}):
            with self.assertRaises(MailError): mailer_from_env(env)
        instance = mailer_from_env({'TUNTOM_MAIL_PROVIDER':'resend','TUNTOM_RESEND_API_KEY':'test-secret'})
        self.assertNotIn('test-secret', repr(instance))

    def test_redirect_is_not_followed(self):
        self.assertIsNone(_NoRedirect().redirect_request(Request('https://api.resend.com/emails'), None, 302, '', {}, 'https://other.example'))
