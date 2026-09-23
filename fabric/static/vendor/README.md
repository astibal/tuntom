# Vendored browser authentication primitive

`noble-auth.js` contains only SHA-256, HMAC-SHA256 and asynchronous PBKDF2
from `@noble/hashes` 2.4.0 (MIT), commit
`663c2aeeffc308ac0cded59bd32f7c212adacfc2`.

It is bundled as an IIFE named `TuntomCrypto` for browsers where Web Crypto is
unavailable on a plain-HTTP LAN origin. No package manager or runtime
dependency is installed on the Fabric host.

Bundle SHA-256:
`d2fedbc9b9a89bae977fafa03b0ba739f592c8d5166f4df30b550b93f185f083`.
