# Ascon workeři — naměřeno 2026-09-13

Skutečný produkční Ascon se v tomto mocku vyplatilo paralelizovat. Pro 1400B payloady a směs TX/RX 50:50 dosáhli čtyři workeři s dávkami po osmi přibližně 3,6× propustnosti přímého zpracování. Cena je pět vytížených jader celkem a čekání ve frontách.

Ryzen 7 3700X, GCC, `-O3 -march=native -mtune=native`; koordinátor na CPU 1, workeři postupně na CPU 2–5, vždy jiná fyzická jádra. CPU nejsou izolována od ostatní práce hostitele. Tři opakování každé konfigurace v náhodném pořadí, cílová délka 0,5 s/běh po kalibraci, 4096 zahřívacích operací. Tabulky uvádějí mediány.

## Propustnost směsi TX/RX

Celková kapacita 256 paketů; pracovní dávka 8. Každá operace provede jedno šifrování **nebo** dešifrování. Gbit/s je součet obou směrů, počítaný z payloadu.

| Payload | Přímo: 1 vlákno | 1 worker + main | 2 workeři + main | 4 workeři + main |
|---|---:|---:|---:|---:|
| 64 B | 1.84 Gbit/s | 1.95 Gbit/s | 3.74 Gbit/s | 4.68 Gbit/s |
| 512 B | 3.99 Gbit/s | 4.16 Gbit/s | 8.00 Gbit/s | 12.88 Gbit/s |
| 1400 B | 4.51 Gbit/s | 4.78 Gbit/s | 8.94 Gbit/s | 16.29 Gbit/s |

## Latence a cena CPU pro 1400 B

Latence se měří od přípravy vstupu po převzetí výsledku v pořadí, při saturaci. Není to RTT. CPU zahrnuje i busy polling koordinátoru a workerů.

| Varianta | Kapacita | Gbit/s | Vytížená jádra | CPU ns/paket | p50 µs | p99 µs |
|---|---:|---:|---:|---:|---:|---:|
| Přímo | 1 | 4.51 | 0.96 | 2367 | 2.3 | 3.0 |
| 1 worker, dávka 8 | 256 | 4.78 | 1.96 | 4594 | 595.5 | 959.8 |
| 2 workeři, dávka 8 | 256 | 8.94 | 2.96 | 3722 | 297.3 | 668.7 |
| 4 workeři, dávka 8 | 256 | 16.29 | 4.96 | 3411 | 158.0 | 443.2 |
| 2 workeři, dávka 8 | 32 | 9.25 | 2.97 | 3593 | 36.7 | 54.2 |
| 4 workeři, dávka 8 | 32 | 13.51 | 4.96 | 4112 | 22.4 | 50.3 |

## Co z toho plyne pro další prototyp

- Jeden pomocný worker přidá téměř celé další vytížené jádro a jen malý zisk propustnosti.
- U 64 B a čtyř workerů zvedla dávka 8 propustnost z 2.55 na 4.68 Gbit/s. Přidávat workery bez dávkování se tady nevyplatilo: čtyři workeři s dávkou 1 byli pomalejší než dva.
- Kapacita 256 zvyšuje propustnost čtyř workerů, ale výrazně prodlužuje čekání. Okno 32 je slibnější výchozí bod pro další test s reálným I/O.
- Další integrační prototyp bych začal se dvěma workery, dávkou 8 a celkovou kapacitou 32 paketů. Ascon ani wire formát by se neměnily. Je nutné doplnit životnost klíčů při rekey, sériovou replay kontrolu, reassembly a probouzení bez trvalého busy pollingu.
- Krátké běhy na sdíleném hostiteli dávají orientační výsledky. Zejména p99 kolísá; tabulky jednotlivých běhů a rozsahy propustnosti jsou přiložené. Zde zvolená kapacita a dávkování nejsou důkazem optimální konfigurace.

## Správnost a omezení

225 případů kontroly správnosti prošlo v optimalizované verzi, s AddressSanitizerem + UndefinedBehaviorSanitizerem i s ThreadSanitizerem. Kontroly porovnávají celé payloady a sekvence, ověřují TX peer kodekem, testují poškozené tagy, opakované použití slotů, malé a nebinární kapacity, neúplné dávky a opožděné workery. ASan běžel s `detect_leaks=0`, protože LeakSanitizer v tomto ptrace prostředí odmítl běžet; úniky paměti tedy tímto nástrojem ověřené nejsou.

Jde o kodek a fronty v paměti, bez TUN, UDP, IPC, replay, fragmentace/reassembly, handshaku a rekey. RX opakuje připravený korpus ciphertextů s opakovanými sekvencemi; nepředstírá živou session. Buffery jsou znovu používané a mohou být v cache. Ve směsi se drží jedno společné pořadí TX/RX, přísnější než samostatné pořadí pro každý směr. Z naměřených Gbit/s nelze přímo odvodit propustnost síťového tunelu.

## Reprodukce a data

- [Postup a implementace](README.md)
- [Celá hlavní tabulka](results/2026-09-13-main.md) · [raw JSON](results/2026-09-13-main.json)
- [Kapacita 32](results/2026-09-13-window32.md) · [raw JSON](results/2026-09-13-window32.json)

```bash
bash experiments/ascon_workers/build.sh
python3 -B experiments/ascon_workers/check.py
python3 -B experiments/ascon_workers/bench.py --output /tmp/ascon-main.json \
  --cpus 1,2,3,4,5 --seconds .5 --repeat 3
python3 -B experiments/ascon_workers/bench.py --output /tmp/ascon-window32.json \
  --cpus 1,2,3,4,5 --seconds .5 --repeat 3 --sizes 1400 --modes mixed --slots 32
```

Měřený git základ: `ca1ba193d8675140f4762960be5ff218b8a77985`. Experiment je přidaný mimo produkční cestu; přesné hashe zdrojů a binárky jsou v JSON.
