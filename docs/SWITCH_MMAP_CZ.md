# Jak funguje mmap ve switch IPC V2

V2 zachovává socket jako jedinou cestu, která určuje pořadí paketů. Mění se to,
co přes socket přenášíme: buď celý rámec jako dosud, nebo odkaz na rámec ve
sdílené paměti. Přesný formát je v [protokolu V2](SWITCH_PROTOCOL_V2.md).
Naměřené přínosy i omezení jsou v [reportu implementace](../experiments/switch_mp/RESULTS_V2_2026-09-13.md).

## Co je v tomto případě mmap

Switch vytvoří pomocí `memfd_create` anonymní soubor, který nemá cestu v běžném
filesystemu. Připraví jeho velikost a přes socket předá klientovi jeho file
descriptor pomocí `SCM_RIGHTS`. Oba procesy si soubor namapují pomocí
`mmap(MAP_SHARED)`.

```
proces tuntom                  proces switch
virtuální adresa A              virtuální adresa B
       |                              |
       +------ stejné RAM stránky ----+
```

Adresy A a B mohou být různé. Proto do socketu **neposíláme pointer**. Posíláme
číslo poolu, číslo slotu, délku rámce a generační token. Každý proces si svou
adresu slotu spočítá z vlastní adresy mapování a ověřené geometrie.

Každé spojení má dva pooly:

```
pool 1: tuntom/adaptér zapisuje  -> switch čte
pool 2: switch zapisuje         -> tuntom/adaptér čte
```

V každém směru je právě jeden producent a jeden konzument. Je to stejný důvod,
proč se nám uvnitř switche hodí SPSC fronty. Přesun role na jiný worker probíhá
pod existující bariérou; dva workery současně stejný směr neobsluhují.

## Kdy je slot volný

Na začátku slotu je zarovnané atomické 64bitové číslo. Sudá hodnota znamená
FREE, lichá READY. Zároveň jde o generaci konkrétního slotu:

```
0 FREE -> zápis rámce -> 1 READY -> zkopírování rámce -> 2 FREE
2 FREE -> další rámec -> 3 READY -> zkopírování rámce -> 4 FREE
```

1. Producent najde sudý token a zapíše rámec za hlavičku slotu.
2. Atomickým `release` zápisem zveřejní lichý token.
3. Přes socket odešle odkaz s tímto přesným tokenem.
4. Konzument přijme odkaz, ověří jeho hranice a `acquire` čtením zkontroluje token.
5. Zkopíruje rámec do svého privátního bufferu.
6. Až potom zapíše další sudý token a slot uvolní.

`release/acquire` zajišťuje, že příjemce uvidí dokončený zápis rámce a producent
nezačne slot přepisovat před dokončením čtení. Token chrání také před duplicitním
nebo opožděným odkazem na už použitý slot. Generace se nesmí přetočit.

Příjemce samotné READY sloty neprohledává. Čte jen sloty, ke kterým dostal odkaz
přes socket. Právě tím zůstává jednoznačné pořadí i při střídání mmap a inline.

## Kam se kopíruje paket

```
tuntom                  switch                         adaptér
privátní paket
   | memcpy
   v
sdílený slot A --------> privátní RX buffer
             memcpy         |
                            | lookup / úprava labelu
                            | pointer přes RX-TX matrix
                            v
                        sdílený slot B ----------------> privátní buffer
                           memcpy              memcpy       |
                                                            v
                                                           TUN
```

Sdílený pool A a B patří jiným spojením. Adaptér nikdy nedostává přístup do
příchozího poolu tunelu. Switch mění label až ve svém privátním bufferu.

Pořád jsou to dvě kopie na jeden IPC hop, tedy čtyři přes tento celý switchový
úsek. Přínosem jsou menší socketové záznamy, menší práce s kernelovými payload
buffery a hlavně možnost poslat více odkazů jediným syscallem. Není to zero-copy.

## Jak dávkujeme bez čekání na dávku

Běžný limit je 8 rámců, maximum 16 musí povolit obě strany. Jeden BATCH záznam
má 24 bajtů společné hlavičky a 16 bajtů na rámec:

```
1 rámec:   40 B socketem + payload v mmap
8 rámců:  152 B socketem + 8 samostatných slotů
16 rámců: 280 B socketem + 16 samostatných slotů
```

