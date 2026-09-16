# Tuntom Fabric Observer

Dočasný **stateless dashboard nad právě běžícími procesy**. Nahrazuje opakované
`ps`, čtení parametrů a `tuntomctl show stats`. Nemá inventář, databázi ani
konfiguraci požadovaného stavu. Procesy znovu objevuje při každém scanu.

```text
prohlížeč → HTTP API (běžný uživatel) → collector → /proc + control sockety
                                      │         → logy na vyžádání
                                      └ v procesu, nebo zvlášť přes Unix socket
```

## Spuštění

Z kořene repozitáře; Linux a Python **3.10+**, bez dalších balíčků:

```bash
python3 -B fabric/server.py
```

Otevři odkaz vypsaný do terminálu. Obsahuje náhodný přístupový token ve fragmentu
URL; stránka ho přesune do `sessionStorage` a z adresy odstraní. API výchozí poslouchá
na **`0.0.0.0:8765`** (všechna IPv4 rozhraní). Pro přístup z jiného stroje nahraď
`127.0.0.1` ve vypsaném odkazu skutečnou IPv4 adresou serveru; token ponech.
Ukončíš ho přes Ctrl+C. Při příštím spuštění si vše znovu najde.

Vpravo nahoře přepneš rozhraní přes **EN / CZ / FR**. Výchozí je čeština;
volba se ukládá pouze v prohlížeči (`localStorage`). Přepnutí zachová filtry,
graf i rozepsaná pravidla. Názvy metrik, syntaxe pravidel, podrobné zprávy daemonu
a exportované JSON zůstávají v původním formátu.

```bash
python3 -B fabric/server.py --port 8766 --interval 3
python3 -B fabric/server.py --host 127.0.0.1  # pouze místní přístup
python3 -B fabric/server.py --host 0.0.0.0    # všechna IPv4 rozhraní (výchozí)
python3 -B fabric/server.py --allow-write   # navíc ruční načítání pravidel
```

Volitelný `TUNTOM_FABRIC_TOKEN` nastaví stabilní token (alespoň 24 znaků,
písmena/číslice/`_.~-`). Bez něj se pro každé spuštění vygeneruje nový.

Pro vzdálený stroj spusť observer přímo tam a přenes port přes SSH; oba porty
ponech stejné kvůli kontrole HTTP Host:

```bash
ssh -L 8765:127.0.0.1:8765 user@host
# Na vzdáleném stroji, pro přístup pouze přes SSH:
cd /cesta/k/tuntom
python3 -B fabric/server.py --host 127.0.0.1
```

Observer potřebuje stejná oprávnění ke čtení `/proc` a připojení ke control
socketům jako ručně spuštěné příkazy. Automaticky nepoužívá sudo. Pokud socket
není přístupný, proces zůstane v přehledu s vysvětlením. Neaktivuje systemd služby.

### Oddělený collector pro rootové daemony

HTTP server pod rootem odmítne start. Vyšší oprávnění dostane pouze samostatný
collector; web dál běží pod tebou. Z kořene repozitáře ve dvou terminálech:

```bash
# Terminál 1: čtení /proc, control socketů a logů, bez HTTP
sudo python3 -B fabric/collector.py \
  --socket "/run/tuntom-fabric-$(id -u).sock" --allow-uid "$(id -u)"

# Terminál 2: web pod běžným uživatelem
python3 -B fabric/server.py --collector "/run/tuntom-fabric-$(id -u).sock"
```

Před druhým příkazem ukonči předchozí náhled na portu 8765. `sudo` může požádat
o tvé heslo. Na běžící daemony ani jejich socketová oprávnění se nesahá.

Collector přijímá jen určené UID (`SO_PEERCRED`), jeho socket má práva `0600`.
Adresář socketu musí patřit collectoru a nesmí být zapisovatelný skupinou ani
ostatními. Existující socket nepřepisuje; při čistém ukončení svůj socket odstraní.
Po pádu nejprve ověř, že starý collector neběží, než jeho socket ručně smažeš.

