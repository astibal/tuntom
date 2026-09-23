# Tuntom Fabric Observer

Dočasný **stateless dashboard nad právě běžícími procesy**. Nahrazuje opakované
`ps`, čtení parametrů a `tuntomctl show stats`. Nemá inventář ani
konfiguraci požadovaného stavu. Telemetrii uchovává v postradatelné SQLite cache. Procesy znovu objevuje při každém scanu.

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

Dole v postranním panelu přepneš rozhraní přes **EN / CZ / FR**. Výchozí je čeština;
volba se ukládá pouze v prohlížeči (`localStorage`). Přepnutí zachová filtry,
graf i rozepsaná pravidla. Názvy metrik, syntaxe pravidel, podrobné zprávy daemonu
a exportované JSON zůstávají v původním formátu.

```bash
python3 -B fabric/server.py --port 8766 --interval 3
python3 -B fabric/server.py --host 127.0.0.1  # pouze místní přístup
python3 -B fabric/server.py --host 0.0.0.0    # všechna IPv4 rozhraní (výchozí)
python3 -B fabric/server.py --allow-write   # navíc ruční načítání pravidel
python3 -B fabric/server.py --users-file ~/.config/tuntom/fabric-users.json
python3 -B fabric/server.py --services-db ~/.local/state/tuntom-fabric/services.sqlite
```

### Lokální administrátoři

`--users-file` zapne lokální účty `admin` a `admin-ro`. Soubor nemusí před
prvním spuštěním existovat. Přihlas se bootstrap tokenem, otevři
**Administrátoři** a založ první účet. Server zapisuje JSON atomicky s módem
`0600`; do prohlížeče ani API nikdy neposílá uložené verifiery.

Heslo se po HTTP neposílá. Přihlášení používá salted challenge/response odvozené
z SCRAM-SHA-256 a PBKDF2-HMAC-SHA256 (600 000 iterací). Každý další API požadavek
je podepsán klíčem odvozeným z přihlášení a obsahuje krátce platnou časovou značku
a jednorázový nonce. To brání pasivnímu odposlechu hesla, krádeži bearer session
a replayi. Čisté HTTP ale nemůže zabránit aktivnímu útočníkovi změnit doručený
JavaScript; proti tomu nadále použij HTTPS nebo SSH tunel.

`admin-ro` může číst stav, logy, flows a provádět validace. Runtime změny rulesetu
a klasifikátoru i správa účtů vyžadují roli `admin`. Bootstrap token má vždy
oprávnění `admin` a slouží jako recovery přístup.

Pro pohodlné přihlašování v labu můžeš webům nastavit jeden stálý token:

```bash
python3 -B fabric/server.py --golden-token 'muj-spolecny-lab-token-2026'
# Stejná volba funguje i s --collector /run/tuntom-fabric-1000.sock.
```

Pořadí je **`--golden-token TOKEN` → `TUNTOM_FABRIC_TOKEN` → náhodný token**.
`--golden-token` dovoluje libovolný neprázdný token z písmen/číslic/`_.~-`;
při délce pod 24 znaků pouze vypíše varování bez hodnoty tokenu. Pro
`TUNTOM_FABRIC_TOKEN` zůstává minimum 24 znaků. Token se zadává
ve stávajícím přihlašovacím poli nebo odkazem z terminálu a platí i po restartu,
pokud web spustíš se stejnou hodnotou. Volba patří pouze HTTP webu, collector ji
nepotřebuje. Jde o pevný bearer token, nikoli další účet nebo obcházení autorizace.
Hodnota CLI argumentu je vidět v seznamu procesů; mimo lab preferuj dosavadní
proměnnou prostředí. PAM přihlášení tato volba nezavádí.

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
Collector drží aktuální procesy a předchozí vzorek v paměti, bez inventáře.
Historii telemetrie ukládá do cache popsané níže.

## Historie telemetrie (SQLite cache)

Výchozí ukládání je na collectoru, každých `--interval` sekund (výchozí 5 s):

- root collector: `/var/cache/tuntom-fabric/history.sqlite`;
- běžný uživatel: `$XDG_CACHE_HOME/tuntom-fabric/history.sqlite`, jinak
  `~/.cache/tuntom-fabric/history.sqlite`;
- jiná cesta: `--history-db /cesta/k/privatnimu/adresari/history.sqlite`;
- vypnutí: `--no-history`.

Při odděleném sběru nastavuj tyto volby **na collector.py**, ne na webu.
U samostatného `server.py` platí pro jeho interní collector. Rodič databáze
musí patřit účtu collectoru a mít práva 0700, soubor 0600. Adresář se vytvoří
automaticky. Pro systemd lze použít `CacheDirectory=tuntom-fabric` a
`CacheDirectoryMode=0700` s cestou `--history-db /var/cache/tuntom-fabric/history.sqlite`.

Retence je **24 h**, staré záznamy se průběžně mažou a SQLite jejich stránky
znovu používá. Ukládají se původní metriky a přírůstky (uint64 jako přesné řetězce),
RX/TX, CPU workerů a příčiny varování. Neukládají se argumenty procesů,
konfigurace, pravidla, logy ani přístupové tokeny. Identita obsahuje boot ID,
PID a start procesu; po restartu procesu se graf nepřipojí ke staré instanci.
UI zůstává přehledem právě běžících procesů, není archivním prohlížečem.

