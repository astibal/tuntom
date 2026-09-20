# Persistentní instalace Tuntomu

`mk_*` slouží k vyzkoušení sestavy. `deploy/` z ní udělá samostatné služby
systemd a později je spravuje přes Ansible. Zdrojové skripty, `src/` ani build
hlavního projektu se nemění. Nová závislost na Ansible patří pouze do `deploy/`.

```text
tvůj počítač                         jednotlivé stroje
privátní inventář                    privátní instalace každé sestavy
       |                                      ^
       +--> tt_deploy.sh --> Ansible / SSH ----+
                                              |
                                           systemd
                                              |
                                      skutečné procesy Tuntomu
```

Po instalaci stačí na každém stroji systemd. Ansible ani SSH spojení ke klientovi
nejsou potřeba k běhu služby nebo k jejímu startu po rebootu.

## Příprava

Na řídicím počítači:

```bash
python3 -m venv deploy/.venv
deploy/.venv/bin/pip install -r deploy/requirements.txt
export TUNTOM_DEPLOY_PYTHON="$PWD/deploy/.venv/bin/python"
```

Na cílových strojích jsou potřeba Python 3, systemd, `g++`, Bash, `iproute2`,
`iptables`, `tar`, `flock` a přístup roota přes SSH nebo sudo. Balíčky se samy
neinstalují. Účet a skupina `tuntom` se vytvoří, pokud chybí; existující účet se
nepřestavuje. Řídicí Python musí odpovídat požadavkům `ansible-core` z requirements.

Cílové platformy jsou Ubuntu 26.04 LTS a Debian 13. Používáme společné rozhraní
systemd a standardní moduly Ansible, bez zvláštních větví pro distribuce.
Automatické testy níže nenahrazují zkoušku bootu a síťových hooků na těchto OS.

## Z vyzkoušeného tunelu na službu

Použij stejné ID, remote, přepínače a proměnné jako při ověřeném spuštění
`mk_tunnel.sh`. Nové jméno určuje celou spravovanou sestavu:

```bash
export TUNTOM_MTU=9000
# TUNTOM_SECRET už máš nastavený stejně jako při testu mk_tunnel.sh.
deploy/tt_tunnel.sh 42 root@psx2 --name psx2 --count 4 --prepare-only
deploy/tt_deploy.sh --tunnel psx2 --install --check --diff
deploy/tt_deploy.sh --tunnel psx2 --install
unset TUNTOM_SECRET
```

`--prepare-only` vytvoří pouze centrální definici. Bez něj `tt_tunnel.sh` rovnou
naváže instalací. Jeho `--check` vytvoří definici a vypíše plán, takže není zcela
bezzápisový; následné `tt_deploy.sh --check` je pouze čtení.

Importer přijímá přepínače `mk_tunnel.sh`: `--count`, `--crypto-auth-only`,
`--no-address`, `--no-stats`, `--all-tools`, `--snat` / `--no-snat`,
`--mss-clamp` / `--no-mss-clamp`, obě varianty `--client-*` a `--server-*`
pro switch, exit node, IPC, batch a classifier. Přenese i nastavení MTU,
transport MTU, prefixu, mark/table/chain a hooků z odpovídajících `TUNTOM_*`
proměnných. Chybějící `--count` převezme z uloženého lokálního `mk_*` stavu.
`--stop` je u importeru pouze zkratka pro zastavení lokálního člena `42c`.

Nejprve se na **všech** vybraných strojích zkompilují a připraví nové soubory.
Teprve potom se ověřená sestava `mk_tunnel.sh` zastaví jeho původním `--stop`,
který zná její uložené hooky. Převzetí prováděj z původního klienta. Pokud se
uložené prostředky neshodují, nasazení odmítne jejich přepsání.
Zbytky převzaté zkoušky (její binárky, logy a stav) přesune do `retired-mk/`
uvnitř instalace. Tím spadají pod její budoucí wipe. Sdílené `mk_*` nástroje
a synchronizační locky zůstávají.

## Běžné operace

V příkladech `psx2` znamená **celou jednu sestavu** tunelů: obě strany a všechny
její členy. `42_1c` je jeden konkrétní proces na jednom stroji.