Pro požadavky přijímá pouze identitu objeveného procesu a pevný seznam operací,
žádné libovolné cesty ani shell příkazy. Web ověřuje UID collectoru. Chceš-li
ruční load pravidel, zapni `--allow-write` **u collectoru i webu**; čtení a validace
fungují i bez něj. Interval sběru určuje `--interval` na collectoru.

Oddělení lze použít i bez roota s adresářem a socketem vlastněným tvým UID.
Collector drží jen aktuální procesy a předchozí vzorek v paměti, bez inventáře.

## Co už umí

- Najít tunely `tuntom`, `tuntom_42c`, `tuntom_42_1s`, oba switche, adaptéry,
  divert a rozpoznatelné instalace s binárkou `main`.
- Ukázat PID, UID, uptime, RSS, počet vláken, binárku, systemd unit a veřejné
  parametry: rozhraní, peer, sockety, porty, MTU a cesty ke konfiguračním souborům.
- Číst metriky přes `SOCK_SEQPACKET`, odlišit dostupný proces od dostupných
  metrik a navázané tunnel session; vykreslit RX/TX a filtrovat všechny metriky.
- Zobrazit zdraví procesu, spojení, provozu a nové chyby s důvodem. Klidový provoz
  je normální stav; nedostupná telemetrie je neznámý stav, nikoliv zdravý daemon.
  Upozornění je červené a v tabulce rovnou uvádí příčinu, například
  `malformed_frames: +25 za 5 s` nebo `session_ready=0`. Kliknutím otevřeš detail;
  z konkrétní příčiny přejdeš na související metriku, také zvýrazněnou červeně.
  Samostatná historie drží nejvýše **5 upozornění celkem po dobu 5 minut** od
  jejich zachycení ve stránce. Ukazuje stáří (`−120 s`) a původní příčiny i po
  zotavení nebo zániku procesu. Křížek záznam zavře; stejný vzorek ho neobnoví,
  nový chybový vzorek může vytvořit další. Stáří a expirace běží i při pauze.
  Historie zůstává jen v paměti stránky a po F5 se vymaže.
- Vybrat proces kliknutím kamkoliv do jeho řádku. Tlačítka v řádku zachovávají
  vlastní akci a výběr textu nepřepíná proces.
- Spočítat přesné přírůstky čítačů mezi vzorky, včetně skutečné délky intervalu.
  Po výpadku čeká na dva nové vzorky. Pokles čítače nebo výměnu portu označí jako
  reset; záporné přírůstky nevymýšlí. Policy/rewrite dropy zobrazuje, ale automaticky
  je nepovažuje za poruchu.
- Ukázat porty MP switche, jejich generace, IPC metriky a přiřazení RX/TX workerům.
  CPU workeru vychází z přírůstku `cpu_ns` / uplynulý čas (100 % = jedno jádro).
  Fronty, obsazené buffery a dropy čte z dostupných metrik. **Aktuální zaplnění
  jednotlivých front daemon neexportuje**; dashboard ho neodhaduje.
  Identita switche, fronty, registrované porty a workery jsou v jednom společném rámu.
  IPC metriky mají vlastní tabulku klíč/hodnota s čísly zarovnanými doprava.
  Ikonka `i` u metrik nabízí nápovědu na hover i po kliknutí, v CZ/EN/FR.
  Popisy rozlišují součty, aktuální stav, běžné chování a možné příčiny problémů;
  u neznámých klíčů je chybějící podrobný popis výslovně uvedený.
- Odvodit lokální vazby ze stejné cesty ke switch socketu ve stejné mount
  namespace. Když namespace nelze přečíst, jednoznačnou shodu cesty označí `(?)`;
  nejednoznačné shody vynechá. MP switch navíc poskytuje skutečně registrované názvy portů.
  Topologie je klikací: vybere proces nebo otevře pravidla odpovídajícího switche.
  Shoda s živým názvem portu je označená zvlášť; neověřuje totožnost jeho protistrany.
- Načíst aktivní pravidla, ověřit návrh daemonem, ukázat diff a volitelně ho
  ručně načíst. Pravidla se do souborů nezapisují.