Cache přežije F5 a restart collectoru/webu/stroje. Při úklidu ji lze smazat
**po zastavení collectoru** (včetně případných souborů `-wal`/`-shm`); při dalším
startu vznikne prázdná. Zpětně nelze doplnit dobu, kdy collector neběžel.
Chyba zápisu za běhu nezastaví živý sběr; API a graf zobrazí nedostupnost historie.
Poškozená/nepřístupná databáze při startu vyvolá chybu, nemaže se potichu.

`GET /api/v1/endpoints/{id}/history?after=0&until=<ms>` vyžaduje stejný token
jako snapshot. Vrací `samples` (body grafů), `until` a `next_after`;
čas je v celočíselných Unix milisekundách. Další stránka používá `next_after`
a stejné `until`, konec značí `null`. Stránka má nejvýše 250 vzorků.
Plné čítače jsou uložené v cache, tento endpoint vrací pouze data existujících grafů.

## Systémové metriky uzlů přes Syspiper

Volitelný sběr zapneš pouze na **collectoru**; HTTP web Syspiper volby nemá:

```bash
sudo python3 -B fabric/collector.py \
  --socket /run/tuntom-fabric-1000.sock --allow-uid 1000 \
  --syspiper-key 'TVUJ_KLIC' --syspiper-node 192.168.55.143
```

Alternativa k `--syspiper-key` je `TUNTOM_SYSPIPER_KEY` v prostředí **collectoru**
(u sudo ji případně nastav v prostředí služby). Web ani prohlížeč klíč nedostanou;
nevstupuje do snapshotu, historie ani diagnostického exportu. Hodnota CLI volby je
viditelná v seznamu procesů. Bez klíče je Syspiper sběr vypnutý. Web spusť s `--collector`;
samostatný `server.py` Syspiper nesbírá.

Cíle jsou pouze konkrétní známé IP, nikoli skenování sítě:

1. **Vždy `127.0.0.1`**, i bez běžících procesů; v namespace collectoru.
2. Ručně doplněné IP přes opakovatelné `--syspiper-node IP`.
3. IPv4 hinty z `peer_info_access` (V5 INFO, patch `aaa9b4c`) z aktuálních
   statistik tunelu: `session_ready=1`, `info_msg_peer_received=1`. Platí pro
   klienta i server. Vzdálený Tuntom musí mít `--info-msg-enable`; oznamuje
   explicitní ne-127/8 IPv4 adresy svého loopback rozhraní po handshaku/rekey.
   Fabric odmítá loopback, neplatné a ne-unicast cíle. Vlastní INFO pole nejsou
   zdrojem pollingových cílů. Hint není zárukou dosažitelnosti ani služby.

Bez INFO se žádná vzdálená adresa neodhaduje. Rozhraní hostu ani tunelů se
pro výběr cílů neprocházejí. Jedna IP se polluje jen jednou, i pokud ji oznámí
více tunelů nebo se zároveň zadá ručně.

Vnější adresa protistrany z parametrů tunelu se už automaticky nepolluje
(a DNS se nepřekládá). Pro starší peer bez INFO použij `--syspiper-node`.
Prázdné INFO nebo ztráta aktuálních statistik/session odstraní hint ze seznamu
cílů při dalším discovery Syspiperu (výchozí interval 30 s); právě probíhající
poll může doběhnout. Nevytváří se routy ani proxy přes tunel; HTTP polling
používá existující síťovou dosažitelnost a backendový Syspiper klíč. Nepřidávají se adresy ze subnetů, rout,
ARP/NDP, konfigurací Syspiper proxy ani výsledků vzdáleného `/interfaces`.
Při známém odlišném network namespace se proces pro automatické cíle vynechá;
pokud `/proc` neumožní namespace přečíst, IP se zkouší z namespace collectoru.
Identické IP se sloučí a UI ukáže jejich zdroj a související procesy. Více IP
jednoho fyzického hostu se zatím neslučuje. Limit je 64 IP včetně localhostu;
přesah se hlásí v UI. Zmizelé automatické cíle při dalším průzkumu vypadnou.

Výchozí `--syspiper-port 8181`, `--syspiper-interval 30` sekund (minimum 5).
Samostatné vlákno se čtyřmi souběžnými sondami neblokuje Tuntom control sockety.
Pomalý průzkum více nedostupných IP může prodloužit skutečný interval; UI ukazuje
čas vzorku a označuje staré výsledky. HTTP používá `X-API-Key`, pouze pevné čtecí
cesty, limit odpovědi 1 MiB a timeouty. Nepoužívá systémový HTTP proxy ani
přesměrování; neposílá klíč na jiné místo podle odpovědi serveru. Přístup je HTTP
v labové síti, stejně jako u ručního dotazu na Syspiper.

V pohledu **Uzly / Syspiper** jsou CPU, RAM, zaplnění kořenového disku a síťové
RX/TX celého hostu. Síťové rychlosti vyžadují dva vzorky `/net`; pokles čítačů,
známá změna boot time nebo dlouhá mezera zruší baseline. U staré verze bez
`/system` nelze spolehlivě poznat reboot, pokud nové čítače už přerostly staré.
Síť zahrnuje všechny hostové interfacové čítače, nejde o provoz samotného Tuntomu.

