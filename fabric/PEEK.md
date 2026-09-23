# Fabric Peek

Peek je bezstavová externí HTTPS sonda. Fabric collector jí při každém pollu
pošle úplný seznam právě požadovaných cílů; Peek nic neplánuje a neukládá.

```text
Fabric collector -- POST /v1/probe --> Peek na razoru --> veřejná HTTPS služba
```

## Jednorázová sonda

```bash
python3 -B fabric/peek.py --once https://github.com
```

## Server

```bash
export TUNTOM_PEEK_TOKEN='nahodny-token-alespon-24-znaku'
python3 -B fabric/peek.py --host 127.0.0.1 --port 8780
```

Před veřejným nasazením má být Peek za HTTPS reverse proxy. Samotný server mluví
HTTP a bearer token proto nesmí cestovat otevřeným internetem bez TLS.

```bash
curl -sS https://peek.example/v1/probe \
  -H "Authorization: Bearer $TUNTOM_PEEK_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"targets":[{"id":"github","url":"https://github.com"}]}'
```

Výsledek obsahuje dostupnost, HTTP status, TCP connect čas, TLS handshake RTT,
HTTP response čas, TLS verzi a cipher, výsledek systémové kontroly důvěry a
základní metadata certifikátu. Nedůvěryhodný certifikát Peek znovu načte bez
ověření, ale ve výsledku zůstane `trusted: false` a původní chyba validace.

Peek přijímá nejvýše 64 cílů na požadavek, má omezený worker pool a odmítá cíle,
které se přeloží na neveřejnou IP adresu. Redirecty nesleduje: měří přesně URL,
kterou Fabric zadal.

## Vlastnictví konfigurace

API záměrně neurčuje, odkud Fabric seznam získá. Aktuální kontrakt je čistá
funkce `targets -> observations`; později mohou cíle vzniknout například mapováním
labelu na externí službu. Autoritativní konfigurace tím nevzniká na Peek serveru.