| Příkaz | Co udělá |
|---|---|
| `deploy/tt_deploy.sh --info` | Přehled celého inventáře, dostupnosti, služeb, souborů a změn. |
| `deploy/tt_deploy.sh --tunnel psx2 --info` | Podrobný stav pouze `psx2`; hodnoty secrets nevypisuje. |
| `deploy/tt_deploy.sh --tunnel psx2 --update` | Znovu sestaví binárky. Při změně je vymění a obnoví jen dříve běžící procesy. |
| `deploy/tt_deploy.sh --tunnel psx2 --update-config` | Aktualizuje binárky, spravovanou konfiguraci, hooky, credentials a definice unit. |
| `deploy/tt_deploy.sh --host local --instance tunnel:42_1c --stop` | Pouze `systemctl stop` tohoto procesu. Autostart nemění. |
| `deploy/tt_deploy.sh --host local --instance tunnel:42_1c --start` | Spustí tento proces. |
| `deploy/tt_deploy.sh --host local --instance tunnel:42_1c --restart` | Restartuje tento proces. |
| `deploy/tt_deploy.sh --tunnel psx2 --restart` | Restartuje celou sestavu včetně skupinových hooků. |
| `deploy/tt_deploy.sh --tunnel psx2 --disable` | Vypne autostart; běžící procesy nezastaví. |
| `deploy/tt_deploy.sh --tunnel psx2 --enable` | Zapne autostart; neběžící procesy nespustí. |
| `deploy/tt_deploy.sh --tunnel psx2 --wipe` | Vypíše rozsah, vyžádá `ano` a nenávratně odstraní vybranou sestavu. |

`--stop` / `--start` bez `--instance` operují s cílem celé sestavy. Jednotlivě
pozastavený proces obnov pomocí jeho `--instance --start`, případně restartem
celé sestavy. `--update` a `--update-config` zachovávají zastavené členy i autostart.
Noví členové přidaní přes `--update-config` se spustí, pokud byl cíl sestavy aktivní.
Nezměněné binárky nevyvolají restart. `--update-config` sestavu dočasně odstaví
i při opakování stejné konfigurace, aby znovu aplikoval síťové hooky.

Pro hromadnou změnu je nutné výslovné `--all`. `--check` před kteroukoli změnou
vypíše plán bez buildů, hooků a zásahů do služeb. `--diff` ukáže veřejnou
konfiguraci; není náhradou kontroly obsahu vlastních hooků. Pro sudo s heslem
slouží `--ask-become-pass` (heslo může být potřeba zadat v několika fázích).

## Inventář, ve kterém se dá ručně orientovat

Výchozí adresář je `~/.config/tuntom/inventory`. Změníš ho přes
`TUNTOM_INVENTORY` nebo `--inventory /cesta`. Skutečný inventář drž mimo Git.
Součástí repozitáře jsou jen nefunkční příklady bez klíčů v `deploy/examples/`.

```text
inventory/
├── hosts.yml                       spojení na stroje
├── instances/
│   ├── tunnels/
│   │   └── psx2/
│   │       ├── instance.yml        definice této sestavy
│   │       ├── hooks/              její vlastní skripty
│   │       ├── files/
│   │       │   ├── client/         její lokální konfigurace
│   │       │   └── server/         její vzdálená konfigurace
│   │       └── secrets/
│   │           └── master.key     32 hex znaků, režim 0600
│   ├── switches/fabric/instance.yml
│   └── adapters/exit0/instance.yml
└── .state/tunnel-psx2/identity.json vlastník a identita instalovaných strojů
```

`hosts.yml` je obyčejný inventář Ansible:

```yaml
all:
  hosts:
    local:
      ansible_connection: local
      ansible_become: true
    psx2:
      ansible_host: 192.0.2.2
      ansible_user: root
```

`instance.yml` je obyčejná YAML konfigurace, která se sama nevykonává:

```yaml
tuntom_instance:
  schema: 1
  tunnel_id: 42
  count: 4
  mtu: 9000
  transport_mtu: 1400
  prefix16: '10.254'
  no_address: false
  crypto_auth_only: false
  secret_file: secrets/master.key
  client:
    host: local
    peer_address: 192.0.2.2
  server:
    host: psx2
```

`client.host` a `server.host` odkazují na jména v `hosts.yml`.
`peer_address` je datová adresa serveru; může se lišit od SSH adresy.
Pro obě strany na témže počítači použij stejné jméno hosta, ne dva aliasy.
Jméno `localhost` je rezervované pro interní práci Ansible; použij `local`.

Volitelné `pre_hook`, `post_hook`, `group_pre_hook`, `group_post_hook` jsou cesty
relativní vůči adresáři instance. `client` a `server` mohou mít vlastní pre/post
hook. Soubory ani secrets nesmějí odkazovat symlinkem mimo tento adresář.
`files/` se kopíruje spolu s konfigurací. Hook dostane jeho cestu v
`TUNTOM_FILES_DIR`; ostatní absolutní cesty uvnitř tvého skriptu importer nepřepisuje.

Napojení na switch patří pod příslušnou stranu:

```yaml
  client:
    host: local
    peer_address: 192.0.2.2
    switch_socket: /run/tuntom/fabric.sock
    port_id: psx2
    label: 42
    ipc: auto
    ipc_batch: 8
    exit_node: false
    classifier_file: files/client/classifier.rules
```

`exit_node: false` se switchem nevytváří TUN. Při více členech dostanou porty
přípony `_1`, `_2`, … stejně jako v `mk_tunnel.sh`. ID je 1–255, počet 1–64.
Vlastní `mark`, `mask`, `table` lze použít pouze pro jednoho člena.

Samostatný switch nebo adaptér používá `--deployment switch:fabric`, případně
`--deployment adapter:exit0`. Jejich konfigurace najdeš v příkladech.
Switch podporuje `implementation: single` / `mp`, `rules_file` (včetně původního
jednoduchého formátu), `auto_pool` a `reserve_cpus`; `options` obsahuje známé
číselné/IPC přepínače bez úvodních `--`. Cesty a identita mají vlastní YAML pole.
`mk_switch*` a `mk_adapter.sh` dál fungují samostatně.

## Nouzová ruční úprava

**Trvalá změna:** uprav `instance.yml` nebo soubory této instance, podívej se na
`--update-config --check --diff`, potom spusť `--update-config`.
Samotné uložení souboru nic na strojích nepřepíše.

**Okamžitá oprava na stroji:** instalace je zde:

```text
/var/lib/tuntom-deploy/instances/tunnel-psx2/
├── installed.json           evidence vlastnictví a poslední nasazené verze
├── config.json              skutečné argumenty a kontext procesů
├── manifest.tsv             popis členů pro skupinové hooky
├── bin/                     main, tuntomctl, případně další nástroje
├── assets/                  kopie centrálních veřejných souborů
├── secrets/                 privátní klíč
├── logs/                    logy procesů a hooků
├── active/                  původní hooky pro korektní zastavení
├── runtime.py               obsluha startu/stopu
└── tuntom-net.sh             kopie stávající síťové knihovny

/run/tuntom-deploy/tunnel-psx2/       PID, control a statistiky
/etc/systemd/system/tuntom-tunnel-psx2*.service
/etc/systemd/system/tuntom-tunnel-psx2.target
```

Můžeš upravit `assets/` nebo po pečlivé kontrole `config.json` a restartovat
příslušnou službu. Změna `assets/instance.yml` sama nepřegeneruje `config.json`.
`installed.json`, `.state/identity.json` a `active/` nejsou určeny k ruční editaci.
`--info` ukáže odchylky; `--update` je zachová. `--update-config` nahradí spravované
soubory centrální verzí, takže nouzovou opravu nejdříve přenes i do inventáře.

```bash
sudo systemctl status tuntom-tunnel-psx2-42c.service
sudo tail -f /var/lib/tuntom-deploy/instances/tunnel-psx2/logs/42c.log
sudo /var/lib/tuntom-deploy/instances/tunnel-psx2/bin/tuntomctl \
  /run/tuntom-deploy/tunnel-psx2/42c.control show stats
```