Rozšířené verze doplní hostname, uptime, load, čítače rozhraní, filesystem/inode
metriky a PSI. Numerický detail je omezený na 128 položek s indikací zkrácení.
Starší servery fungují přes `/cpu`, `/ram`, `/disk`, `/net`; nepodporované
`/system`, `/interfaces`, `/filesystems`, `/pressure` se znovu zkusí po 10 min.
Chybějící data jsou `—`, ne nula; chyba klíče, timeout nebo selhání mají popis.

Kliknutím na hodnotu otevřeš graf (CPU, RAM, disk nebo RX/TX) se stejnými
rozsahy a odečtem jako procesové grafy. Při zapnuté historii se body ukládají do
stejné 24h SQLite cache, včetně mezer a příčin chyb sběru. F5 je obnoví přes
`GET /api/v1/syspiper/{id}/history` se stejnou autentizací a stránkováním jako
procesová historie. IP+port identifikuje sledovaný cíl; historie neslouží jako
identita fyzického stroje, pokud na stejné IP později poběží jiný host.

## Flows a labely (control snapshot)

Podpora `show flows` z commitu `95ef9d7`: v navigaci otevři **Flows a labely**,
vyber proces a stiskni **Načíst flows**. Funguje také přes oddělený collector,
bez `--allow-write`. Po aktualizaci restartuj collector i web a obnov stránku;
daemon musí obsahovat podporu tohoto příkazu.

- Souhrn tabulek a labelů, filtry IP/portů/labelů/kontextu, TCP/UDP/L3,
  řazení podle idle, stránkování po 50 řádcích a export JSON.
- Labely mají přesné 64bitové hodnoty, přepínač DEC/HEX a zachované pořadí
  v zásobníku. Kliknutí na label v souhrnu nastaví filtr.
- L3/L4 cache exit adaptéru ukazuje **návratový směr**. `routes` ukazuje
  dopředný klíč a uchované client/server kontexty DIVERT/VIA včetně path,
  chain, step, origin, cookie, action a reverse. Přesná data jsou rozbalovací.
- Admission tabulky jsou historie učení, nikoli seznam prokazatelně živých
  spojení. `labels=unknown` se liší od prázdného zásobníku.
  `tracking=none` u tunelu/switche neznamená nulový provoz.
- Počet flows je počet řádků; stejná tuple může být ve více tabulkách.
  Souhrn labelů počítá řádky s daným labelem, nikoli opakování v zásobníku.
  Metadata DIVERT body se nepočítají jako labely.

Snapshoty se načítají **pouze ručně**, neukládají se do historie telemetrie.
Velký dump může dočasně zdržet daemon při zpracování paketů. Fabric přijme
nejvýše 8 MiB odpovědi a povolí jeden souběžný dump. Do UI vrací nejvýše
2 000 řádků / 2 MiB dat řádků, souhrny však počítá z celé přijaté odpovědi.
Souhrn ukazuje nejvýše 128 labelů. Zkrácení je viditelně označené;
filtry i JSON export pracují s vrácenou podmnožinou. Pro větší dump použij
`tuntomctl show flows`.

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
- Stáhnout aktuální snapshot jako JSON. Grafy načítají až **24 hodin** z cache
  collectoru, s rozsahy **5m (výchozí), 1h, 12h, 24h**. F5 historii zachová.
  Přepnutí rozsahu historii nemaže; delší pohled zhušťuje vykreslení se zachováním
  minim, špiček a mezer. Collector sbírá i bez otevřené stránky; pauza v UI
  pozastaví jen její obnovování. Po návratu se chybějící vzorky doplní přes API.
  S vypnutou cache (`--no-history`) zůstává pouze historie v paměti stránky.