Tuntom a adaptér během stávajícího omezeného průchodu připravenými vstupy zapisují
rovnou do mmap slotů. Plná dávka se odešle ihned, neúplná na konci průchodu, vždy
před usnutím. Žádný timer ani čekání na osmý paket nepřibyly. Při malé zátěži má
průchod často jediný rámec, takže dávka je přirozeně velikosti 1.

MP TX projde vstupní fronty s RR začátkem, vezme nejvýše jeden pointer z každé
a odešle hotovou dávku. Kvóta fronty se nezvětšila. Výstup jediného toku s jedinou
vstupní frontou proto může mít pořád jeden rámec na záznam. Limit 8 sám o sobě
neznamená osmkrát méně syscallů na celém switchi.

## Co při plném poolu nebo socketu

Plný mmap pool neznamená čekání: paket může jít inline přes stejný socket.
Předchozí připravené odkazy se nejprve odešlou, aby je inline paket nepředběhl.
Totéž platí pro rámce větší než mmap slot.

`EAGAIN` při odeslání dávky znamená, že se nepřijal žádný její odkaz. Rezervované
sloty se vrátí do FREE. MP TX si ponechá původní privátní buffery a čeká na
`EPOLLOUT`. Po úspěchu je neopakuje. Tuntom a adaptér zachovávají původní politiku
zahazování při backpressure; jejich počitadla rozlišují skutečně odeslané rámce
od těch, které byly jen připravené v lokální dávce.

Po přijetí socketového BATCH záznamu může zbývat například sedm neobsloužených
odkazů. Socket už přitom může být prázdný. Proto existuje `receive_pending()`:

```
práce připravena = socket je readable NEBO transport má neobsloužené odkazy
```

Bez této podmínky by poslední pakety dávky mohly čekat až na další provoz.
Stejnou informaci obnovujeme při přesunu RX úlohy na jiný worker.

## Životnost a kompatibilita

Nový klient se nejprve nepojmenovaným HELLO zeptá na V2. Starý switch ho odmítne,
klient pak jednou zkusí původní V1 registraci. Starý klient na novém MP switchi
žádné nové zprávy ani FD nedostane. Síťový protokol V5 a význam labelů se nemění.

Při náhradě stejného portu zůstává stará instance funkční během přípravy poolů,
mapování na klientovi a přípravy nového plánu workerů. Až úspěšné odeslání ACTIVE
umožní publikovat nový port pod bariérou. Chyba před tímto bodem starý port
nenahradí. Vše se musí vejít do původního pětisekundového deadline.

Pooly mají zapečetěnou velikost: druhá strana je nemůže zkrátit ani zvětšit.
Jejich obsah ale může měnit, takže geometrii uchováváme privátně a payload nejdřív
kopírujeme, teprve pak dekódujeme. Po odpojení se mapování zruší; soubor zanikne,
až zmizí poslední reference. Aktivní spojení už nemusí držet memfd descriptor.

## Orientace v kódu

| Soubor | Co v něm hledat |
|---|---|
| [switch_v2.hpp](../src/ipc/switch_v2.hpp) | typy zpráv, capability bity, encode/decode a limity |
| [switch_mmap.hpp](../src/ipc/switch_mmap.hpp) | memfd, seals, mmap, předání FD a atomické tokeny |
| [switch_transport.hpp](../src/ipc/switch_transport.hpp) | `send_batch`, `append`/`flush`, inline fallback a rozpracované RX odkazy |
| [switch_handshake.hpp](../src/ipc/switch_handshake.hpp) | neblokující serverový handshake |
| [switch_client.hpp](../src/switch_client.hpp) | klientský handshake, V1 fallback a API používané tunely/adaptérem |
| [runtime.hpp](../src/switch_mp/runtime.hpp) | privátní RX buffery, dávka TX pointerů, RR a migrace |
| [main.cpp](../src/switch_mp/main.cpp) | admission, paměťový limit a okamžik publikování portu |

Výchozí mmap slot pojme kompletní rámec do 16 KiB, tedy i běžný 9000B jumbo
payload s labely. 128 slotů v každém směru stojí asi 4 MiB na spojení. Rozpočet
256 MiB zahrnuje aktivní i připravované pooly; při jeho vyčerpání se nové spojení
dohodne na inline. Podrobné volby a statistiky jsou v [protokolu V2](SWITCH_PROTOCOL_V2.md).
