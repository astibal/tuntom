# Fabric Peek

Peek je externí HTTPS pozorovatel s pronájmem zájmu. Fabric mu requestem předá
targety, obnoví jejich sedmidenní lease a dostane čerstvý vzorek i historii.
Peek targety dál měří podle intervalu a historii standardně drží 30 dní.

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
python3 -B fabric/peek.py --host 127.0.0.1 --port 8780 \
  --history-db /var/lib/tuntom-peek/history.sqlite
```

Před veřejným nasazením má být Peek za HTTPS reverse proxy. Samotný server mluví
HTTP a bearer token proto nesmí cestovat otevřeným internetem bez TLS.

```bash
curl -sS https://peek.example/v1/probe \
  -H "Authorization: Bearer $TUNTOM_PEEK_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"targets":[{"id":"github","url":"https://github.com"}]}'
```

Výsledek obsahuje `results` s čerstvým měřením a `history` podle stabilního ID
targetu. Měření obsahuje dostupnost, HTTP status, TCP connect čas, TLS handshake RTT,
HTTP response čas, TLS verzi a cipher, výsledek systémové kontroly důvěry a
základní metadata certifikátu. Nedůvěryhodný certifikát Peek znovu načte bez
ověření, ale ve výsledku zůstane `trusted: false` a původní chyba validace.

Detailní grafy čtou
`GET /v1/history/<target-id>?after=<unix>&before=<unix>&metric=total_ms&points=1200`.
Endpoint vyžaduje stejný bearer token. Delší intervaly vrací časové buckety s
průměrem a minimem/maximem, takže přenos zůstává omezený a outliery se neztratí.

Peek přijímá nejvýše 64 cílů na požadavek, má omezený worker pool a odmítá cíle,
které se přeloží na neveřejnou IP adresu. Redirecty nesleduje: měří přesně URL,
kterou Fabric zadal.

## Vlastnictví konfigurace

API záměrně neurčuje, odkud Fabric seznam získá. Aktuální kontrakt je čistá
funkce `targets -> observations`; později mohou cíle vzniknout například mapováním
labelu na externí službu. Autoritativní konfigurace tím nevzniká na Peek serveru.

## Test konkrétní DNS služby

V Managed Service lze přes formulář „Test DNS služby“ vybrat server, explicitní
port, DNS/DoT/DoH, jméno, typ záznamu a volitelné kontroly. Builder přidá target
do návrhu; aktivuje se až uložením služby. Existující HTTPS targety se nemění.
Target je uložen v URL, takže stejný test zachová scheduler, lease i historii:

```text
dns://1.1.1.1:53/?name=example.com&type=AAAA
dot://one.one.one.one:853/?name=example.com&type=AAAA&ad=1
doh://cloudflare-dns.com:443/dns-query?name=example.com&type=AAAA&ad=1
```

`doh://` je pouze konfigurační označení; přenos používá HTTPS POST s DNS wire
zprávou (`application/dns-message`, RFC 8484). `name`, `type`, `ad`, `expect`
nejsou HTTP query parametry odesílané serveru. Cesta je volitelná, výchozí
`/dns-query`. DoT používá DNS framing přes TLS (RFC 7858); běžné DNS používá
UDP a při TC odpovědi opakuje dotaz přes TCP na stejném serveru/portu.

Typy: A, AAAA, NS, CNAME, SOA, PTR, MX, TXT, SRV. PTR zadává reverzní jméno.
`expect` je volitelné URL-encoded očekávání: musí odpovídat alespoň jedné hodnotě
požadovaného typu pro dotazované jméno nebo jeho CNAME řetězec. Další hodnoty
nevadí. Pro A/AAAA se validuje rodina IP; MX používá `priorita jméno`, SRV
`priorita váha port jméno`, TXT spojuje části jednoho RR. Typy NS/CNAME/PTR
porovnávají jména bez ohledu na velikost písmen a koncovou tečku.

Úspěch vyžaduje NOERROR a alespoň jeden požadovaný záznam. NXDOMAIN/NODATA,
SERVFAIL, chybějící očekávaná hodnota nebo požadované AD jsou neúspěšný test,
ikoli nutně nedostupný server. Výsledek obsahuje RCODE, AD, odpovědi s TTL,
kontrolu očekávání a `dns_response_ms` (celý DNS transport včetně navázání).
Graf podporuje tuto metriku vedle celkového RTT a TLS handshake.

`ad=1` vyžaduje tvrzení resolveru o DNSSEC validaci (Authenticated Data).
Peek provádí dotaz s RD/AD a EDNS DO, nikoli vlastní DNSSEC validaci řetězce
podpisů. Přes nešifrované DNS není AD chráněno před změnou po cestě.
DoT/DoH vyžadují platný certifikát serveru a nedowngradují na neověřené TLS.
Cílový resolver nadále musí mít veřejné IP adresy; privátní adresy vrácené
v DNS odpovědi se mohou zobrazit, ale Peek se na ně nepřipojuje.

Specifikace: https://datatracker.ietf.org/doc/html/rfc8484 a
https://www.rfc-editor.org/rfc/rfc7858.

Přehledové `history` v odpovědi `/v1/probe` obsahuje nejvýše 120 posledních
vzorků na target a celkový rozpočet 512 KiB rovnoměrně rozdělený mezi targety.
Úplná uložená historie se tím nemaže; detailní graf používá `/v1/history`.