Systemd hlídá skutečný proces; pomocný Python se při startu nahradí binárkou.
Pád restartuje příslušného člena. Skupinové hooky se při jeho samostatném
restartu nespouštějí. Při startu celé sestavy běží group-pre, členové, group-post;
při zastavení opačně. Původní hooky a manifest zůstávají uložené pro teardown.
Hooky musí být opakovatelné a musí po sobě uklidit prostředky, které vytvořily.

## Co přesně znamená wipe

`--wipe --tunnel psx2` maže **jen `psx2`**. Po potvrzení odstraní jeho služby,
drop-iny, celé privátní adresáře včetně ručních souborů, secrets, logů,
nedokončeného stagingu, centrální definice i centrálního stavu. Jiný tunel nebo
sdílený switch zůstane. Účet `tuntom`, nástroje systému a sdílené host records
zůstávají; nesdílené host records, které importer založil pro právě mazanou
instanci, se odstraní.

Před mazáním se ověří **všechny** vybrané stroje. Nedostupný remote znamená
žádné mazání. Jediná výjimka je explicitní:

```bash
deploy/tt_deploy.sh --tunnel psx2 --wipe --local
```

Tím smažeš lokální část i centrální konfiguraci a klíče; remote zůstane bez
centrální evidence. Je to úmyslné opuštění vzdálené instalace, ne odložený úklid.
`--local` pozná jen hosty s `ansible_connection: local`.

Pokud spojení spadne až během mazání, už provedené kroky nelze vrátit.
Centrální definice se proto maže poslední a zůstane při neúspěchu zachovaná pro
opakování. Wipe se neptá na jednotlivé soubory a nemá `--yes` ani rollback.
Jeho potvrzení musí přijít z interaktivního terminálu.

Mazání nezasahuje do externích záloh, historie Gitu, původních sdílených hooků
v `/etc/tuntom`, auditních záznamů systému ani libovolných cest vytvořených
vlastním hookem. Není to fyzické přepsání bloků SSD. Proto inventář nepatří do
Gitu a hook musí vlastní síťové prostředky odstranit při `down`.

## Přerušení a první verze

Operace jsou zamčené v inventáři a na strojích. Po tvrdém přerušení může zůstat
`/run/tuntom-deploy.lock`. Nejdříve ověř, že žádné nasazení neběží; teprve potom
odstraň tento lock na dotčeném stroji. Staré privátní stagingy zobrazí `--info`
a odstraní `--wipe`. Neúplnou instalaci můžeš opravit přes `--update-config`
a následně ji podle potřeby zapnout/spustit.

První verze nemá automatický rollback ani distribuovanou transakci. Úspěšná
kompilace na všech strojích předchází stopu; chyba během aktivace může přesto
zanechat částečně změněnou sestavu. Správní SSH spojení veď nezávisle na tunelu,
který právě aktualizuješ. Změna stroje nebo `tunnel_id` je nová instalace,
nikoli aktualizace stávající identity.

## Ověření

```bash
deploy/.venv/bin/python -m unittest discover -s deploy/tests -p 'test_*.py' -v
deploy/.venv/bin/python deploy/tests/integration.py "$PWD/deploy/.venv/bin/python"
```

První sada ověřuje validaci, vlastnictví, rozsah wipe, zachování konfigurace,
obsluhu hooků a jednotky přes `systemd-analyze verify`. Druhá používá skutečné
Ansible v izolovaném Bubblewrap prostředí s dočasnými systémovými adresáři;
kompilátor a systemd jsou testovací náhrady. Potřebuje povolené user namespaces.
Neprovádí síťový test na skutečných serverech ani test rebootu.

`deploy/tests/smoke.py /adresar/s/binarkami` navíc testuje skutečné varianty
switche, control sockety a start/stop hooky pod běžným uživatelem. Očekává
binárky `main`, `adapter`, `switch`, `switch-mp`, `tuntomctl` a `planner`, sestavené
ze stejných zdrojů jako nasazení. Používá jen dočasné soubory a Unix sockety.