- Na vyžádání číst posledních nejvýše 60 řádků / 64 KiB z běžných souborů
  otevřených jako stdout/stderr. Pokud nejsou k dispozici, zkusí journal omezený na
  PID, boot a dobu života procesu. Nikdy nekonzumuje pipe, socket ani zařízení.
- Kopírovat nebo uložit diagnostický JSON: vybraný proces, metriky, přírůstky,
  lokální vazby, logy a dostupná aktivní pravidla. Report má v UI náhled.
  Logy maskují známá tajná pole a bloky privátních klíčů; jde o best effort,
  vlastní formáty logů mohou obsahovat další citlivé údaje. Před sdílením ho zkontroluj.
- Stáhnout aktuální snapshot jako JSON. Graf drží až **24 hodin** pouze v paměti
  stránky, s rozsahy **5m (výchozí), 1h, 12h, 24h**. Přepnutí rozsahu historii
  nemaže; delší pohled zhušťuje vykreslení se zachováním minim, špiček a mezer.
  Sběr probíhá při obnovování viditelné stránky; pauza/skrytá karta zanechá mezeru,
  F5 historii vymaže. Historie ukončeného procesu se zahodí.
- Kliknutím na graf RX/TX nebo CPU ukazatel workeru otevřít velký detail.
  Pod kurzorem ukazuje čas a přesnou hodnotu původního vzorku; kliknutím bod
  připneš i přes refresh. Šipky procházejí vzorky, Home/End první/poslední,
  Escape zavírá detail. Rozsah 5m/1h/12h/24h je společný s malým grafem.
  Také historie CPU se sbírá jen v paměti stránky.
- Červené body pod křivkou zachovávají problémy konkrétního procesu v okamžiku
  sběru, včetně původního důvodu a délky intervalu chybových přírůstků.
  Zůstávají s grafem až 24 h, nezávisle na zavření nebo expiraci balónků.
  Blízké body se v dlouhém rozsahu seskupí; detail ukáže jejich počet a časový
  rozsah. Hodnoty skupiny patří prvnímu vzorku, šipkami lze odečíst jednotlivé
  vzorky. Značka u CPU patří celému procesu switche, nedokazuje chybu workeru.
  Výpadek čtení metrik má značku i bez hodnot; výpadek API bez vzorku zůstává
  mezerou. Čas značky je čas pozorování, nikoli přesný okamžik jednotlivé chyby.

Proces zmizí po ukončení a nově spuštěný se objeví automaticky. Identita obsahuje
boot ID, PID a čas vytvoření procesu. Před ruční operací se proces znovu dohledá;
control klient ověřuje PID protistrany pomocí `SO_PEERCRED` i čas vytvoření.
Chyba při čtení metrik zahodí předchozí hodnoty a zobrazí nedostupnost.

### Rozsah prvního kroku

Objevuje pouze procesy viditelné v aktuálním `/proc`. Skryté PID, kontejnery a
procesy přejmenované mimo rozpoznávané názvy mohou chybět. U jiné mount namespace
zobrazí proces, ale nezkouší jeho socket v namespace observeru. Neodvozuje
vzdálenou topologii z pouhé shody tunnel ID. Vazba z parametrů neprokazuje průchod dat.

Metriky potřebují explicitní `--control-socket`. Observer čte pouze veřejné
parametry z povoleného seznamu; nečte `/proc/PID/environ`, klíče, cookie ani obsah
konfiguračních souborů. Ukazuje parametry, které proces dostal při startu, a
aktuální metriky/pravidla dostupná přes control protokol.

HTTP server může sloužit i přes LAN; token je stále povinný. Přístup je přes
HTTP bez TLS, tedy i token přenáší nešifrovaně; pro nedůvěryhodnou síť použij SSH
forward s `--host 127.0.0.1`. Kontrola Host přijímá localhost a konkrétní cílovou
IPv4 adresu přijatého spojení, nikoli libovolné DNS jméno. Cizí Origin a cross-site
požadavky odmítá. Statické soubory mají pevné cesty a nepotřebují CDN ani internet.

## API v1