- Kliknutím na graf RX/TX nebo CPU ukazatel workeru otevřít velký detail.
  Pod kurzorem ukazuje čas a přesnou hodnotu původního vzorku; kliknutím bod
  připneš i přes refresh. Šipky procházejí vzorky, Home/End první/poslední,
  Escape zavírá detail. Rozsah 5m/1h/12h/24h je společný s malým grafem.
  Také historie CPU workerů a příčiny červených bodů se obnoví z cache.
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
| GET | `/api/v1/endpoints/{id}/flows` | Ruční read-only snapshot flows a labelů, souhrny a příznaky zkrácení |
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
history.py    → SQLite cache telemetrie, retence a stránkované čtení
syspiper.py   → oddělené sondy známých IP, normalizace systémových metrik
collector.py  → samostatný sběr, Unix IPC a kontrola UID
server.py     → snapshot, diagnostika, ruční akce, HTTP bez roota
static/       → pohledy nad pozorovaným stavem
```

Další kroky mohou přidat síťová rozhraní/routes, skutečné zaplnění front
(po rozšíření metrik daemonu) nebo další ruční runtime akce. Pro více strojů lze přidat další zdroj
discovery; konkrétní akce vždy patří k identitě nalezeného procesu. Není nutné
kvůli tomu zavádět provisioning ani trvalý inventář. SQLite slouží pouze jako cache telemetrie.

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

### Rozšířená data Syspiperu

Collector dotazuje také `/apt`: počty dostupných aktualizací (`total`, `security`,
`held`, `security_held`) a stáří lokálních APT indexů. Jde pouze o čtení;
nespouští se aktualizace indexů ani instalace balíčků. Staré indexy mohou znamenat
neaktuální počty. Endpoint má delší timeout kvůli APT helperu (12 s na odpověď).
Nepodporovaný APT či chybějící indexy se zobrazí stavem/důvodem, nikoli nulou.

V „Další metriky“ jsou i distribuce, kernel, počty CPU a adresy/stav rozhraní
z již dotazovaných `/system` a `/interfaces`. Adresy z tohoto výpisu se nikdy
nepoužijí jako nové pollingové cíle. Tabulka zachovává limit 128 položek a
upozornění při zkrácení; APT souhrny mají přednost před detaily rozhraní.

Souhrn OS a dostupných aktualizací se zobrazuje pouze v „Uzly / Syspiper“,
z aktualizací zůstává u procesů tunelů v Live pouze počet bezpečnostních aktualizací.
Stáří APT indexů UI nezobrazuje ani v detailních
metrikách: neudává čas poslední instalace aktualizací.

Live sdružuje tunely podle ID: `231s`, `231_1s`, `231_2s` atd.
Skupinu propojuje jemná boční čára. Klientské/serverové konce a různé hosty
či síťové namespaces zůstávají oddělené. CPU hostu, steal a bezpečnostní aktualizace jsou pouze
u první viditelné instance; filtrování souhrn přesune na zbývající první cestu.
Cesty se řadí podle číselného indexu; INFO peer-access přiřazuje metriky
hostu, ale není podmínkou seskupení.
Steal se počítá z rozdílu surových `/system.cpu_times.data` mezi dvěma odběry
Syspiperu (user až steal, bez opětovného započítání guest). Vyžaduje Syspiper
s tímto polem. První vzorek, restart hostu, pokles čítače, dlouhá mezera nebo
chybějící údaj znamenají „—“, nikoli nulu. Zastaralá data se označí a procenta
se skryjí. CPU a steal popisují celý host, nikoli jednu tunelovou instanci.

### Observed topology

Pod Live overview je samostatná mapa lokálních procesů: tunely, switche a
adaptéry/divert. Stabilní pozice, zoom 50/75/100 %, RTT a RX/TX na uzlech,
výběr uzlu s detailem (na menším okně pod mapou). Plná linka znamená port
potvrzený statistikami switche a namespace; přerušovaná vazbu z parametrů.
Pulzování značí provoz procesu, ne trasování konkrétních paketů. Respektuje
reduced-motion. Procesy bez doložené vazby zůstávají samostatné; mapa
nedoplňuje vzdálenou topologii odhadem. Stačí aktualizace statických souborů a F5.

Přehled a mapa rozlišují `TUNEL · DATA` a `TUNEL · IPC`. IPC vychází ze
stats `relay_mode=listen/connect`; úplný vzorek tunelu s `tunnel_id` bez
`relay_mode` označuje DATA režim. Bez dostupných stats je režim `?`.
Neodvozuje se z počtu paketů ani názvu procesu; protokol se nemění.

IPC relay vazby na lokální switch se objevují také přes `--relay-connect`
a `--relay-port-id` (včetně rozlišení mount namespace). Mapa používá zelené
DATA a modrošedé IPC linky s odděleným vedením a jemně zaoblenými koleny.
Po této změně discovery restartuj collector; samotné F5 nové vazby nedoplní.

### Topology rule labels

The topology reads active switch rules while its view is open. Next to verified
tunnel attachments it lists matching source and destination label selectors,
rewrites, destination patterns and via services, including destinations absent
from the map. Collapsed groups combine duplicate rules; hover shows the original
rule and associated ports. Red entries retain matching drops in switch order.
These are declared policy selectors, not a computed effective reachability set:
ordered drops, rewrites, service availability and connected destinations still
determine forwarding. Unknown rules are shown explicitly; editor drafts are never
used. Lists scroll independently and retain their scroll position on refresh.

### VIA tunnel distribution

The collapsible panel above the topology groups verified local IPC tunnel
attachments by active service `client-relay` (IN) and `server-relay` (OUT)
selectors. Legacy `relay` selectors have one combined IN / OUT group. It shows
5-second UDP RX + TX rates in b/s or pps, including transport overhead and any
other services sharing that tunnel. These are tunnel totals, not per-service
traffic attribution or flow counts. Bars share a scale within each service;
percentages are computed per side only when all displayed members have current
measurements. Missing/stale values show a dash. Clicking a row reveals its tunnel
in the map. Active rules are read while the map is open, even with label overlays
hidden, because VIA membership also depends on them.

### Flow snapshot cascade

Flow tables default to SRC → SPORT → DST → DPORT grouping. Four selectors set
the order; selecting a dimension clears duplicates to its right, and dimensions
already used to the left are disabled. None skips a level. SRC and DST each
have independent IPv4/IPv6 prefixes (defaults /24 and /64); skip grouping omits
that address-family level, while /32 and /128 group exact addresses. SPORT and
DPORT independently use ranges of 1, 100, 1000 (default), 10000 or all ports.
IPv4 and IPv6 groups are distinct. Missing ports are distinct from port zero.

Filters apply before grouping. Group counts describe the filtered snapshot,
not a complete flow inventory when the daemon truncated its dump. Leaf rows
preserve original protocol, labels and VIA context; groups are summaries, not
merged flow identities. Pagination covers 50 root entries. Open branches render
50 children at a time with a Show more control. Open paths persist across new
snapshots and are scoped to the process identity and grouping configuration.

Bypass grouping disables cascade/range controls and local sorting, preserving
their choices. It shows snapshot order with explicit filters and pagination
still applied; it does not pause snapshot loading. Grouping is cached per loaded
snapshot and view settings and does not require daemon or protocol changes.

Cascade summaries use the same SRC / SPORT / DST / DPORT columns as individual
flows. Shared addresses, ports, protocol, label stacks and VIA context are shown
immediately; differing values show variant counts. Single-child group chains are
collapsed into one summary and single-flow groups render directly as flow rows.
Expanding reveals the next real branching point or original rows, keeping the
parent summary visible. Label/context differences remain intact at the leaves.

### Scoped flow filters and regular expressions

Flow search accepts a category followed by a space or colon:

- `port 8000-8999`, `sport 32000-32999`, `dport:443`: either/source/destination port; ranges include both bounds.
- `addr 10.20.`, `src 10.20.`, `dst 2001:db8`: either/source/destination address, both IP families. `saddr` and `daddr` are aliases.
- `ip` / `ip4` restrict to IPv4; `ip6` / `addr6` restrict to IPv6. Address and side aliases also accept `4`/`6` suffixes.
- `net`, `snet`, `dnet` match CIDR networks; `4`/`6` suffixes restrict family. Complete hosts default to `/32` (IPv4) or `/128` (IPv6). Incomplete addresses without a mask use text matching, e.g. `snet 10.20.`. Explicit malformed CIDR reports an error.

Enable Regex for expressions such as `sport ^32[0-9]{3}$`. Expressions match individual values, respect category scopes and run in a disposable worker with a time limit. Invalid or excessively expensive expressions report an error without blocking the UI. In Regex mode port patterns are regular expressions; use normal mode for numeric port ranges. Network categories retain CIDR or incomplete-address text semantics. Filters run before cascade grouping and also apply in raw mode.


## Asynchronní čtecí požadavky collectoru (CONTROL v2)

Při `server.py --collector SOCKET` všechny provozní operace webu procházejí
collectorem. Prohlížeč komunikuje pouze s HTTP webem. Samostatný collector
poslouchá jen na lokálním Unix socketu s kontrolou UID, nemá síťové HTTP API.
Bez `--collector` běží stejná sběrná vrstva uvnitř webu.

Unix RPC operace:

- `request_submit`: `id` je identita místního objeveného procesu;
  `body` obsahuje `operation` (`stats`, `flows`, `show` = rules show, `discover`)
  a volitelnou `route` (výchozí prázdná = místní cíl).
- `request_status`: `id` je ID úlohy collectoru vrácené při zadání.

Python klient z adresáře `fabric/`:

```python
from collector import RemoteFabric
client = RemoteFabric("/run/tuntom-fabric-1000.sock")
job = client.submit_request(endpoint_id, {
    "operation": "stats",
    "route": [{"port": "tunnel42"}, {"peer": True}],
})
state = client.request_status(job["id"])
```

Zadání vrací ihned `id`, `state=running`, `request_id=null`. Po potvrzení
od daemonu obsahuje `request_id` jeho CONTROL ID (odlišné od ID úlohy).
Stav přejde na `succeeded` s `result.text`, nebo `failed` s `status` a `error`.
Výsledek lze opakovaně číst; polling nic znovu nespouští. Čtení stavu není
CONTROL `request status` a neobnovuje úlohy po restartu collectoru.

Úlohy běží mimo periodický sběr a RPC obsluhu. Pro jeden cíl smí běžet
jen jeden požadavek, bez čekací fronty; další ihned dostane 429. Cíl je zatím
identifikován dvojicí (ID místního výchozího procesu, zakódovaná cesta).
Různé cesty ke stejnému vzdálenému uzlu zatím nejsou sloučené podle instance ID.
Pomalý cíl tak zabere nejvýše jeden z celkových 16 souběžných slotů.

Cache má nejvýše 64 záznamů, z toho nejvýše 8 pro jeden cíl. Při zaplnění
se nejstarší dokončený výsledek uvolní pro nový požadavek; běžící úlohy
se nevyhazují. Při vyčerpání běžících slotů odpověď 429. Hotové výsledky
expirují nejpozději za 60 sekund; neznámé/expirující/vytlačené ID vrací 404.
Data žijí pouze v paměti. Timeout celé control komunikace je 30 sekund,
odpověď nejvýše 1 MiB (i pro flows, větší dump je odmítnut, nikoli potichu zkrácen).
Ukončení collectoru čeká na dokončení rozběhnutých úloh v rámci timeoutu.

Cesta má nejvýše 16 hopů typu `{"port": "jméno"}` nebo `{"peer": true}`.
Collector nepřijímá libovolný příkaz ani cestu k socketu. Před připojením znovu
ověřuje místní proces a jeho socket. Identita vzdáleného cíle zatím není
svázaná s DISCOVER instance ID: cesta označuje aktuálního příjemce.
Všechny tyto úlohy používají routed CONTROL v2 včetně prázdné cesty;
staré daemony bez podpory vrátí chybu. Vzdálené čtecí příkazy v trusted režimu
vyžadují `--allow-control-trusted` na výchozím daemonu, relays i cíli,
privátní authority klíč na výchozím daemonu a odpovídající veřejný pin s granty
na cíli. Fabric ani collector CONTROL klíče nenačítají; autentizaci provádí daemon.
DISCOVER v trusted režimu provede vlastní challenge na každém navštíveném nodu;
ten potřebuje pin autority s právem read, než odpoví a rozšíří průzkum dál.
Odpovědi FOUND/ALT_PATH mají MAC. Výchozí daemon obsluhuje všechny challenge,
collector ani web nepotřebují klíč. Nejde o nezávislé ověření identity cílových nodů.
`--allow-control-all` je pouze debug / at own risk a obchází kontrolu autority.
Současné rozdělení rolí se ještě může měnit; viz
[CONTROL trust model](../docs/CONTROL_PLANE_SECURITY.md#fabric-and-collector-current-integration)
a [návod ke klíčům](../docs/CONTROL_KEYS_QUICK_HOWTO.md).
DISCOVER vrací původní TSV, stále jde jen
o časově omezený průzkum, nikoli důkaz úplnosti topologie.

Zápisové operace tímto rozhraním zatím nejsou povolené. Dosavadní synchronní
API a kontroly `--allow-write` zůstávají platné.


### DISCOVER v diagnostice

V Diagnostics vyber proces s control socketem a klikni **Prozkoumat síť**.
UI odešle jeden async DISCOVER a čte jeho stav, bez opakovaného zadávání.
Zobrazí cesty, FOUND / ALT_PATH / NO_RESPONSE, komponenty,
capabilities, počet odpovědí a unikátních instancí a čas dokončení.
Výsledek patří vybranému výchozímu procesu, změna výběru jej nepřepíše jinému.
Průzkum se nespouští automaticky; prázdný výsledek neprokazuje starou verzi uzlů.

Autorizované HTTP API: `POST /api/v1/endpoints/{id}/requests` s JSON tělem
stejným jako RPC `body` vrací 202 a úlohu; `GET /api/v1/requests/{job_id}`
vrací její stav. Stav `failed` je výsledek úlohy v HTTP 200, neznámé ID HTTP 404.
UI používá pouze DISCOVER; ostatní čtecí operace jsou dostupné přes API.


### CONTROL stats v UI

Diagnostics zobrazuje všech jedenáct `control_*` konfiguračních údajů
v rozbalovacím detailu, včetně veřejných klíčů, požadované autority, masky
capabilities a levelu. Číselné hodnoty zůstávají přesnými řetězci.

Tlačítko Prozkoumat síť se zobrazuje pouze u dosažitelného procesu s control
socketem a aktuálními stats `control_enabled=1`, `control_discover_enabled=1`
a `control_can_initiate=1`. Samotný příjem DISCOVER nestačí k jeho zahájení.
Chybějící stats staršího daemonu znamenají nezjištěnou možnost, bez tlačítka;
UI ji neodhaduje z argumentů ani z typu komponenty.

Proces s neprázdným seznamem `control_authority_keys` má u názvu štítek
Autorita (seznam procesů, detail, diagnostika a jednotlivé uzly topologie).
Označuje načtený klíč i při vypnutém CONTROL, nikoli zaručené oprávnění ke
všem cílům. Samotné trusted keys ani allow-all tento štítek nezapínají.
Při nedostupných metrikách se štítek i akce skryjí.


## Automatický DISCOVER a polling přes CONTROL

Collector automaticky vybere místní procesy, jejichž aktuální stats povolují
CONTROL, DISCOVER a zahájení požadavků. Přes ně spustí DISCOVER při prvním
nalezení a poté každých **120 sekund**. Tlačítko Obnovit plánuje i nový síťový
průzkum; ruční DISCOVER v Diagnostics také doplní společný přehled.
Privátní klíč zůstává na výchozím daemonu, collector jej nečte.

Vzdálené instance se slučují podle instance ID; jejich stabilní Fabric ID je
`control:<instance>`. Uchovává se nejvýše osm cest na instanci. `self` a známé
přímé lokální přípojky switche ve stejné mount namespace se neduplikují.
Vzdálená položka má u typu malý štítek **discovered**, místo lokálního PID
uvádí CONTROL. Detail ukazuje cesty a čas posledního DISCOVER; PID, UID,
RSS, uptime ani vzdálené /proc údaje se neodhadují.

Statistiky se načítají v samostatných workerech po **5 sekundách** od dokončení
předchozího vzorku. Nejvýše jeden dotaz na instanci, 16 celkem a 4 přes jeden
výchozí proces; žádná čekací fronta. Po chybě roste interval 10/20/40/80/120 s,
zkouší se alternativní cesta; úspěch obnoví 5 s. Lokální sběr tím není blokovaný.
Metriky, delty, grafy a SQLite historie jsou stejné jako u místních komponent.
Vzorek starší než 15 s je nedostupný, nikoli živá nula.

Neúplný průzkum nemaže dřívější nálezy. Po 4 minutách bez DISCOVER potvrzení
má položka stale hint. Cesta se nepoužije po 10 minutách bez potvrzení; položka
zmizí po 10 minutách bez discovery i úspěšné statistiky. Restart s novým instance
ID dostane novou historii. Změna instance na známé cestě vyřadí starou cestu
včetně právě dobíhajícího výsledku. **Současné stats ale neobsahují instance ID**:
výměnu stejného typu procesu na stejné cestě mezi průzkumy nelze spolehlivě
poznat. Cesty nejsou kryptograficky ověřenou identitou vzdáleného stroje.

Limity: 8 výchozích procesů (switche mají přednost), 256 vzdálených instancí,
64 KiB serializovaných metrik na instanci a 2 MiB celkem. Přesah instancí hlásí
`network_discovery.truncated`; chyba cache limitu je viditelná u komponenty.
Metadata síťového sběru a chyby průzkumu jsou v `snapshot.network_discovery`.

Flows a čtení pravidel vzdálené komponenty jsou ruční, sdílejí stejný limit
jednoho aktivního dotazu na instanci (při souběhu 429). Logy, validace a zápisy
pravidel přes vzdálenou položku nejsou dostupné. Automatické čtení pravidel
pro místní topologii nezahrnuje vzdálené switche. Syspiper dál vychází pouze
z lokálního discovery a svých dosavadních pravidel.

The observed map resolves adjacent DISCOVER path prefixes (including verified local switch attachments) and lays out a stable spanning forest rooted at local switches. Alternate paths add edges, not cards; unresolved or ambiguous intermediate hops do not imply a direct connection. Grey dashed CONTROL edges represent discovered reachability and never animate as data traffic.

Discovered tunnel peers can collapse into a visual bundle when adjacent `peer` hops uniquely connect them to members of an already verified local tunnel stack. Names alone never create these bundles; stale, ambiguous and unknown-payload peers remain separate. Expanding preserves every component and its individual detail.

Direct discovered adapter attachments to a peer bundle can also collapse by component type, provided each peer has exactly one adapter attachment and each adapter has one unambiguous parent. These are visual bundles only; stale or ambiguous attachments stay separate.

### Klasifikátor

Pohled **Classifier / Klasifikátor** spravuje L3/L4 klasifikátor DATA tunelů
připojených ke switchi a TUN adaptérů. Zobrazuje stav, generaci, počet pravidel,
hits/misses, chyby parsování/načítání a flushes. VIA a divert komponenty tuto
rodinu CONTROL příkazů nepodporují. U vzdálených tunelů UI vyžaduje aktuálně
hlášené připojení ke switchi; definitivní podporu a oprávnění ověřuje daemon.

API `/api/v1/endpoints/{id}/classifier` vrací aktivní text, SHA-256 a generaci.
POST `/check`, `/load`, `/load-flush`, `/disable` používají rodinu `classifier`
přes lokální socket i routed CONTROL. Check neaktivuje návrh. Zápisy vyžadují
`--allow-write` na webu i kolektoru a odpovídající oprávnění cílového daemonu.
Body zápisu obsahuje `expected_sha256` a řetězec `expected_generation`;
load/load-flush navíc `rules`. Disable neposílá text pravidel.

Každý load znovu validuje návrh. Změna hash/generace proti načtené verzi
zastaví zápis (409). Jde o optimistickou kontrolu, nikoli atomický compare-and-swap
v daemonu: jiný řídicí klient může změnit konfiguraci mezi kontrolou a zápisem.
U routed CONTROL také nadále platí popsané omezení identity cíle mezi DISCOVER koly.

Load zachová cache; load-flush u adaptéru smaže L3/L4 reverse cache a může ovlivnit
existující toky. Disable vypne klasifikátor, cache zachová. UI zobrazuje dopad
před potvrzením; operace se týká jedné komponenty, nikoli celé vizuální skupiny.
Změny jsou runtime-only. Po zápisu nebo nejasném výsledku se musí konfigurace
znovu načíst; UI zápisy automaticky neopakuje. Vypnutý export není validní návrh.

### Managed Services

Pohled **Services** přidává volitelnou sémantickou vrstvu nad živě objevenou
Fabric. Managed Service má jméno, typ, popis, jeden nebo více uint64 labelů a
HTTPS targety pro externí Peek. Jeden label smí vlastnit nejvýše jedna MS; jedna
MS může vlastnit více labelů. Hexadecimální vstup se ukládá v kanonické desítkové
podobě, UI zobrazuje oba tvary.

Pokud se kterýkoliv label MS objeví kdekoliv ve stacku frame nebo flow, je tento
frame/flow asociovaný s danou MS. Jeden stack proto může souviset s více službami.
Pozice labelu nemá význam. UI doplňuje jména MS do Label Topology a flow stacků;
neznámé labely nadále zobrazuje jako původní čísla.

Metadata jsou uložena odděleně ve `--services-db` (výchozí
`$XDG_STATE_HOME/tuntom-fabric/services.sqlite`, jinak
`~/.local/state/tuntom-fabric/services.sqlite`). Nejde o autoritu síťového stavu:
smazání databáze nemění forwarding, aktivní ruleset ani klasifikátory. Fabric pouze
ztratí názvy, vazby a Peek plán a labely znovu ukáže jako neznámé.

Zápisy vyžadují roli `admin`, používají optimistickou generaci a vstupují do
auditu. Smazání MS nikdy samo nemaže ani nepřepisuje runtime classifier. Peek
target musí být HTTPS URL bez credentials a fragmentu; interval je 10..86400 s.
V této iteraci UI targety definuje, ale pravidelné volání vzdáleného Peeku ani
generování classifier rules z MS ještě neprovádí.

U ověřené lokální skupiny DATA tunelů editor implicitně cílí load/load-flush
na všechny její členy. Lze přepnout na jedinou komponentu. Vizuální discovered
peer balíčky samy nedokládají sibling identitu a automatické hromadné zápisy
proto nezapínají. Read/check ukazují aktivní text nebo diff a generaci každého cíle.
Seznam cílů zůstává během kontroly zmrazený; nedostupný člen se tiše nevynechá.
Před první mutací projdou všechny cíle novou validací a kontrolou hash/generace.
Zápisy probíhají postupně, při chybě se zastaví bez automatického retry/rollbacku.
Výsledek zobrazuje applied/error/pending pro každý cíl; nejde o distribuovanou
transakci. Disable nadále zasahuje pouze explicitně vybranou komponentu.

### Události a audit

Pohled **Events & audit / Události a audit** nabízí aktivní problémy, historii
přechodů a audit ručních operací. Sběr událostí běží v kolektoru i bez prohlížeče.
Jde o interpretované události Tuntom komponent, nikoli o syslog nebo sběr všech
textových logů zařízení. Systémové alerty Syspiperu zatím do tohoto proudu nevstupují.

Výchozí trvalé SQLite úložiště je `/var/lib/tuntom-fabric/journal.sqlite` pro root
kolektor a `$XDG_STATE_HOME/tuntom-fabric/journal.sqlite` (jinak
`~/.local/state/tuntom-fabric/journal.sqlite`) pro samostatný web. Přepíšeš jej
`--journal-db`; `--journal-retention-days` má výchozí hodnotu 90. Je nezávislé na
24hodinové telemetry cache a `--no-history` ho nevypíná. Adresář musí mít 0700,
soubory 0600. Obsahuje citlivou historii konfigurací; není kryptograficky
nezměnitelné ani nezávislé na oprávněních správce hostitele.

Audit zaznamenává ověřený účet a roli z HTTP session; sdílený nouzový token je
explicitně `bootstrap (shared)`. Klient nemůže jméno přepsat v API body. Web
předává identitu kolektoru pouze přes Unix socket ověřující UID webu; tento UID
je důvěryhodnou součástí auditní hranice. Tokeny, hesla, verifiery a session klíče
se nezaznamenávají. Diff je obsah konfigurace a může obsahovat její komentáře.

Zaznamenávají se ruční reads/checks/loads klasifikátoru a switchových pravidel,
flows/logs/diagnostics, refresh a asynchronní CONTROL dotazy (včetně dokončení),
přihlášení/odhlášení, správa účtů a zamítnuté přístupy. Automatické statistické
pollingy ani čtení žurnálu nejsou auditní události. Záznamy používají společné
`operation_id`: started → prepared (diff + hashes/generace) → succeeded/failed/unknown.
U async dotazu je navíc submitted. Záznam bez dokončení znamená neznámý výsledek,
například po pádu procesu; nikdy se neinterpretuje jako úspěch. Diff se omezuje
na 64 Ki znaků s explicitním příznakem truncation. Pokud nelze uložit audit před
operací, operace nezačne. Selhání zápisu dokončení už provedenou operaci nevrací;
API a UI upozorní a zápis se nesmí slepě opakovat.

Události zachycují vznik, změnu typu příčiny a odeznění health podmínek, nedostupnost
telemetrie a zmizení komponenty. Opakovaný stejný problém nevytváří novou událost
každý poll. Neznámá nebo chybějící telemetrie neznamená zotavení předchozí chyby.
Stav aktivních podmínek přežije restart. Zmizelé komponenty a historie podléhají
retenci; nejde o trvalý inventář. Zdrojové časy jsou časy detekce na kolektoru.
Po startu kolektoru má detekce zmizení 150 s odklad, aby se nejprve obnovil
DISCOVER inventář. Výslovná nedostupnost telemetrie se tím neodkládá.

GET `/api/v1/journal?category=audit|event` podporuje `before` (ID), přesné filtry
`actor` a `target`. Vrací nejvýše 100 záznamů a 2 MiB na stránku. `active=1`
vrací aktivní podmínky (nejvýše 500, s celkovým počtem). UI má filtr a načítání
starších stránek; při prohlížení otevřeného detailu je samo nepřepisuje.
