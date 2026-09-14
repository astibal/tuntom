# Showcase switch pravidel — format 2

[Kompletní konfigurace](../examples/switch-showcase.rules) je spustitelná na
obou switchích. Nová syntaxe vyžaduje `format 2`; `format 1` zachovává starý
význam oddělených příkazů `switch` a `label`.

```text
původní port + stack
  -> první odpovídající allow: cílová skupina + přepis stacku
  -> předchozí drop pravidla vyřadí zakázané kandidáty
  -> ECMP vybere jeden výstup
  -> EXIT pro exit port, jinak SWITCH
```

Drop bez omezení cíle ukončí zpracování hned při shodě vstupu. Pravidla za
vybraným allow už rozhodnutí nezmění. Nepřipojený cíl ani neúspěšný přepis
nevedou k vyzkoušení dalšího allow.

## 1. Obousměrný přepis prvního labelu

```text
format 2
serial 2026091403
exit inet*

switch H42*, [17, ...] to inet*, [99, ...] allow bidir
```

```text
H42* [17]        <-> inet* [99]
H42* [17, 42, 8] <-> inet* [99, 42, 8]
```

Adaptér si zapamatuje přijatý stack a použije ho pro odpověď z TUN. Návrat do
`H42*` nemusí vybrat stejný konkrétní port jako dopředná cesta; členové skupiny
mají představovat zaměnitelné cesty. `exit inet*` určuje opcode EXIT.

## 2. Služba ve druhém labelu, origin zachovat

```text
switch H42*, [42, 99, ...] to inet*, [42, *, ...] allow bidir
```

```text
[42, 99, 7] -> [42, 99, 7]
[42, 98, 7] -> toto pravidlo nematchuje
```

První label označuje origin, druhý službu a další mohou nést metadata.
Na výstupu `*` kopíruje původní hodnotu na stejné pozici. Pokud má exit sloužit
více origin skupinám, návratové matchování je musí rozlišit.

## 3. Příznaky jako bitmaska

```text
switch flags*, ["FLAG", &0x10, ...] to inet*, ["FLAG", *, ...] allow bidir
```

```text
["FLAG", 16]     MATCH
["FLAG", 17, 7]  MATCH
["FLAG", 48]     MATCH
["FLAG", 8]      bez shody
["FLAG", 32]     bez shody
```

`&0x10` testuje `(label & 16) != 0`. Ostatní bity zůstanou zachované.
`&24` vyžaduje alespoň jeden z bitů 8 a 16, nikoli oba. `&0` nematchuje nic.
Vygenerované zpáteční pravidlo zachová i maskovou podmínku.

## 4. Interval, hex, binární zápis a text

```text
switch qos*, ["QOS", <b1000, 0xf>, ...] to inet*, ["QOS", *, ...] allow bidir
```

Druhý label musí být 8 až 15 včetně. Protože se zachová jeho hodnota,
`bidir` ji může vrátit beze změny.

```text
"ABCD"     = 0x4142434400000000
"ABCDEFGH" = 0x4142434445464748
""         = 0
b1000      = 0x08 = 8
```

Řetězce mají nejvýše 8 bajtů; znaky jsou vlevo a nulové bajty tvoří suffix.
UTF-8 znaky se počítají po bajtech. Export číselné i řetězcové literály
normalizuje do desítkového zápisu. Čísla mohou být i v mezích intervalů a maskách.

## 5. Přesná délka a zkrácení stacku

```text
switch exact*, [42, 99] to exact-out, [42, 99] allow bidir
```

Přijme přesně dva labely. `[42,99,7]` neodpovídá. Samotné `17` je zkratka
pro `[17]`, tedy přesně jeden label. To se liší od starého formátu 1.

```text
switch H43*, [17, ...] to inet*, 100 allow
switch inet*, 100 to H43*, 18 allow
```

```text
H43* [17, 42, 8] -> inet* [100]
H43* [18]        <- inet* [100]
```

Dva explicitní statementy umožní vlastní návratový label. Zahozené labely
adaptér neobnoví. Pokud wildcard, rozsah nebo masku přepíšeš konstantou,
`bidir` takový nejednoznačný návrat odmítne; zapiš opačný směr explicitně.

## 6. Výjimka a blokování před obecným pravidlem

```text
switch H42-block* drop bidir
switch inet*, [99, ...] to H42-test drop
switch H42-test, [17, ...] to diag-exit, [199, ...] allow bidir
switch H42*, [17, ...] to inet*, [99, ...] allow bidir
```

`H42-test` jde na diagnostický port, ostatní členové do internetu.
První dvě pravidla vyřadí z návratového ECMP blokované a diagnostické porty.
Výjimka musí být před obecným allow. Pro diagnostický adaptér přidej
`exit diag-exit`; pro internet `exit inet*`.

## 7. Transparentní předání a vynechané selektory

```text
trunk backbone*
switch transit* to backbone* allow bidir
```

Vynechaný vstupní match přijme jakýkoliv platný stack, vynechaný výstupní
přepis ho celý zachová. `trunk` zachovává SWITCH a ovlivňuje plánování MP.

```text
switch H42* drop                 # přípustný vynechaný cílový selektor
switch [42, &16, ...] to inet* allow  # vynechaný port, zachovat stack
switch *, 17 to inet*, 99 allow  # chyba: výslovné samotné * jako port
```

`H42*` zahrnuje i samotné `H42` a `H420`; `H42_*` vybere jen prefix `H42_`.

## Kontrola a hranice této změny

Z kořene repozitáře proti odpovídajícímu běžícímu switchi:

```bash
./cmake-build-debug/tuntomctl /run/tuntom/switch.control \
  rules check examples/switch-showcase.rules
```

`check` nic neaktivuje. Kontroluje i serial; před použitím zvol vyšší než
aktivní konfigurace. `rules load` aplikuje celý soubor atomicky.

Stack zatím zůstává lokální IPC metadata. UDP V5 přenáší jen payload a tuntom
na druhém konci přidává nakonfigurovaný ingress label. Tato změna nepřidává
výběr stacku podle IP/portů ani dynamickou konfiguraci stacku producentů.
Adaptér si labely zapamatuje, ale například QoS podle nich sám nenastavuje.

[Úplná specifikace formátu 2](SWITCH_RULESET_V2.md)