Vše kromě `/healthz` a statických souborů vyžaduje
`Authorization: Bearer <token>`. JSON čítače z daemonu zůstávají řetězce,
aby se neztratila přesnost uint64.

| Metoda | Cesta | Výsledek |
|---|---|---|
| GET | `/healthz` | Stav API serveru, nikoliv zdraví fabric |
| GET | `/api/v1/snapshot` | Nalezené procesy, scan, metriky, chyby a odvozené vazby |
| POST | `/api/v1/refresh` | Naplánuje nový scan; odpověď 202 |
| GET | `/api/v1/endpoints/{id}/rules` | Aktivní ruleset a `sha256` |
| POST | `/api/v1/endpoints/{id}/rules/check` | Validace a diff vůči právě aktivním pravidlům |
| POST | `/api/v1/endpoints/{id}/rules/load` | Validace a runtime load; vyžaduje `--allow-write` |
| GET | `/api/v1/endpoints/{id}/logs` | Omezený výpis logů, zdroj, čas a případné chyby |
| GET | `/api/v1/endpoints/{id}/diagnostics` | Sestavený diagnostický report vybraného procesu |

Snapshot navíc obsahuje `collector`, u každého procesu `health`, `changes`
a u switchů `switch_detail`. Přírůstky zůstávají desetinné řetězce stejně jako
původní čítače. `changes.ready=false` znamená chybějící srovnávací vzorek;
`resets` obsahuje čítače, pro které přírůstek není platný.

`id` převezmi ze snapshotu. Pro `check` pošli `{"rules":"format 2\nserial …\n…"}`.
`load` navíc vyžaduje `expected_sha256` z kontroly. Nový ruleset musí vyhovovat
pravidlům serialu implementovaným switchem.

API serializuje vlastní změny a odmítne změněnou aktivní konfiguraci kódem 409.
Daemona ale může souběžně změnit i externí `tuntomctl`: control protokol nemá
atomický compare-and-swap, takže kontrola hashe není zámkem vůči jiným klientům.
Při timeoutu loadu API hlásí nejistý výsledek; znovu načti aktivní pravidla před
dalším pokusem. Úspěšné loady se zapisují do stderr jako události s hashi, bez
obsahu pravidel. Log ani historii aplikace sama neukládá.

## Rozšíření později

Oddělené vrstvy umožňují přidávat funkce postupně:

```text
discovery.py  → pozorovaný proces + veřejné parametry + stabilní identita
control.py    → omezené operace existujícího control protokolu
telemetry.py  → zdraví, přírůstky, porty, workery
logs.py       → omezené čtení logů a maskování známých tajných polí
collector.py  → samostatný sběr, Unix IPC a kontrola UID
server.py     → snapshot, diagnostika, ruční akce, HTTP bez roota
static/       → pohledy nad pozorovaným stavem
```

Další kroky mohou přidat síťová rozhraní/routes, skutečné zaplnění front
(po rozšíření metrik daemonu) nebo další ruční runtime akce. Pro více strojů lze přidat další zdroj
discovery; konkrétní akce vždy patří k identitě nalezeného procesu. Není nutné
kvůli tomu zavádět databázi, provisioning ani trvalý inventář.

## Ověření

```bash
python3 -B -m unittest discover -s fabric/tests -v
node --check fabric/static/app.js
node --test fabric/tests/warnings.test.js
node --test fabric/tests/throughput.test.js
```

Testy pokrývají discovery, filtrování neveřejných parametrů, přesné čítače,
framing/limity/protistranu socketu a HTTP přístup. Pokud existují
`cmake-build-debug/tuntom-switch` a `cmake-build-debug/tomtom-switch-mp`, spustí
navíc oba switche v dočasném adresáři a ověří nalezení, metriky, pravidla,
konflikt změny, registrované porty, přenos rámce, logy a zmizení procesu.
Integrační scénář vede přes HTTP → samostatný Unix collector → skutečný daemon.
Další testy ověřují delty a resety, zdraví, výměnu portů, odmítnutí cizího UID,
limity IPC a logů i zachování obsahu pipe. Testy potřebují povolené místní Unix/HTTP sockety;
nevyžadují root ani TUN.
