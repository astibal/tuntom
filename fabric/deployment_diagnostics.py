"""Bounded, secret-redacted deployment output suitable for stored diagnostics."""
import re


def safe_output(value, limit=4096):
    if isinstance(value, bytes): value = value.decode(errors='replace')
    text = str(value or '')
    text = re.sub(r'-----BEGIN [^-]*PRIVATE KEY-----.*?(?:-----END [^-]*PRIVATE KEY-----|$)', '[REDACTED PRIVATE KEY]', text, flags=re.S)
    text = re.sub(r'(?im)((?:[\w-]*(?:secret|token|password|private_key)[\w-]*)\s*[=:]\s*)[^\r\n]+', r'\1[REDACTED]', text)
    text = re.sub(r'(?i)(Bearer\s+)\S+', r'\1[REDACTED]', text)
    text = re.sub(r'(?i)\b[0-9a-f]{32,}\b', '[REDACTED KEY/HASH]', text)
    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', text)
    text = ''.join(c for c in text if c in '\n\t' or ord(c) >= 32)
    return text[-limit:].strip()
