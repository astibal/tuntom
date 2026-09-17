"use strict";

// Each entry is [Czech, English, French]. Only presentation is localized;
// process identities, metric keys, rule syntax and exported JSON stay intact.
const languages = {cs:{index:0,locale:"cs-CZ"}, en:{index:1,locale:"en-GB"}, fr:{index:2,locale:"fr-FR"}};
const messages = {
  title:["Tuntom Fabric · Živý přehled","Tuntom Fabric · Live observer","Tuntom Fabric · Vue en direct"],
  workspace:["Pracovní prostor","Workspace","Espace de travail"],
  views:["Zobrazení","Views","Vues"],
  language:["Jazyk rozhraní","Interface language","Langue de l’interface"],
  overview:["Živý přehled","Live overview","Vue en direct"],
  metrics:["Metriky","Metrics","Métriques"],
  rules:["Pravidla switche","Switch rules","Règles du switch"],
  host:["POZOROVANÝ STROJ","OBSERVED HOST","MACHINE OBSERVÉE"],
  stateless:["BEZ STAVU","STATELESS","SANS ÉTAT"],
  currentProcesses:["Aktuální procesy.","Current processes.","Processus actuels."],
  currentConfig:["Aktuální konfigurace.","Current configuration.","Configuration actuelle."],
  noInventory:["Bez inventáře.","No inventory.","Sans inventaire."],
  observing:["POZOROVÁNÍ","OBSERVATION","OBSERVATION"],
  writeEnabled:["RUČNÍ ZÁPIS POVOLEN","MANUAL WRITES ENABLED","ÉCRITURE MANUELLE ACTIVÉE"],
  loginEyebrow:["PŘIPOJENÍ K OBSERVERU","CONNECT TO THE OBSERVER","CONNEXION À L’OBSERVATEUR"],
  loginTitle:["Otevři odkaz z terminálu","Open the link from your terminal","Ouvre le lien du terminal"],
  run:["Spusť","Run","Exécute"],
  loginInstructions:["a otevři vypsanou adresu. Přístupový token můžeš vložit i sem.","and open the printed URL. You can also enter the access token here.","et ouvre l’URL affichée. Tu peux aussi saisir le jeton d’accès ici."],
  accessToken:["Přístupový token","Access token","Jeton d’accès"],
  connect:["Připojit","Connect","Se connecter"],
  explorer:["ŽIVÝ PRŮZKUM PROCESŮ","LIVE RUNTIME EXPLORER","EXPLORATEUR DE PROCESSUS"],
  heading:["Běžící fabric","Live fabric","Fabric en direct"],
  subtitle:["Co právě běží, jak je to propojené a co se děje uvnitř.","What’s running, how it’s connected and what’s happening inside.","Ce qui tourne, comment tout est connecté et ce qui se passe à l’intérieur."],
  pause:["Ⅱ Pozastavit","Ⅱ Pause","Ⅱ Suspendre"],
  resume:["▶ Pokračovat","▶ Resume","▶ Reprendre"],
  refresh:["↻ Obnovit","↻ Refresh","↻ Actualiser"],
  loadingProcesses:["Načítám procesy…","Loading processes…","Chargement des processus…"],
  summary:["Souhrn","Summary","Résumé"],
  running:["BĚŽÍCÍ PROCESY","RUNNING PROCESSES","PROCESSUS ACTIFS"],
  waitingScan:["Čekám na scan…","Waiting for a scan…","En attente d’une analyse…"],
  metricsAvailable:["DOSTUPNÉ METRIKY","AVAILABLE METRICS","MÉTRIQUES DISPONIBLES"],
  controlReply:["Odpověď control socketu","Control socket response","Réponse du socket de contrôle"],
  established:["NAVÁZANÉ TUNELY","ESTABLISHED TUNNELS","TUNNELS ÉTABLIS"],
  readyFound:["Session ready / nalezené tunely","Session ready / discovered tunnels","Sessions établies / tunnels détectés"],
  registered:["REGISTROVANÉ PORTY","REGISTERED PORTS","PORTS ENREGISTRÉS"],
  switchSum:["Součet dostupných switchů","Total across available switches","Total des switches accessibles"],
  processes:["Procesy","Processes","Processus"],
  selectDetails:["Výběrem procesu otevřeš podrobnosti.","Select a process to see its details.","Sélectionne un processus pour voir ses détails."],
  processType:["Typ procesu","Process type","Type de processus"],
  allTypes:["Všechny typy","All types","Tous les types"],
  tunnels:["Tunely","Tunnels","Tunnels"],
  switches:["Switche","Switches","Switches"],
  adapters:["Adaptéry","Adapters","Adaptateurs"],
  searchProcess:["Hledat proces","Search processes","Rechercher un processus"],
  searchPlaceholder:["Hledat jméno, PID, rozhraní…","Search name, PID, interface…","Nom, PID, interface…"],
  processPID:["Proces / PID","Process / PID","Processus / PID"],
  type:["Typ","Type","Type"],
  telemetry:["Telemetrie","Telemetry","Télémétrie"],
  uptime:["Běží","Uptime","Durée d’activité"],
  noProcesses:["Žádný proces Tuntomu","No Tuntom processes","Aucun processus Tuntom"],
  startProcesses:["Spusť tunel, switch nebo adaptér na tomto stroji. Při příští obnově se objeví automaticky.","Start a tunnel, switch or adapter on this host. It will appear automatically on the next refresh.","Démarre un tunnel, un switch ou un adaptateur sur cette machine. Il apparaîtra à la prochaine actualisation."],
  noMatches:["Filtru neodpovídá žádný proces","No processes match the filter","Aucun processus ne correspond au filtre"],
  changeFilter:["Změň typ procesu nebo hledaný výraz.","Change the process type or search term.","Modifie le type de processus ou le terme recherché."],
  selectedProcess:["VYBRANÝ PROCES","SELECTED PROCESS","PROCESSUS SÉLECTIONNÉ"],
  chooseProcess:["Vyber proces","Select a process","Sélectionne un processus"],
  appearAfterStart:["Procesy se objeví po jejich spuštění.","Processes will appear when they start.","Les processus apparaîtront à leur démarrage."],
  throughput:["Propustnost","Throughput","Débit"],
  fiveSecondAverage:["Vybraný proces · pětisekundové průměry","Selected process · five-second averages","Processus sélectionné · moyennes sur cinq secondes"],
  chartLabel:["Historie RX a TX vybraného procesu","RX and TX history for the selected process","Historique RX et TX du processus sélectionné"],
  chartCollecting:["Historie se sbírá po dobu otevření stránky.","History is collected while this page is open.","L’historique est collecté tant que cette page est ouverte."],
  chartUnavailable:["Tento proces zatím neposkytuje metriky propustnosti.","This process does not yet provide throughput metrics.","Ce processus ne fournit pas encore de métriques de débit."],
  chartHistory:["Historie v SQLite cache collectoru · až 24 h · F5 ji zachová. Mezery značí chybějící vzorky.","Collector SQLite cache · up to 24 h · retained across reloads. Gaps mean missing samples.","Cache SQLite du collecteur · jusqu’à 24 h · conservé après rechargement. Les lacunes indiquent des échantillons manquants."],
  historyMemory:["Historie pouze v paměti stránky · F5 ji smaže. Collector nemá zapnutou historii.","Page memory only · reload clears history. Collector history is disabled.","Mémoire de la page uniquement · recharger efface l’historique. Historique du collecteur désactivé."],
  historyLoading:["Načítám uloženou historii…","Loading saved history…","Chargement de l’historique…"],
  historyFailed:["Uložená historie není dostupná; zobrazuji načtené vzorky.","Saved history unavailable; showing loaded samples.","Historique enregistré indisponible ; affichage des échantillons chargés."],
  chartRange:["Rozsah grafu","Chart range","Période du graphique"],
  chartExpand:["Zvětšit graf","Expand chart","Agrandir le graphique"],
  chartClose:["Zavřít graf","Close chart","Fermer le graphique"],
  chartExplore:["Najeď na graf pro hodnoty · kliknutím zvětšíš","Hover for values · click to expand","Survole pour les valeurs · clique pour agrandir"],
  chartInspect:["Pohybem odečítáš vzorky · kliknutí připne bod · šipky přecházejí mezi vzorky","Move to inspect samples · click to pin · arrow keys step through samples","Déplace le pointeur pour lire les relevés · clique pour épingler · utilise les flèches pour parcourir"],
  chartNoSample:["V tomto čase není zaznamenaný vzorek.","No sample was recorded at this time.","Aucun relevé enregistré à cet instant."],
  chartIssueLegend:["Problém procesu při sběru","Process issue when sampled","Problème du processus lors du relevé"],
  chartIssueScope:["Značky patří celému procesu; samy neurčují příčinu změny grafu.","Markers refer to the whole process; they do not by themselves explain a change in the chart.","Les marqueurs concernent tout le processus ; ils n’expliquent pas à eux seuls une variation du graphique."],
  chartIssueCount:["Vzorky s problémem: {count}","Samples with an issue: {count}","Relevés avec un problème : {count}"],
  chartValuesAt:["Hodnoty vzorku v {time}","Sample values at {time}","Valeurs du relevé à {time}"],
  chartIssueWindow:["Zachyceno ve vzorku; přírůstky chyb pokrývají předchozí interval.","Recorded at sample time; error deltas cover the preceding interval.","Enregistré au moment du relevé ; les variations d’erreurs couvrent l’intervalle précédent."],
  chartPinned:["Bod připnutý","Sample pinned","Relevé épinglé"],
  chartUnpin:["Uvolnit bod","Unpin sample","Désépingler le relevé"],
  chartPointExpired:["Připnutý vzorek už není v tomto rozsahu.","The pinned sample is no longer in this range.","Le relevé épinglé n’est plus dans cette période."],
  chartWorker:["CPU · worker {worker}","CPU · worker {worker}","CPU · worker {worker}"],
  chartWorkerExpand:["Zvětšit graf CPU workeru {worker}","Expand CPU chart for worker {worker}","Agrandir le graphique CPU du worker {worker}"],
  metricInfo:["Informace o metrice {key}","About metric {key}","À propos de la métrique {key}"],
  closeInfo:["Zavřít nápovědu","Close help","Fermer l’aide"],
  localLinks:["Lokální propojení","Local connections","Connexions locales"],
  linksHint:["Vazby podle parametrů procesů. (?) = namespace není ověřená. Neověřují průchod dat.","Links from process arguments. (?) = unverified namespace. Links do not confirm data flow.","Liens déduits des arguments. (?) = espace de noms non vérifié. Ces liens ne confirment pas le passage des données."],
  observedConfig:["POZOROVANÁ KONFIGURACE","OBSERVED CONFIG","CONFIGURATION OBSERVÉE"],
  processMetrics:["Metriky procesu","Process metrics","Métriques du processus"],
  metricsHint:["Hodnoty přímo z control socketu; čítače zůstávají přesné i nad 2⁵³.","Values directly from the control socket; counters remain exact above 2⁵³.","Valeurs issues du socket de contrôle ; les compteurs restent exacts au-delà de 2⁵³."],
  filterMetrics:["Filtrovat metriky","Filter metrics","Filtrer les métriques"],
  filterKeys:["Filtrovat klíče…","Filter keys…","Filtrer les clés…"],
  key:["Klíč","Key","Clé"],
  value:["Hodnota","Value","Valeur"],
  rulesRuntime:["Načtení mění běžící switch. Změna není uložená do konfiguračního souboru.","Loading changes the running switch. The change is not saved to its configuration file.","Le chargement modifie le switch actif. La modification n’est pas enregistrée dans son fichier de configuration."],
  readRules:["↓ Načíst aktivní","↓ Read active rules","↓ Lire les règles actives"],
  chooseSwitch:["Vyber běžící switch s control socketem v tabulce procesů.","Select a running switch with a control socket in the process table.","Sélectionne un switch actif avec un socket de contrôle dans le tableau des processus."],
  ruleset:["Sada pravidel","Ruleset","Jeu de règles"],
  checkRules:["Ověřit a zobrazit diff","Validate and show diff","Valider et afficher le diff"],
  loadRules:["Načíst do switche","Load into switch","Charger dans le switch"],
  syspiperTitle:["Uzly / Syspiper","Nodes / Syspiper","Nœuds / Syspiper"],
  syspiperHint:["Systémové metriky známých IP Tuntom sítě. Sběr zajišťuje backend.","System metrics for known Tuntom IPs. Collected by the backend.","Métriques système des IP Tuntom connues. Collectées par le backend."],
  syspiperDisabled:["Syspiper není zapnutý. Nastav klíč v backendu (na collectoru při odděleném sběru).","Syspiper is disabled. Configure its key in the backend (on the separate collector when used).","Syspiper est désactivé. Configure sa clé sur le backend (collecteur séparé si utilisé)."],
  syspiperEmpty:["Čekám na známé IP protistran nebo rozhraní Tuntomu. Další známou IP lze zadat backendu.","Waiting for known Tuntom peer or interface IPs. Additional known IPs can be configured in the backend.","En attente d’IP Tuntom connues. D’autres IP connues peuvent être configurées sur le backend."],
  syspiperScope:["CPU, RAM a disk patří celému hostu. Síť je součet rozhraní hostu, nikoli pouze provoz Tuntomu. Síťová rychlost vyžaduje dva vzorky.","CPU, RAM and disk cover the whole host. Network totals include all host interfaces, not just Tuntom. Rates require two samples.","CPU, RAM et disque concernent l’hôte entier. Le réseau regroupe toutes les interfaces, pas seulement Tuntom. Les débits nécessitent deux échantillons."],
  syspiper_ip:["IP","IP","IP"],
  syspiper_cpu:["CPU","CPU","CPU"],
  syspiper_ram:["RAM","RAM","RAM"],
  syspiper_disk:["Disk /","Disk /","Disque /"],
  syspiper_net:["Síť RX / TX","Network RX / TX","Réseau RX / TX"],
  syspiper_ok:["Dostupné","Available","Disponible"],
  syspiper_partial:["Částečné metriky","Partial metrics","Métriques partielles"],
  syspiper_unavailable:["Nedostupné","Unavailable","Indisponible"],
  syspiper_pending:["Čekám na vzorek","Waiting for sample","En attente d’un échantillon"],
  syspiper_local_host:["Lokální host","Local host","Hôte local"],
  syspiper_manual:["Zadaná IP","Configured IP","IP configurée"],
  syspiper_tunnel_peer:["Protistrana tunelu","Tunnel peer","Pair du tunnel"],
  syspiper_interface_local:["Adresa rozhraní","Interface address","Adresse d’interface"],
  syspiper_interface_peer:["Protistrana rozhraní","Point-to-point peer","Pair point à point"],
  syspiper_unauthorized:["Klíč odmítnut / přístup zakázán","Key rejected / access denied","Clé refusée / accès interdit"],
  syspiper_unsupported:["Verze endpoint nepodporuje","Endpoint not supported by this version","Endpoint non pris en charge"],
  syspiper_unreachable:["Spojení není dostupné","Connection unavailable","Connexion indisponible"],
  syspiper_timeout:["Vypršel čas spojení","Request timed out","Délai dépassé"],
  syspiper_redirect_rejected:["Přesměrování odmítnuto","Redirect rejected","Redirection refusée"],
  syspiper_http_error:["HTTP chyba","HTTP error","Erreur HTTP"],
  syspiper_invalid_response:["Neplatná odpověď","Invalid response","Réponse invalide"],
  syspiper_response_too_large:["Příliš velká odpověď","Response too large","Réponse trop volumineuse"],
  syspiper_probe_failed:["Sběr systémových metrik selhal nebo je neúplný","System collection failed or is incomplete","Collecte système échouée ou incomplète"],
  syspiperDetails:["Další metriky a dostupnost endpointů","Additional metrics and endpoint availability","Autres métriques et disponibilité des endpoints"],
  syspiperTruncated:["Zobrazený seznam metrik je omezen na 128 položek.","Metric list is limited to 128 entries.","La liste est limitée à 128 métriques."],
  syspiperDiscovery:["Adresy rozhraní se nepodařilo načíst; parametry tunelů a zadané IP zůstávají dostupné.","Interface address discovery failed; tunnel arguments and configured IPs are still used.","Échec de découverte des interfaces ; les IP des tunnels et les IP configurées restent utilisées."],
  syspiperLimit:["Limit 64 IP: vynecháno {count}.","64 IP limit: {count} omitted.","Limite de 64 IP : {count} ignorées."],
  footerSource:["Průzkum procesů · /proc + control sockety","Runtime discovery · /proc + control sockets","Détection des processus · /proc + sockets de contrôle"],
  footerState:["Historie telemetrie · žádné automatické změny","Telemetry history · no automatic changes","Historique de télémétrie · sans modifications automatiques"],
  stale:["Starý vzorek","Stale sample","Échantillon ancien"],
  waitingSession:["Čeká na session","Waiting for session","En attente de session"],
  metricsReady:["Metriky dostupné","Metrics available","Métriques disponibles"],
  loading:["Načítání","Loading","Chargement"],
  socketUnavailable:["Socket nedostupný","Socket unavailable","Socket inaccessible"],
  processOnly:["Pouze proces","Process only","Processus seul"],
  unknown:["Neznámé","Unknown","Inconnu"],
  scanFailed:["Průzkum /proc selhal: {error}","/proc discovery failed: {error}","Échec de l’analyse de /proc : {error}"],
  permissionCount:["Procesy nepřístupné kvůli oprávněním: {count}.","Processes inaccessible due to permissions: {count}.","Processus inaccessibles faute de permissions : {count}."],
  paused:["Obnovování stránky je pozastavené. Zobrazený stav může být starší.","Page refresh is paused. The displayed state may be out of date.","L’actualisation est suspendue. L’état affiché peut être ancien."],
  apiUnavailable:["API není dostupné: {error}","API unavailable: {error}","API inaccessible : {error}"],
  disconnected:["Spojení s API přerušeno","API connection lost","Connexion à l’API interrompue"],
  scanTime:["Scan {time} · {interval} s","Scan {time} · {interval} s","Analyse {time} · {interval} s"],
  kindCount:["Tunely: {tunnels} · switche: {switches}","Tunnels: {tunnels} · switches: {switches}","Tunnels : {tunnels} · switches : {switches}"],
  unknownSessions:["Tunely s neověřenou session: {count}","Tunnels with unverified sessions: {count}","Tunnels avec une session non vérifiée : {count}"],
  day:["d","d","j"], hour:["h","h","h"], minute:["m","m","min"],
  binary:["Binárka","Executable","Exécutable"],
  control:["Control socket","Control socket","Socket de contrôle"],
  unset:["Nenastavený","Not configured","Non configuré"],
  portRole:["Port / role","Port / role","Port / rôle"],
  peer:["Protistrana","Peer","Pair distant"],
  memoryThreads:["Paměť / vlákna","Memory / threads","Mémoire / threads"],
  cgroup:["Cgroup unit","Cgroup unit","Unité cgroup"],
  metricAge:["Stáří metrik","Metric age","Âge des métriques"],
  publicArgs:["Veřejné parametry spuštění","Public startup arguments","Arguments publics de démarrage"],
  noArgs:["Bez rozpoznaných parametrů","No recognized arguments","Aucun argument reconnu"],
  registeredPorts:["Registrované porty:","Registered ports:","Ports enregistrés :"],
  noLinks:["bez nalezených lokálních vazeb","no local connections found","aucune connexion locale détectée"],
  noLocalSwitch:["Bez nalezeného lokálního switche","No local switch found","Aucun switch local détecté"],
  noTopology:["Zatím není co propojit. Vazby vznikají ze společných cest ke switch socketům.","Nothing to connect yet. Links are derived from shared switch socket paths.","Aucune connexion à afficher. Les liens sont déduits des chemins de sockets de switch communs."],
  noMetrics:["Žádné dostupné metriky pro tento výběr.","No metrics available for this selection.","Aucune métrique disponible pour cette sélection."],
  metricsFor:["Metriky · {name}","Metrics · {name}","Métriques · {name}"],
  rulesFor:["Pravidla · {name}","Rules · {name}","Règles · {name}"],
  editRulesHint:["Načti aktivní pravidla, uprav serial a obsah, potom ověř diff.","Read the active rules, update the serial and content, then validate the diff.","Lis les règles actives, modifie le numéro de série et le contenu, puis valide le diff."],
  readOnlyHint:["Čtení a validace jsou dostupné. Ruční zápis zapneš pomocí --allow-write.","Reading and validation are available. Enable manual writes with --allow-write.","La lecture et la validation sont disponibles. Active l’écriture manuelle avec --allow-write."],
  discardDraft:["Zahodit rozepsaný návrh a načíst aktivní pravidla?","Discard the draft and read the active rules?","Abandonner le brouillon et lire les règles actives ?"],
  confirmLoad:["Načíst ověřený návrh do běžícího switche {name}? Změna platí pouze do restartu.","Load the validated draft into the running switch {name}? The change lasts only until restart.","Charger le brouillon validé dans le switch actif {name} ? La modification ne dure que jusqu’au redémarrage."],
  working:["Pracuji…","Working…","Traitement…"],
  rulesLoaded:["Aktivní pravidla načtena.","Active rules retrieved.","Règles actives récupérées."],
  noDiff:["Bez rozdílu.","No differences.","Aucune différence."],
  reloadRules:["Načti znovu aktivní pravidla pro další úpravu.","Read the active rules again before editing further.","Relis les règles actives avant de les modifier à nouveau."],
  draftChanged:["Návrh změněn; před načtením ho ověř.","Draft changed; validate it before loading.","Brouillon modifié ; valide-le avant de le charger."],
  tokenRequired:["Je potřeba platný přístupový token.","A valid access token is required.","Un jeton d’accès valide est nécessaire."],
  noControl:["Chybí přístupný --control-socket; pouze informace o procesu.","No accessible --control-socket; process information only.","Aucun --control-socket accessible ; informations du processus uniquement."],
  inaccessibleArgs:["Parametry procesu nejsou přístupné.","Process arguments are inaccessible.","Les arguments du processus sont inaccessibles."],
  permissionDenied:["Přístup odepřen","Permission denied","Permission refusée"],
  relativePath:["Nelze vyřešit relativní --{option}: pracovní adresář procesu není přístupný.","Cannot resolve relative --{option}: process working directory is inaccessible.","Impossible de résoudre --{option} relatif : le répertoire de travail du processus est inaccessible."],
  processGone:["Proces už neexistuje; obnov přehled.","The process no longer exists; refresh discovery.","Le processus n’existe plus ; actualise la détection."],
  rulesDisabled:["Zápis pravidel není povolen; spusť observer s --allow-write.","Rule writes are disabled; restart with --allow-write.","L’écriture des règles est désactivée ; redémarre avec --allow-write."],
  rulesConflict:["Aktivní pravidla se změnila; načti je znovu a zkontroluj návrh.","Active rules changed; read them again and review the draft.","Les règles actives ont changé ; relis-les et vérifie le brouillon."],
  typeTunnel:["TUNEL","TUNNEL","TUNNEL"], typeSwitch:["SWITCH","SWITCH","SWITCH"],
  typeAdapter:["ADAPTÉR","ADAPTER","ADAPTATEUR"], typeDivert:["DIVERT","DIVERT","DIVERT"],
  typeProcess:["PROCES","PROCESS","PROCESSUS"],
  loadResult:["{result} · Načti znovu aktivní pravidla pro další úpravu.","{result} · Read the active rules again before editing further.","{result} · Relis les règles actives avant de les modifier à nouveau."],
};
Object.assign(messages, {
  health:["Zdraví","Health","État"], healthReason:["Zdraví a důvody","Health and reasons","État et explications"],
  fieldStatus:["RUNTIME / STAV","RUNTIME / STATUS","RUNTIME / ÉTAT"], fieldNotes:["RUNTIME / DIAGNOSTIKA","RUNTIME / DIAGNOSTICS","RUNTIME / DIAGNOSTIC"],
  switchRuntime:["Porty a workery","Ports and workers","Ports et workers"],
  diagnostics:["Diagnostika","Diagnostics","Diagnostic"], openDiagnostics:["Otevřít diagnostiku ↗","Open diagnostics ↗","Ouvrir le diagnostic ↗"],
  openRules:["Otevřít pravidla ↗","Open rules ↗","Ouvrir les règles ↗"],
  recentChanges:["Od posledního vzorku","Since the last sample","Depuis le dernier relevé"],
  delta:["Přírůstek","Delta","Variation"], counterReset:["Reset čítače","Counter reset","Compteur réinitialisé"],
  health_ok:["V pořádku","Healthy","Normal"], health_warn:["Vyžaduje pozornost","Needs attention","À vérifier"],
  health_unknown:["Neúplná telemetrie","Incomplete telemetry","Télémétrie incomplète"],
  check_process:["PROCES","PROCESS","PROCESSUS"], check_session:["SPOJENÍ","CONNECTION","CONNEXION"],
  check_traffic:["PROVOZ","TRAFFIC","TRAFIC"], check_errors:["NOVÉ CHYBY","NEW ERRORS","NOUVELLES ERREURS"],
  process_running:["Proces běží","Process is running","Processus actif"],
  process_blocked:["Proces je zastavený nebo čeká v I/O","Process is stopped or waiting in I/O","Processus arrêté ou en attente d’E/S"],
  telemetry_missing:["Metriky nejsou dostupné","Metrics are unavailable","Métriques indisponibles"],
  session_down:["Session nebo spojení se switchem není navázané","Session or switch connection is down","Session ou connexion au switch non établie"],
  session_up:["Spojení je navázané","Connection established","Connexion établie"],
  session_unknown:["Chybí údaj o spojení","Connection status is unknown","État de connexion inconnu"],
  session_na:["Tento proces nehlásí tunnel session","This process does not report a tunnel session","Ce processus ne signale pas de session tunnel"],
  traffic_active:["Přenáší data","Transferring data","Transfert de données"],
  traffic_idle:["Bez provozu · klidový stav","No traffic · idle","Aucun trafic · au repos"],
  traffic_unknown:["Propustnost není dostupná","Throughput is unavailable","Débit indisponible"],
  errors_growing:["Přibyly chyby nebo neplánované dropy","New errors or unplanned drops","Nouvelles erreurs ou pertes inattendues"],
  errors_clear:["Žádné nové hlášené chyby","No new reported errors","Aucune nouvelle erreur signalée"],
  delta_waiting:["Čekám na dva po sobě jdoucí vzorky","Waiting for two consecutive samples","En attente de deux relevés consécutifs"],
  noChanges:["Sledované čítače se nezměnily.","Tracked counters have not changed.","Les compteurs suivis n’ont pas changé."],
  windowSeconds:["Δ / {seconds} s","Δ / {seconds} s","Δ / {seconds} s"],
  localCollector:["Lokální sběr · UID {uid}","Local collector · UID {uid}","Collecte locale · UID {uid}"],
  separateCollector:["Oddělený sběr · UID {uid}","Separate collector · UID {uid}","Collecte séparée · UID {uid}"],
  port:["Port","Port","Port"], workers:["Workery","Workers","Workers"],
  portsHint:["Živé porty switche a přiřazení RX/TX. Kliknutím otevřeš nalezený proces.","Live switch ports and RX/TX assignments. Click to open a discovered process.","Ports actifs et affectations RX/TX. Clique pour ouvrir un processus détecté."],
  workersHint:["CPU = přírůstek času workeru / čas mezi vzorky. 100 % odpovídá jednomu jádru.","CPU = worker time delta / time between samples. 100% equals one core.","CPU = variation du temps worker / intervalle des relevés. 100 % correspond à un cœur."],
  queueCaveat:["Počet front a použitých bufferů z metrik. Aktuální zaplnění front daemon neexportuje.","Queue count and used buffers from metrics. The daemon does not export current queue occupancy.","Nombre de files et buffers utilisés selon les métriques. Le démon n’exporte pas le remplissage actuel des files."],
  matrix_queues:["FRONTY","QUEUES","FILES"], buffers_in_use:["POUŽITÉ BUFFERY","BUFFERS IN USE","BUFFERS UTILISÉS"],
  queue_full_drops:["DROPY PLNÝCH FRONT","QUEUE FULL DROPS","PERTES / FILES PLEINES"],
  pool_stalls:["ČEKÁNÍ NA POOL","POOL STALLS","BLOCAGES DU POOL"],
  workers_active:["AKTIVNÍ WORKERY","ACTIVE WORKERS","WORKERS ACTIFS"],
  noPortStats:["Tento vzorek neobsahuje seznam živých portů. Vazby podle parametrů najdeš v přehledu.","This sample has no live port list. See the overview for links inferred from arguments.","Ce relevé ne contient pas de liste de ports actifs. Les liens déduits des arguments sont dans la vue d’ensemble."],
  noWorkerStats:["Per-worker metriky nejsou dostupné. Poskytuje je MP switch.","Per-worker metrics are unavailable. They are provided by the MP switch.","Métriques par worker indisponibles. Elles sont fournies par le switch MP."],
  workerIdle:["nepřiřazený","unassigned","non affecté"], generation:["generace {value}","generation {value}","génération {value}"],
  noMatchedProcess:["Proces nenalezen","Process not found","Processus introuvable"],
  connectedPorts:["Přiřazené porty","Assigned ports","Ports affectés"],
  observed:["z parametrů","from arguments","selon les arguments"],
  confirmedPort:["port registrován","port registered","port enregistré"],
  logsHint:["Posledních 60 řádků z běžného souboru stdout/stderr, případně journalu. Na vyžádání.","Last 60 lines from a regular stdout/stderr file, or the journal. On demand.","Les 60 dernières lignes du fichier stdout/stderr ou du journal. À la demande."],
  readLogs:["↻ Načíst logy","↻ Read logs","↻ Lire les logs"],
  logsEmpty:["Žádné dostupné záznamy. Proces může logovat jinam nebo chybí oprávnění.","No accessible records. The process may log elsewhere or access may be restricted.","Aucune entrée accessible. Le processus peut écrire ailleurs ou l’accès est restreint."],
  logsNotRead:["Logy zatím nejsou načtené.","Logs have not been read yet.","Les logs n’ont pas encore été lus."],
  truncated:["výpis zkrácen","output truncated","sortie tronquée"],
  report:["Diagnostický report","Diagnostic report","Rapport de diagnostic"],
  reportHint:["Proces, metriky, přírůstky, vazby, logy a aktivní pravidla switche. Známá tajná pole v logu se maskují; před sdílením report zkontroluj.","Process, metrics, deltas, links, logs and active switch rules. Known secret fields in logs are masked; review the report before sharing.","Processus, métriques, variations, liens, logs et règles actives. Les champs secrets connus des logs sont masqués ; vérifie le rapport avant de le partager."],
  copyReport:["Kopírovat report","Copy report","Copier le rapport"], saveReport:["↓ Uložit report","↓ Save report","↓ Enregistrer le rapport"],
  reportCopied:["Report je ve schránce; náhled najdeš níže.","Report copied; preview below.","Rapport copié ; aperçu ci-dessous."],
  copyFallback:["Schránka není přístupná. Report můžeš zkopírovat z pole níže nebo uložit.","Clipboard unavailable. Copy from the field below or save the report.","Presse-papiers inaccessible. Copie le champ ci-dessous ou enregistre le rapport."],
  reportSaved:["Report uložen.","Report saved.","Rapport enregistré."],
  moreReasons:["Další příčiny: {count}","{count} more reasons","{count} autres causes"],
  attentionCause:["Příčina upozornění","Reason for attention","Cause de l’alerte"],
  counterInWindow:["{key}: +{delta} za {seconds} s","{key}: +{delta} in {seconds} s","{key} : +{delta} en {seconds} s"],
  processStopped:["Proces je zastavený (stav {state})","Process is stopped (state {state})","Processus arrêté (état {state})"],
  processWaitingIO:["Proces čeká v nepřerušitelném I/O (stav D)","Process is waiting in uninterruptible I/O (state D)","Processus en attente d’E/S non interruptibles (état D)"],
  reason_session_ready:["Tunnel session není navázaná","Tunnel session is not established","La session tunnel n’est pas établie"],
  reason_switch_connected:["Spojení se switchem je přerušené","Switch connection is down","La connexion au switch est interrompue"],
  reason_divert_in_connected:["Vstupní spojení divertu je přerušené","Divert input connection is down","La connexion d’entrée divert est interrompue"],
  reason_divert_out_connected:["Výstupní spojení divertu je přerušené","Divert output connection is down","La connexion de sortie divert est interrompue"],
  warningHistory:["Poslední upozornění","Recent warnings","Alertes récentes"],
  warningRetention:["Posledních 5 chybových vzorků · nejvýše 5 minut","Last 5 warning samples · up to 5 minutes","5 derniers relevés en alerte · pendant 5 minutes maximum"],
  recordedWarning:["Zachycené upozornění","Recorded warning","Alerte enregistrée"],
  secondsAgo:["Před {seconds} s","{seconds} s ago","Il y a {seconds} s"],
  dismissWarning:["Zavřít upozornění pro {name}","Dismiss warning for {name}","Fermer l’alerte pour {name}"],
  warningProcessGone:["Proces už není v přehledu","Process is no longer listed","Le processus n’est plus dans la liste"],
});
let language = "cs";
try { const saved = localStorage.getItem("tuntom-fabric-language"); if (Object.hasOwn(languages,saved)) language=saved; } catch { /* Browser storage can be disabled. */ }
const locale = () => languages[language].locale;
const t = (key, params = {}) => (messages[key]?.[languages[language].index] ?? key).replace(/\{(\w+)\}/g, (match,name) => String(params[name] ?? match));
function diagnostic(value) {
  if (!value) return "";
  const keys = {
    "a valid bearer token is required":"tokenRequired",
    "No accessible --control-socket; process information only":"noControl",
    "Process arguments are inaccessible":"inaccessibleArgs",
    "process no longer exists; refresh discovery":"processGone",
    "rule writes are disabled; restart with --allow-write":"rulesDisabled",
    "active rules changed or expected_sha256 is missing; reload and review":"rulesConflict",
  };
  if (Object.hasOwn(keys,value)) return t(keys[value]);
  const relative = /^Cannot resolve relative --(.+): process cwd is inaccessible$/.exec(value);
  if (relative) return t("relativePath",{option:relative[1]});
  return String(value).replace("Permission denied",t("permissionDenied"));
}
const messageText = message => message?.key ? t(message.key,message.params) : diagnostic(message || "");

// Merge collector history with live five-second averages; changing views preserves it.
class ThroughputHistory {
  static retention = 24 * 60 * 60 * 1000;
  constructor() { this.samples = []; }
  prune(now) {
    const cutoff = now - ThroughputHistory.retention;
    let expired = 0;
    while (expired < this.samples.length && this.samples[expired].time < cutoff) expired++;
    if (expired) this.samples.splice(0, expired);
  }
  add(sample, now) {
    this.prune(now);
    if (!Number.isFinite(sample.time) || sample.time < now - ThroughputHistory.retention ||
        sample.time <= (this.samples.at(-1)?.time ?? -Infinity)) return;
    this.samples.push(sample);
  }
  merge(samples, now) {
    const points = new Map(this.samples.map(p=>[p.time,p]));
    // Persisted samples are authoritative and include collector-side incidents.
    for (const p of samples) if (Number.isFinite(p.time)) points.set(p.time,p);
    this.samples=[...points.values()].sort((a,b)=>a.time-b.time);
    this.prune(now);
  }
  window(range, now) {
    this.prune(now);
    return this.samples.filter(p => p.time >= now - range && p.time <= now);
  }
}

// At most four points per pixel bucket in each uninterrupted segment: first,
// minimum, maximum, last. Preserve peaks and never bridge missing observations.
function chartSeries(samples, key, start, end, gap, width = 576) {
  const result = [];
  let bucket = [], column = -1, previous = null;
  const flush = () => {
    if (!bucket.length) return;
    let min = 0, max = 0;
    for (let i = 1; i < bucket.length; i++) {
      if (bucket[i][key] < bucket[min][key]) min = i;
      if (bucket[i][key] > bucket[max][key]) max = i;
    }
    for (const i of [...new Set([0,min,max,bucket.length-1])].sort((a,b)=>a-b)) result.push(bucket[i]);
    bucket = [];
  };
  for (const p of samples) {
    if (!Number.isFinite(p[key]) || (previous !== null && p.time - previous > gap)) {
      flush();
      if (result.length && result.at(-1) !== null) result.push(null);
      previous = null;
    }
    if (!Number.isFinite(p[key])) continue;
    const next = Math.floor((p.time - start) / Math.max(1,end-start) * width);
    if (next !== column) { flush(); column = next; }
    bucket.push(p); previous = p.time;
  }
  flush();
  return result;
}

// Inspect original observations, not the reduced drawing or interpolated gaps.
function nearestChartSample(samples, time, tolerance = Infinity) {
  let left = 0, right = samples.length;
  while (left < right) {
    const middle = (left + right) >>> 1;
    if (samples[middle].time < time) left = middle + 1;
    else right = middle;
  }
  const before = samples[left-1], after = samples[left];
  const sample = !before ? after : !after ? before :
    time-before.time <= after.time-time ? before : after;
  return sample && Math.abs(sample.time-time) <= tolerance ? sample : null;
}
function chartIssueBuckets(samples, start, end, width) {
  const buckets = new Map();
  for (const sample of samples) {
    if (!sample.issues?.length || sample.time < start || sample.time > end) continue;
    const column = Math.floor((sample.time-start) / Math.max(1,end-start) * width / 12);
    if (!buckets.has(column)) buckets.set(column,[]);
    buckets.get(column).push(sample);
  }
  return [...buckets.values()];
}

// History is independent of the current health and lives only in this page.
class WarningHistory {
  constructor(clock = () => performance.now()) {
    this.clock = clock;
    this.items = [];
    this.seen = new Map();
    this.sequence = 0;
  }
  prune() {
    const cutoff = this.clock() - 300000;
    this.items = this.items.filter(item => item.recordedAt > cutoff);
  }
  observe(samples) {
    this.prune();
    const present = new Set(samples.map(sample => sample.endpointId));
    for (const id of this.seen.keys()) if (!present.has(id)) this.seen.delete(id);
    for (const sample of samples) {
      if (!sample.sampleId || this.seen.get(sample.endpointId) === sample.sampleId) continue;
      this.seen.set(sample.endpointId, sample.sampleId);
      if (!sample.checks.length) continue;
      this.items.unshift({...sample, id:String(++this.sequence), recordedAt:this.clock()});
    }
    this.items = this.items.slice(0, 5);
  }
  list() { this.prune(); return this.items; }
  age(item) { return Math.max(0, Math.floor((this.clock() - item.recordedAt) / 1000)); }
  dismiss(id) { this.items = this.items.filter(item => item.id !== id); }
}

const $ = id => document.getElementById(id);
const state = {data: null, selected: null, view: "overview", paused: false, busy: false,
  history: new Map(), historyLoads: new Map(), chartRange: 300000, chartNow: Date.now(), drafts: new Map(), logs: new Map(), reports: new Map(), diagnosticBusy: false,
  warnings: new WarningHistory(), warningsLayout: "", chartDialog: null,
  token: "", failure: "", loginError: "", ruleBusy: false};
const types = {tunnel:"typeTunnel", switch:"typeSwitch", adapter:"typeAdapter", divert:"typeDivert", process:"typeProcess"};
const typeName = kind => types[kind] ? t(types[kind]) : kind || "—";
const esc = text => String(text ?? "").replace(/[&<>"']/g, char => ({"&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;"}[char]));

// Interpretations follow src/ipc/switch_transport.hpp, switch_mp/runtime.hpp,
// throughput_stats.hpp and processing_stats.hpp. Unknown keys stay explicit.
const metricDescriptions = {
  total:["Součet za životnost čítače; sleduj hlavně nové přírůstky Δ. Po restartu či výměně transportu může začít znovu.","Total over the counter’s lifetime; focus on new deltas Δ. It may restart after a process or transport replacement.","Total sur la durée de vie du compteur ; surveille surtout les variations Δ. Il peut repartir après un redémarrage ou un remplacement du transport."],
  ipc_direction:["Směr {direction} z pohledu procesu, který metriku hlásí.","Direction {direction} from the reporting process’s perspective.","Sens {direction} du point de vue du processus qui publie la métrique."],
  ipc_version:["Vyjednaná verze IPC. 1 = původní přenos rámců, 2 = rozšířený protokol; sama verze 2 nezaručuje mmap.","Negotiated IPC version. 1 = original frame transport, 2 = extended protocol; version 2 alone does not imply mmap.","Version IPC négociée. 1 = transport initial, 2 = protocole étendu ; la version 2 seule ne garantit pas mmap."],
  ipc_mmap:["Sdílená paměť: 1 = aktivní, 0 = přenos inline přes socket. Nula je platný režim, ne chyba.","Shared memory: 1 = active, 0 = inline socket transport. Zero is a valid mode, not an error.","Mémoire partagée : 1 = active, 0 = transfert direct par socket. Zéro est un mode valide, pas une erreur."],
  ipc_batch_limit:["Maximální vyjednaný počet rámců v jedné mmap dávce. Bez mmap je 1; nejde o počet čekajících rámců.","Maximum negotiated frames per mmap batch. Without mmap it is 1; this is not a queue depth.","Nombre maximal négocié de trames par lot mmap. Sans mmap : 1 ; ce n’est pas la taille d’une file d’attente."],
  ipc_mapping_bytes:["Celková velikost vstupního a výstupního mapování v bajtech. Není to aktuálně použitá kapacita ani RSS; bez mmap může být 0.","Combined input and output mapping size in bytes. This is neither occupied capacity nor RSS; it can be 0 without mmap.","Taille totale des mappings d’entrée et de sortie, en octets. Ce n’est ni l’occupation ni le RSS ; elle peut être nulle sans mmap."],
  ipc_calls:["Pokusy o socketové čtení/zápis, včetně neúspěšných. Více volání než záznamů může znamenat prázdné čtení nebo dočasný backpressure, ne nutně chybu.","Socket read/write attempts, including unsuccessful ones. More calls than records can mean empty reads or temporary backpressure, not necessarily a fault.","Tentatives de lecture/écriture du socket, même sans succès. Plus d’appels que d’enregistrements peut signaler des lectures à vide ou une contre-pression temporaire."],
  ipc_records:["Socketové záznamy přijaté/odeslané transportem. Jeden záznam může obsahovat celou mmap dávku; nejde vždy o počet rámců.","Socket records received/sent by the transport. One record may carry a whole mmap batch; this is not always a frame count.","Enregistrements reçus/envoyés par le transport. Un enregistrement peut contenir tout un lot mmap ; ce n’est pas toujours un nombre de trames."],
  ipc_inline_frames:["Rámce přenesené přímo socketem. Běžné bez mmap, u příliš velkých rámců nebo při fallbacku; samotný růst není chyba.","Frames carried directly by the socket. Normal without mmap, for oversized frames or during fallback; growth alone is not a fault.","Trames transférées directement par socket. Normal sans mmap, pour les grandes trames ou en repli ; une hausse seule n’est pas une erreur."],
  ipc_mapped_frames:["Rámce přenesené odkazem do sdílené paměti. Nula je běžná při neaktivním mmap nebo bez provozu.","Frames transferred by shared-memory reference. Zero is normal when mmap is inactive or traffic is idle.","Trames transférées par référence en mémoire partagée. Zéro est normal sans mmap actif ou sans trafic."],
  ipc_batches:["Záznamy v dávkovém mmap režimu; dávka může obsahovat i jediný rámec. Nula je běžná bez vyjednaného dávkování.","Records in mmap batch mode; a batch can contain just one frame. Zero is normal when batching is not negotiated.","Enregistrements en mode lot mmap ; un lot peut contenir une seule trame. Zéro est normal sans traitement par lots négocié."],
  ipc_largest_batch:["Nejvyšší pozorovaný počet mmap rámců v jednom záznamu. Historické maximum, ne součet; 0 znamená žádné zaznamenané mmap rámce.","Largest observed number of mmap frames in one record. A high-water mark, not a total; 0 means no mmap frames recorded.","Nombre maximal observé de trames mmap par enregistrement. Maximum historique, pas un total ; 0 signifie aucune trame mmap enregistrée."],
  ipc_pool_fallback:["Mmap slot nebyl dostupný a transport přešel na kratší dávku či inline přenos. Růst ukazuje tlak na sdílený pool, sám o sobě neznamená ztrátu rámců. RX tuto událost nepočítá.","No mmap slot was available, so transport used a shorter batch or inline transfer. Growth indicates shared-pool pressure, not necessarily frame loss. RX does not count this event.","Aucun slot mmap disponible : lot raccourci ou transfert direct. Une hausse indique une pression sur le pool partagé, pas forcément une perte. RX ne compte pas cet événement."],
  ipc_invalid:["Odmítnuté neplatné IPC záznamy, reference nebo tokeny slotů. Nové přírůstky zaslouží kontrolu kompatibility protistrany a logů. Zvyšuje se na RX; TX ho nepočítá.","Rejected invalid IPC records, references or slot tokens. New increments warrant checking peer compatibility and logs. Counted on RX; TX does not increment it.","Enregistrements IPC, références ou jetons de slots invalides rejetés. Une hausse mérite de vérifier la compatibilité du pair et les logs. Compté en RX, pas en TX."],
  matrix_queues:["Počet front mezi workery v aktuálním plánu. Není to jejich zaplnění; mění se s topologií a přiřazením rolí.","Number of inter-worker queues in the current plan. This is not occupancy; it changes with topology and role assignment.","Nombre de files entre workers dans le plan actuel. Ce n’est pas leur occupation ; il varie avec la topologie et les rôles."],
  buffers_in_use:["Aktuálně obsazené buffery v poolech portů. Růst může doprovázet čekání na zpracování; vyhodnocuj spolu s pool_stalls a dropy.","Buffers currently occupied in port pools. Growth can accompany processing delays; compare with pool_stalls and drops.","Buffers actuellement occupés dans les pools des ports. Une hausse peut accompagner un retard de traitement ; compare avec pool_stalls et les pertes."],
  queue_full_drops:["Rámce zahozené při plné frontě mezi workery. Nové přírůstky ukazují, že navazující fáze nestíhá; porovnej CPU workerů a zátěž.","Frames dropped when an inter-worker queue was full. New increments indicate a downstream stage falling behind; compare worker CPU and traffic.","Trames perdues car une file entre workers était pleine. Une hausse indique qu’une étape suivante peine à suivre ; compare CPU et trafic."],
  pool_stalls:["Pokusy o příjem odložené kvůli chybějícímu volnému bufferu. Nejde přímo o drop; dlouhodobý růst znamená tlak na pool nebo pomalé uvolňování bufferů.","Receive attempts deferred because no free buffer was available. Not a direct drop; sustained growth indicates pool pressure or slow buffer release.","Réceptions différées faute de buffer libre. Ce n’est pas une perte directe ; une hausse durable indique une pression sur le pool ou une libération lente."],
  workers_active:["Počet workerů s přiřazenou rolí v aktuálním plánu. Neznamená počet právě vytížených CPU; porovnej s workers_pool.","Workers assigned a role in the current plan. This is not the number of busy CPUs; compare with workers_pool.","Workers ayant un rôle dans le plan actuel. Ce n’est pas le nombre de CPU occupés ; compare avec workers_pool."],
  workers_pool:["Celkový počet vytvořených workerů, včetně neaktivních. Určuje dostupnou kapacitu plánovače.","Total created workers, including idle ones. Defines the scheduler’s available worker capacity.","Nombre total de workers créés, y compris inactifs. Définit la capacité disponible de l’ordonnanceur."],
  cpu_ns:["CPU čas vlákna v nanosekundách. Δ / uplynulý čas × 100 dává vytížení; 100 % odpovídá jednomu jádru. Vysoké CPU posuzuj spolu s propustností a dropy.","Thread CPU time in nanoseconds. Delta / elapsed time × 100 gives utilization; 100% equals one core. Compare high CPU with throughput and drops.","Temps CPU du thread en nanosecondes. Variation / temps écoulé × 100 donne l’utilisation ; 100 % correspond à un cœur. Compare avec débit et pertes."],
  poll_calls:["Počet volání čekání na I/O události workeru. Rychlý růst může znamenat mnoho krátkých probuzení; sám o sobě nepotvrzuje vytížení CPU.","Worker I/O event wait calls. Fast growth can mean many short wakeups; it does not by itself prove high CPU utilization.","Appels d’attente d’événements d’E/S du worker. Une hausse rapide peut indiquer de nombreux réveils courts, sans prouver une forte charge CPU."],
  route_misses:["Rámce bez odpovídajícího směrování. Nové přírůstky: zkontroluj pravidla, labely a dostupné porty.","Frames with no matching route. For new increments, check rules, labels and available ports.","Trames sans route correspondante. En cas de hausse, vérifie règles, labels et ports disponibles."],
  route_hits:["Rámce, pro které se našlo směrování. Růst je běžný při provozu; neznamená sám o sobě úspěšné odeslání cíli.","Frames for which a route was found. Growth is normal with traffic; it does not alone mean successful delivery to the target.","Trames pour lesquelles une route a été trouvée. Une hausse est normale avec du trafic ; elle ne prouve pas la livraison à la cible."],
  ecmp_packets:["Pakety, při jejichž směrování se vybíralo z více cest pomocí ECMP. Růst ukazuje použití vícecestného směrování, ne rovnoměrnost rozložení ani doručení.","Packets whose routing selected among multiple paths using ECMP. Growth indicates multipath routing use, not balanced distribution or delivery.","Paquets routés en choisissant parmi plusieurs chemins avec ECMP. Une hausse indique l’usage du multipath, pas un équilibrage uniforme ni une livraison réussie."],
  drops_mtu:["Pakety přesahující povolenou velikost MTU, i po zpracování či složení fragmentů. Nové přírůstky: porovnej MTU rozhraní a tunelu.","Packets exceeding the allowed MTU, including after processing or reassembly. For new increments, compare interface and tunnel MTUs.","Paquets dépassant le MTU autorisé, y compris après traitement ou réassemblage. En cas de hausse, compare les MTU de l’interface et du tunnel."],
  drops_process:["Pakety odmítnuté zpracováním v packet pipeline. Zkontroluj nakonfigurované procesory/filtry a logy; odmítnutí může být záměrné.","Packets rejected by processing in the packet pipeline. Check configured processors/filters and logs; rejection may be intentional.","Paquets rejetés par le traitement du pipeline. Vérifie les processeurs/filtres configurés et les logs ; le rejet peut être voulu."],
  drops_replay:["Pakety odmítnuté ochranou proti opakování. Přírůstky mohou souviset s duplikací nebo pořadím paketů; samy neprokazují útok.","Packets rejected by replay protection. Increments can relate to duplicates or packet ordering; they do not alone prove an attack.","Paquets rejetés par la protection anti-rejeu. Une hausse peut venir de doublons ou de l’ordre des paquets ; elle ne prouve pas une attaque."],
  rtt_lost:["RTT sondy bez odpovědi do timeoutu. Růst může ukazovat ztrátu, přetížení nebo nedostupnou protistranu; není to počet všech ztracených datových paketů.","RTT probes with no reply before timeout. Growth can indicate loss, congestion or an unavailable peer; this is not a count of all lost data packets.","Sondes RTT sans réponse avant expiration. Une hausse peut indiquer perte, congestion ou pair indisponible ; ce n’est pas le total des paquets de données perdus."],
  target_disconnected:["Směrování nemělo připojený cíl. Zkontroluj registraci cílových portů a jejich procesy.","Routing had no connected target. Check target port registrations and their processes.","Le routage n’avait pas de cible connectée. Vérifie l’enregistrement des ports cibles et leurs processus."],
  malformed_frames:["Přijaté rámce s neplatným formátem nebo opcode. Nové přírůstky mohou ukazovat na neslučitelnou protistranu nebo chybný vstup.","Received frames with invalid format or opcode. New increments can indicate an incompatible peer or malformed input.","Trames reçues avec format ou opcode invalide. Une hausse peut indiquer un pair incompatible ou une entrée incorrecte."],
  policy_drops:["Rámce odmítnuté politikou pravidel. Mohou být záměrné; ověř, zda odpovídají zamýšlené filtraci.","Frames rejected by rule policy. They may be intentional; check that they match the intended filtering.","Trames rejetées par la politique des règles. Cela peut être voulu ; vérifie que le filtrage correspond à l’intention."],
  rewrite_drops:["Rámce, u kterých nešla provést úprava zásobníku labelů nebo se výsledek nevešel. Zkontroluj operace pravidel a velikost rámce.","Frames whose label-stack rewrite failed or whose result did not fit. Check rule operations and frame size.","Trames dont la réécriture des labels a échoué ou dont le résultat était trop grand. Vérifie les règles et la taille des trames."],
  reconfiguration_drops:["Rámce odložené v pipeline a zahozené při změně plánu. Přírůstky kolem rekonfigurace mohou být očekávané.","Frames pending in the pipeline and discarded during a plan change. Increments around reconfiguration can be expected.","Trames en attente dans le pipeline, abandonnées lors d’un changement de plan. Une hausse pendant la reconfiguration peut être attendue."],
  shutdown_drops:["Rámce zahozené při vyprazdňování pipeline během ukončování. Souvisí s vypnutím, ne s běžným přenosem.","Frames discarded while draining the pipeline during shutdown. Related to stopping, not normal forwarding.","Trames abandonnées lors du vidage du pipeline à l’arrêt. Lié à l’arrêt, pas au transfert normal."],
  backpressure:["Rámce zahozené, protože příjemce/socket nestíhal přijímat. Sleduj nové přírůstky, zátěž a CPU protistrany. MP switch tuto souhrnnou metriku exportuje jako 0; sleduj také send_eagain a queue_full_drops.","Frames dropped because the receiver/socket could not keep up. Check new deltas, traffic and peer CPU. The MP switch exports this aggregate as 0; also check send_eagain and queue_full_drops.","Trames perdues car le récepteur/socket ne suivait pas. Compare variations, trafic et CPU du pair. Le switch MP exporte cet agrégat à 0 ; regarde aussi send_eagain et queue_full_drops."],
  eagain:["Socket dočasně nemohl pokračovat; příjem může být prázdný a odesílání blokované. Opakování je běžná součást neblokujícího I/O, nikoliv automaticky ztráta dat.","The socket temporarily could not proceed: a receive may be empty or a send blocked. Retries are normal in nonblocking I/O, not automatically data loss.","Le socket ne pouvait temporairement pas continuer : lecture à vide ou écriture bloquée. Les nouvelles tentatives sont normales en E/S non bloquantes, sans perte automatique."],
  traffic:["Počet přenesených paketů/rámců nebo bajtů v uvedeném směru z pohledu procesu (RX příjem, TX odesílání). Nula bez provozu je normální; růst nepotvrzuje doručení celé cesty.","Transferred packets/frames or bytes in the process’s indicated direction (RX receive, TX send). Zero at idle is normal; growth does not confirm end-to-end delivery.","Paquets/trames ou octets transférés dans le sens du processus (RX réception, TX émission). Zéro au repos est normal ; une hausse ne prouve pas la livraison de bout en bout."],
  rate5s:["Průměr za poslední dokončený 5s interval; bps = bity/s, pps = pakety/s. Nula může být klid nebo rozběh sběru. Krátké špičky se v průměru vyhladí.","Average over the last completed 5s bucket; bps = bits/s, pps = packets/s. Zero can mean idle or collection startup. Brief spikes are averaged out.","Moyenne du dernier intervalle de 5 s terminé ; bps = bits/s, pps = paquets/s. Zéro peut indiquer le repos ou le démarrage. Les pics brefs sont lissés."],
  rate1m:["Průměr dokončených 5s intervalů za nejvýše minutu. Při rozběhu pokrývá kratší dobu; porovnej s 5s hodnotou pro změnu zátěže.","Average of completed 5s buckets over up to one minute. Covers less time at startup; compare with the 5s value for traffic changes.","Moyenne des intervalles de 5 s terminés, sur une minute au maximum. Plus courte au démarrage ; compare avec la valeur de 5 s pour voir les changements de trafic."],
  connection:["Aktuální stav spojení/session: 1 = navázané nebo připravené, 0 = nenavázané. Trvalou nulu ověř proti protistraně, konfiguraci a logům.","Current connection/session flag: 1 = connected or ready, 0 = not established. For persistent zero, check the peer, configuration and logs.","État actuel de la connexion/session : 1 = établie ou prête, 0 = non établie. Si zéro persiste, vérifie le pair, la configuration et les logs."],
  errors:["Hlášené chyby dané operace. Historická nenulová hodnota nemusí znamenat aktuální problém; u nových přírůstků zkontroluj související socket, protistranu a logy.","Reported errors for this operation. A historical nonzero value need not mean a current problem; for new increments check the relevant socket, peer and logs.","Erreurs signalées pour cette opération. Un total historique non nul n’indique pas forcément un problème actuel ; en cas de hausse, vérifie socket, pair et logs."],
  drops:["Zahození označená daným důvodem. Sleduj přírůstky vůči objemu provozu a související logy; samotný historický součet neurčuje současný stav.","Discards labelled with this reason. Compare increments with traffic volume and related logs; a historical total alone does not establish current health.","Rejets portant ce motif. Compare les variations au volume de trafic et aux logs ; le total historique seul ne détermine pas l’état actuel."],
  disconnects:["Ztráty spojení se switchem. Nové přírůstky mohou souviset s restartem protistrany nebo chybou socketu; porovnej reconnects a aktuální connected.","Lost switch connections. New increments can reflect peer restarts or socket errors; compare reconnects and current connected state.","Connexions au switch perdues. Une hausse peut venir d’un redémarrage du pair ou d’une erreur de socket ; compare reconnects et connected."],
  reconnects:["Úspěšná opětovná připojení ke switchi. Růst ukazuje zotavení; časté opakování spolu s disconnects značí nestabilní spojení.","Successful switch reconnections. Growth indicates recovery; frequent repeats alongside disconnects suggest an unstable connection.","Reconnexions réussies au switch. Une hausse indique un rétablissement ; des répétitions fréquentes avec disconnects suggèrent une connexion instable."],
  processing:["Vzorkovaná doba zpracování v mikrosekundách: každý 1024. pokus. p95/p99 používají posledních nejvýše 4096 měření, avg/max celou historii. Růst může ukazovat na dražší zpracování.","Sampled processing time in microseconds: every 1024th attempt. p95/p99 use up to the latest 4096 measurements; avg/max cover all history. Growth can indicate more expensive processing.","Temps de traitement échantillonné en microsecondes : une tentative sur 1024. p95/p99 utilisent les 4096 dernières mesures au plus ; avg/max couvrent tout l’historique. Une hausse peut indiquer un traitement plus coûteux."],
  unknown:["Metrika přímo z daemonu. Pro tento klíč zatím není ověřený podrobný popis; z hodnoty samotné nelze určit příčinu ani zdraví procesu.","Metric reported directly by the daemon. A verified detailed description is not yet available for this key; the value alone cannot identify a cause or determine process health.","Métrique publiée directement par le daemon. La description détaillée de cette clé n’est pas encore vérifiée ; la valeur seule ne permet pas de déterminer une cause ou l’état du processus."]
};
function metricHelp(key) {
  const base = key.replace(/^port_\d+_/,"").replace(/^worker_\d+_/,"").replace(/^switch_ipc_/,"ipc_");
  const ipc = /^ipc_(rx|tx)_(.+)$/.exec(base);
  let description = ipc ? "ipc_"+ipc[2] : base, cumulative = !!ipc && ipc[2] !== "largest_batch";
  if (!Object.hasOwn(metricDescriptions,description)) {
    if (/_bps_5s$|_pps_5s$/.test(base)) description = "rate5s";
    else if (/_bps_1m$|_pps_1m$/.test(base)) description = "rate1m";
    else if (/^(session_ready|session_confirmed|switch_connected|divert_(in|out)_connected)$/.test(base)) description = "connection";
    else if (/backpressure_drops$/.test(base)) description = "backpressure";
    else if (/_eagain$/.test(base)) description = "eagain";
    else if (/_disconnects$/.test(base)) description = "disconnects";
    else if (/_reconnects$/.test(base)) description = "reconnects";
    else if (/_errors$/.test(base)) description = "errors";
    else if (/^drops_|_drops$/.test(base)) description = "drops";
    else if (/^(frames|bytes)_(rx|tx)$|_(rx|tx)_(packets|bytes)$|^fragments_(rx|tx)$/.test(base)) description = "traffic";
    else if (/_(avg|max|p95|p99)_us$/.test(base)) description = "processing";
    else description = "unknown";
  }
  cumulative ||= ["cpu_ns","poll_calls","queue_full_drops","pool_stalls","route_hits","route_misses","ecmp_packets","target_disconnected","malformed_frames","policy_drops","rewrite_drops","reconfiguration_drops","shutdown_drops","drops_mtu","drops_process","drops_replay","rtt_lost","backpressure","eagain","traffic","errors","drops","disconnects","reconnects"].includes(description);
  const local = name => metricDescriptions[name][languages[language].index];
  return [ipc ? local("ipc_direction").replace("{direction}",ipc[1].toUpperCase()) : "",local(description),cumulative ? local("total") : ""].filter(Boolean).join(" ");
}
function metricInfo(key) {
  return `<button type="button" class="metric-info" data-metric-help="${esc(key)}" title="${esc(metricHelp(key))}" aria-label="${esc(t("metricInfo",{key}))}">i</button>`;
}
document.addEventListener("click",event=>{
  const button = event.target.closest("[data-metric-help]");
  if (!button) return;
  $("metric-help-key").textContent = button.dataset.metricHelp;
  $("metric-help-text").textContent = metricHelp(button.dataset.metricHelp);
  $("metric-help").showModal();
});
const selected = () => state.data?.endpoints.find(e => e.id === state.selected);
const endpointURL = id => `/api/v1/endpoints/${encodeURIComponent(id)}/rules`;
const rate = (e, direction) => {
  const value = e?.metrics[`${e.kind === "tunnel" ? "udp" : "switch"}_${direction}_bps_5s`];
  return value !== undefined && Number.isFinite(Number(value)) ? Number(value) : null;
};
function bps(value) {
  if (value === null || value === undefined) return "—";
  const units = ["b/s", "kb/s", "Mb/s", "Gb/s", "Tb/s"];
  let index = 0;
  while (value >= 1000 && index < units.length - 1) { value /= 1000; index++; }
  return `${value.toLocaleString(locale(), {maximumFractionDigits: index ? 1 : 0})} ${units[index]}`;
}
function duration(seconds) {
  if (seconds === undefined) return "—";
  seconds = Math.max(0, Number(seconds));
  if (seconds >= 86400) return `${Math.floor(seconds / 86400)} ${t("day")} ${Math.floor(seconds % 86400 / 3600)} ${t("hour")}`;
  if (seconds >= 3600) return `${Math.floor(seconds / 3600)} ${t("hour")} ${Math.floor(seconds % 3600 / 60)} ${t("minute")}`;
  return `${Math.floor(seconds / 60)} ${t("minute")} ${Math.floor(seconds % 60)} s`;
}
function sampleAge(e) { return e?.sampled_at ? Math.max(0, Math.floor((Date.now() - Date.parse(e.sampled_at)) / 1000)) : null; }
function outdated(e) { return !!state.failure || sampleAge(e) > Math.max(15,(state.data?.poll_interval_seconds || 5)*3); }
function warningChecks(e) { return outdated(e) ? [] : (e?.health?.checks || []).filter(check=>check.state === "warn"); }
function checkReasons(e,check) {
  if (check.key === "process") return [{text:check.value === "D" ? t("processWaitingIO") : t("processStopped",{state:check.value})}];
  const flags=Object.entries(check.values || {}).filter(([,value])=>value === "0").map(([key,value])=>({
    metric:key, text:`${t(messages["reason_"+key] ? "reason_"+key : check.code)} · ${key}=${value}`
  }));
  const counters=Object.entries(check.counters || {}).map(([key,value])=>({metric:key,
    text:t("counterInWindow",{key,delta:BigInt(value).toLocaleString(locale()),
      seconds:e.changes?.interval_seconds?.toLocaleString(locale(),{maximumFractionDigits:1}) ?? "—"})
  }));
  return flags.length || counters.length ? [...flags,...counters] : [{text:t(check.code)}];
}
function attentionReasons(e) { return warningChecks(e).flatMap(check=>checkReasons(e,check)); }
function problemMetrics(e) { return new Set(attentionReasons(e).map(reason=>reason.metric).filter(Boolean)); }
function status(e) {
  if (e.health) {
    if (outdated(e)) return ["warn",t("stale")];
    return [{ok:"",warn:"alert",unknown:"dim"}[e.health.level] || "dim", t("health_"+e.health.level)];
  }
  if (e.status === "reachable") {
    if (sampleAge(e) > Math.max(15, state.data.poll_interval_seconds * 3)) return ["warn", t("stale")];
    if (e.kind === "tunnel" && e.metrics.session_ready === "0") return ["warn", t("waitingSession")];
    return ["", t("metricsReady")];
  }
  return {pending:["dim", t("loading")], unavailable:["warn", t("socketUnavailable")], process_only:["dim", t("processOnly")]}[e.status] || ["dim", t("unknown")];
}
async function api(path, method = "GET", body) {
  const headers = {Authorization: `Bearer ${state.token}`};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const response = await fetch(path, {method, headers, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(15000)});
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401) { $("login").hidden = false; $("workspace").hidden = true; }
    throw new Error(data.error || `HTTP ${response.status}`);
  }
  return data;
}
function notice() {
  const info = state.data?.discovery;
  const parts = [state.failure ? t("apiUnavailable",{error:diagnostic(state.failure)}) : ""];
  if (info?.status === "error") parts.push(t("scanFailed",{error:diagnostic(info.error)}));
  if (info?.permission_denied) parts.push(t("permissionCount",{count:info.permission_denied}));
  if (state.paused) parts.push(t("paused"));
  $("notice").textContent = parts.filter(Boolean).join(" ");
  $("notice").hidden = !$("notice").textContent;
}
async function refresh(force = false) {
  if (state.busy || !state.token || (state.paused && !force)) return;
  state.busy = true;
  try {
    const data = await api("/api/v1/snapshot");
    state.data = data;
    state.failure = "";
    state.warnings.observe(data.endpoints.map(e=>({endpointId:e.id, name:e.name, pid:e.pid,
      sampleId:e.sampled_at, checks:warningChecks(e), changes:e.changes})));
    $("login").hidden = true; $("workspace").hidden = false;
    $("login-error").hidden = true;
    state.loginError = "";
    if (!data.endpoints.some(e => e.id === state.selected)) state.selected = data.endpoints[0]?.id || null;
    const present = new Set([...data.endpoints.map(e => e.id),...(data.syspiper?.nodes || []).map(n=>n.id)]);
    for (const id of state.history.keys()) if (!present.has(id)) state.history.delete(id);
    for (const map of [state.logs,state.reports,state.historyLoads]) for (const id of map.keys()) if (!present.has(id)) map.delete(id);
    state.chartNow = Date.parse(data.generated_at) || Date.now();
    for (const e of data.endpoints) {
      const history = state.history.get(e.id) || new ThroughputHistory();
      const issues = warningChecks(e);
      if (e.status === "unavailable" && !outdated(e)) issues.push({key:"telemetry",code:"telemetry_missing"});
      history.add({time:Date.parse(e.sampled_at), rx:rate(e,"rx"), tx:rate(e,"tx"),
        ...Object.fromEntries((e.switch_detail?.workers || []).map(w=>[`cpu_${w.index}`,w.cpu_percent])),
        ...(issues.length ? {issues,interval:e.changes?.interval_seconds} : {})}, state.chartNow);
      state.history.set(e.id, history);
    }
    for (const node of data.syspiper?.nodes || []) {
      if (!node.chart) continue;
      const history=state.history.get(node.id)||new ThroughputHistory();
      history.add(node.chart,state.chartNow); state.history.set(node.id,history);
    }
    render();
    loadHistory(state.selected);
    if (state.chartDialog) loadHistory(state.chartDialog.endpointId);
  } catch (error) {
    state.failure = error.message;
    state.loginError = error.message;
    $("login-error").textContent = diagnostic(error.message);
    $("login-error").hidden = false;
    notice();
    $("last-update").textContent = t("disconnected");
    if (state.data) { renderProcesses(); renderHealth(); renderMetrics(); renderSwitch(); renderTopology(); drawChart(); }
  } finally { state.busy = false; }
}
function render() {
  const data = state.data;
  $("hostname").textContent = data.discovery.host || "—";
  $("write-mode").textContent = t(data.allow_write ? "writeEnabled" : "observing");
  $("last-update").textContent = state.failure ? t("disconnected") : data.discovery.scanned_at ? t("scanTime",{time:new Date(data.discovery.scanned_at).toLocaleTimeString(locale()),interval:data.poll_interval_seconds}) : t("waitingScan");
  const endpoints = data.endpoints, tunnels = endpoints.filter(e => e.kind === "tunnel");
  $("count-processes").textContent = endpoints.length;
  $("count-kinds").textContent = t("kindCount",{tunnels:tunnels.length,switches:endpoints.filter(e => e.kind === "switch").length});
  $("count-metrics").textContent = `${endpoints.filter(e => e.status === "reachable").length} / ${endpoints.length}`;
  const knownSessions = tunnels.filter(e => ["0","1"].includes(e.metrics.session_ready));
  $("count-sessions").textContent = `${knownSessions.length ? tunnels.filter(e => e.metrics.session_ready === "1").length : "—"} / ${tunnels.length}`;
  $("count-sessions").nextElementSibling.textContent = knownSessions.length < tunnels.length ? t("unknownSessions",{count:tunnels.length-knownSessions.length}) : t("readyFound");
  const ports = endpoints.filter(e => e.kind === "switch" && /^\d+$/.test(e.metrics.connections_current || ""));
  $("count-ports").textContent = ports.length ? ports.reduce((n,e) => n + BigInt(e.metrics.connections_current), 0n).toString() : "—";
  notice(); renderProcesses(); renderDetail(); renderTopology(); renderMetrics(); renderRules();
  renderHealth(); renderSwitch(); renderDiagnostics(); renderWarnings(); renderSyspiper();
}
function renderProcesses() {
  if (!state.data) return;
  const query = $("search").value.toLowerCase(), kind = $("kind-filter").value;
  const rows = state.data.endpoints.filter(e => (!kind || e.kind === kind) &&
    [e.name,e.pid,e.interface,e.executable,e.port_id].join(" ").toLowerCase().includes(query));
  $("process-total").textContent = rows.length;
  $("processes").innerHTML = rows.map(e => {
    const [color, text] = status(e);
    const reasons=attentionReasons(e);
    const indicator=reasons.length ? `<button class="status attention-status" data-attention="${esc(e.id)}" title="${esc(reasons.map(reason=>reason.text).join("\n"))}"><span class="attention-title"><span class="attention-mark" aria-hidden="true">!</span>${esc(text)} ↗</span><span class="status-reasons">${reasons.slice(0,2).map(reason=>esc(reason.text)).join("<br>")}${reasons.length > 2 ? `<br>${esc(t("moreReasons",{count:reasons.length-2}))}` : ""}</span></button>` : `<span class="status"><i class="dot ${color}"></i>${esc(text)}</span>`;
    return `<tr data-process-row="${esc(e.id)}" class="${e.id === state.selected ? "selected" : ""}"><td><button class="process-name" data-id="${esc(e.id)}" aria-pressed="${e.id === state.selected}">${esc(e.name)}<small>PID ${e.pid}${e.interface && e.interface !== "-" ? " · " + esc(e.interface) : ""}</small></button></td><td><span class="kind">${esc(typeName(e.kind))}</span></td><td>${indicator}</td><td>${bps(rate(e,"rx"))}<small>↑ ${bps(rate(e,"tx"))}</small></td><td>${duration(e.uptime_seconds)}</td></tr>`;
  }).join("");
  $("empty").hidden = rows.length > 0;
  $("empty").querySelector("h3").textContent = t(state.data.endpoints.length ? "noMatches" : "noProcesses");
  $("empty").querySelector("p").textContent = t(state.data.endpoints.length ? "changeFilter" : "startProcesses");
}
function renderDetail() {
  const e = selected();
  $("detail-title").textContent = e?.name || t("chooseProcess");
  $("detail-kind").textContent = typeName(e?.kind);
  if (!e) { $("detail").innerHTML = `<p class="muted">${esc(t("appearAfterStart"))}</p>`; drawChart(); return; }
  const values = [["PID / UID", `${e.pid} / ${e.uid}`], [t("binary"), e.executable], [t("control"), e.control || t("unset")],
    ["Switch", e.switch_socket || "—"], [t("portRole"), [e.port_id,e.role].filter(Boolean).join(" / ") || "—"],
    [t("peer"), e.peer || "—"], [t("memoryThreads"), `${(e.rss_bytes / 1048576).toLocaleString(locale(),{minimumFractionDigits:1,maximumFractionDigits:1})} MiB / ${e.threads}`],
    [t("cgroup"), e.unit || "—"], [t("metricAge"), sampleAge(e) === null ? "—" : `${sampleAge(e)} s`]];
  const notes = [...e.notes, e.error].filter(Boolean).map(diagnostic);
  const wasOpen = $("detail").querySelector("details")?.open;
  $("detail").innerHTML = `<dl>${values.map(([key,value])=>`<dt>${esc(key)}</dt><dd>${esc(value)}</dd>`).join("")}</dl>${notes.length ? `<div class="notice">${notes.map(esc).join("<br>")}</div>` : ""}<details ${wasOpen ? "open" : ""}><summary>${esc(t("publicArgs"))}</summary><pre>${esc(Object.entries(e.options).map(([k,v])=>`--${k}${v === true ? "" : " " + v}`).join("\n") || t("noArgs"))}</pre></details>`;
  drawChart();
}
function historyNote(id) {
  if (!state.data?.history?.enabled) return t("historyMemory");
  const load=state.historyLoads.get(id);
  return t("chartHistory")+" "+(state.data.history.error || load?.error ? t("historyFailed") : load?.busy ? t("historyLoading") : "");
}
async function loadHistory(id) {
  if (!id || state.paused || !state.data?.history?.enabled) return;
  let load=state.historyLoads.get(id);
  if (!load) { load={busy:false,until:0,retry:0}; state.historyLoads.set(id,load); }
  if (load.busy || Date.now()<load.retry) return;
  load.busy=true;
  try {
    // An overlap catches probes which finished committing while the previous page was read.
    let after=Math.max(0,load.until-60000), until=null;
    do {
      const page=await api(`/api/v1/${id.startsWith("syspiper:") ? "syspiper" : "endpoints"}/${encodeURIComponent(id)}/history?after=${after}`+(until===null ? "" : `&until=${until}`));
      until=page.until;
      if (state.historyLoads.get(id)!==load) return;
      const history=state.history.get(id)||new ThroughputHistory();
      history.merge(page.samples,state.chartNow);
      state.history.set(id,history);
      after=page.next_after;
    } while (after!==null);
    load.until=until; load.error=false;
  } catch (error) {
    load.error=true;
  } finally {
    load.busy=false; load.retry=Date.now()+(load.error ? 10000 : 4000);
    if (state.historyLoads.get(id)===load) drawChart();
  }
}
const chartViews = new WeakMap();
function chartValue(value, cpu = false) {
  if (!Number.isFinite(value)) return "—";
  const exact = value.toLocaleString(locale(),{maximumFractionDigits:3});
  return cpu ? `${exact} %` : `${bps(value)} (${exact} b/s)`;
}
function chartReasons(sample) {
  return (sample.issues || []).flatMap(check=>checkReasons({changes:{interval_seconds:sample.interval}},check).map(reason=>reason.text));
}
function chartIssueText(group) {
  const first = new Date(group[0].time).toLocaleString(locale());
  const last = new Date(group.at(-1).time).toLocaleString(locale());
  const reasons = [...new Set(group.flatMap(chartReasons))];
  return {time:group.length > 1 ? `${first} → ${last}` : first, reasons};
}
function drawTimeChart(svg, endpointId, worker = null, expanded = false) {
  const previous = chartViews.get(svg);
  const samples = state.history.get(endpointId)?.window(state.chartRange,state.chartNow) || [];
  const width = Math.max(320,svg.clientWidth), height = Math.max(200,svg.clientHeight);
  const model = {endpointId,worker,samples,start:state.chartNow-state.chartRange,end:state.chartNow,
    width,height,left:76,right:width-18,top:24,bottom:height-66,
    gap:Math.max(15000,(endpointId?.startsWith("syspiper:") ? (state.data?.syspiper?.interval_seconds || 30) : (state.data?.poll_interval_seconds || 5))*3000),
    keys:worker === null ? ["rx","tx"] : [typeof worker === "string" ? worker : `cpu_${worker}`],expanded,
    time:null,pinned:false,issue:false};
  if (previous && previous.endpointId === endpointId && previous.worker === worker) {
    for (const key of ["time","pinned","issue"]) model[key] = previous[key];
  }
  model.max = samples.reduce((max,p)=>Math.max(max,...model.keys.map(key=>p[key] ?? 0)),worker === null ? 1 : 100);
  model.x = time => model.left+(time-model.start)/Math.max(1,model.end-model.start)*(model.right-model.left);
  model.y = value => model.bottom-value/model.max*(model.bottom-model.top);
  model.groups = chartIssueBuckets(samples,model.start,model.end,model.right-model.left);
  chartViews.set(svg,model);
  svg.setAttribute("viewBox",`0 0 ${width} ${height}`);
  const axes = Array.from({length:4},(_,i)=>{
    const value = model.max*i/3, y=model.y(value);
    return `<path class="grid" d="M${model.left},${y} H${model.right}"/><text x="${model.left-9}" y="${y+4}" text-anchor="end">${esc(worker === null ? bps(value) : value.toLocaleString(locale(),{maximumFractionDigits:1})+" %")}</text>`;
  }).join("");
  const ticks = width > 700 ? 4 : state.chartRange >= 43200000 ? 1 : 2;
  const times = Array.from({length:ticks+1},(_,i)=>{
    const time = model.start+(model.end-model.start)*i/ticks;
    const text = new Date(time).toLocaleString(locale(),state.chartRange >= 43200000 ?
      {day:"numeric",month:"short",hour:"2-digit",minute:"2-digit"} : {hour:"2-digit",minute:"2-digit"});
    return `<text x="${model.x(time)}" y="${height-12}" text-anchor="${i===0 ? "start" : i===ticks ? "end" : "middle"}">${esc(text)}</text>`;
  }).join("");
  const paths = model.keys.map((key,index)=>{
    let connected = false;
    const d = chartSeries(samples,key,model.start,model.end,model.gap,model.right-model.left).map(p=>{
      if (!p) { connected=false; return ""; }
      const command = connected ? "L" : "M"; connected=true;
      return `${command}${model.x(p.time).toFixed(2)},${model.y(p[key]).toFixed(2)}`;
    }).join(" ");
    const latest = samples.at(-1), color=index ? "tx" : "rx";
    return `<path class="${color}" d="${d}"/>`+(Number.isFinite(latest?.[key]) ? `<circle class="chart-point ${color}" cx="${model.x(latest.time)}" cy="${model.y(latest[key])}" r="2.5"/>` : "");
  }).join("");
  // A dedicated incident lane also shows warnings when throughput/CPU is missing.
  const issues = model.groups.map((group,index)=>{
    const x = model.x(group[0].time);
    return `<g data-chart-issue="${index}" class="chart-issue"><title>${esc(t("chartIssueCount",{count:group.length}))} · ${esc(new Date(group[0].time).toLocaleString(locale()))}</title><circle class="chart-issue-hit" cx="${x}" cy="${height-39}" r="11"/><circle cx="${x}" cy="${height-39}" r="${group.length > 1 ? 5 : 4}"/></g>`;
  }).join("");
  svg.innerHTML = axes+times+paths+issues+'<g class="chart-cursor" hidden></g>';
  renderChartReadout(svg);
}
function renderChartReadout(svg) {
  const model = chartViews.get(svg), readout = $(model.expanded ? "chart-detail-readout" : "chart-readout");
  const cursor = svg.querySelector(".chart-cursor");
  const sample = model.time === null ? null : nearestChartSample(model.samples,model.time,model.pinned ? 0 : model.gap/2);
  cursor.setAttribute("hidden","");
  if (model.expanded) $("chart-unpin").disabled = !model.pinned;
  if (model.time === null) { readout.textContent=t(model.expanded ? "chartInspect" : "chartExplore"); return; }
  if (!sample) {
    readout.textContent=new Date(model.time).toLocaleString(locale())+" · "+t(model.pinned && (model.time < model.start || model.time > model.end) ? "chartPointExpired" : "chartNoSample");
    return;
  }
  const group = model.issue ? model.groups.find(group=>group.some(p=>p.time===sample.time)) : null;
  const issues = group || (sample.issues?.length ? [sample] : []);
  const details = issues.length ? chartIssueText(issues) : null;
  const heading = details?.time || new Date(sample.time).toLocaleString(locale());
  const values = model.keys.map((key,index)=>`<span class="chart-value ${index ? "tx-value" : "rx-value"}">${model.worker === null ? key.toUpperCase() : typeof model.worker === "string" ? t("syspiper_"+model.worker) : "CPU"} <strong>${esc(chartValue(sample[key],model.worker !== null))}</strong></span>`).join("");
  // Grouped incident ranges list their causes; the value readout is the first sample.
  const html = `<div class="chart-readout-heading"><time>${esc(heading)}</time>${model.pinned ? `<span class="tag">${esc(t("chartPinned"))}</span>` : ""}</div>${issues.length > 1 ? `<small>${esc(t("chartValuesAt",{time:new Date(sample.time).toLocaleString(locale())}))}</small>` : ""}${values}${details ? `<div class="chart-issue-details"><strong>${esc(t("chartIssueCount",{count:issues.length}))}</strong><ul>${details.reasons.slice(0,10).map(reason=>`<li>${esc(reason)}</li>`).join("")}${details.reasons.length > 10 ? `<li>${esc(t("moreReasons",{count:details.reasons.length-10}))}</li>` : ""}</ul><small>${esc(t("chartIssueWindow"))}</small></div>` : ""}`;
  if (readout.innerHTML !== html) readout.innerHTML=html;
  cursor.removeAttribute("hidden");
  const x = model.x(sample.time);
  cursor.innerHTML = `<path d="M${x},${model.top} V${model.height-29}"/>`+model.keys.map((key,index)=>Number.isFinite(sample[key]) ? `<circle class="${index ? "tx" : "rx"}" cx="${x}" cy="${model.y(sample[key])}" r="4"/>` : "").join("");
}
function inspectChartPointer(svg,event,pin = false) {
  const model = chartViews.get(svg);
  if (!model || (model.pinned && !pin)) return;
  const marker = event.target.closest("[data-chart-issue]");
  const group = marker ? model.groups[Number(marker.dataset.chartIssue)] : null;
  const matrix = svg.getScreenCTM();
  if (!matrix) return;
  const point = new DOMPoint(event.clientX,event.clientY).matrixTransform(matrix.inverse());
  const time = model.start+Math.max(0,Math.min(1,(point.x-model.left)/(model.right-model.left)))*(model.end-model.start);
  const nearest = nearestChartSample(model.samples,time,model.gap/2);
  model.time = group?.[0].time ?? nearest?.time ?? time;
  model.issue = !!group; model.pinned=pin;
  renderChartReadout(svg);
}
function openChart(endpointId,worker = null,inspection = null) {
  const endpoint = state.data?.endpoints.find(e=>e.id===endpointId);
  if (!endpoint) return;
  state.chartDialog={endpointId,worker,name:endpoint.name,pid:endpoint.pid};
  chartViews.delete($("chart-detail"));
  loadHistory(endpointId);
  $("chart-dialog").showModal();
  renderChartDialog();
  if (inspection) {
    Object.assign(chartViews.get($("chart-detail")),inspection);
    renderChartReadout($("chart-detail"));
  }
}
function renderChartDialog() {
  if (!$("chart-dialog").open || !state.chartDialog) return;
  const {endpointId,worker,name,pid}=state.chartDialog;
  if (state.chartDialog.node) {
    const metric=worker || "net";
    $("chart-dialog-title").textContent=t("syspiper_"+metric);
    $("chart-dialog-context").textContent=name+" · Syspiper";
    $("chart-detail-range").value=String(state.chartRange);
    $("chart-detail").setAttribute("aria-label",t("syspiper_"+metric));
    $("chart-dialog-legend").innerHTML=`<span>${esc(worker === null ? "RX / TX" : t("syspiper_"+metric))}</span><span><i class="dot alert"></i>${esc(t("syspiper_probe_failed"))}</span>`;
    $("chart-dialog-note").textContent=t("syspiperScope")+" "+historyNote(endpointId)+(state.data?.syspiper?.history_error ? " "+t("historyFailed") : "");
    drawTimeChart($("chart-detail"),endpointId,worker,true);
    return;
  }
  const exists = state.data?.endpoints.some(e=>e.id===endpointId);
  $("chart-dialog-title").textContent=worker === null ? t("throughput") : t("chartWorker",{worker});
  $("chart-dialog-context").textContent=`${name} · PID ${pid}`+(exists ? "" : " · "+t("warningProcessGone"));
  $("chart-detail-range").value=String(state.chartRange);
  $("chart-detail").setAttribute("aria-label",worker === null ? t("chartLabel") : t("chartWorker",{worker}));
  $("chart-dialog-legend").innerHTML=(worker === null ? '<span><i class="dot"></i>RX <i class="dot tx"></i>TX</span>' : '<span><i class="dot"></i>CPU</span>')+`<span><i class="dot alert"></i>${esc(t("chartIssueLegend"))}</span>`;
  $("chart-dialog-note").textContent=t(worker === null ? "fiveSecondAverage" : "workersHint")+" "+t("chartIssueScope")+" "+historyNote(endpointId);
  drawTimeChart($("chart-detail"),endpointId,worker,true);
}
function drawChart() {
  const e = selected();
  $("rate-rx").textContent = bps(rate(e,"rx")); $("rate-tx").textContent = bps(rate(e,"tx"));
  $("chart-expand").disabled = !e;
  $("chart-range").value=String(state.chartRange);
  drawTimeChart($("chart"),e?.id);
  $("chart-note").textContent = (rate(e,"rx") === null ? t("chartUnavailable")+" " : "")+historyNote(e?.id);
  renderChartDialog();
}
function changeChartRange(value) {
  state.chartRange=Number(value);
  for (const svg of [$("chart"),$("chart-detail")]) {
    const model=chartViews.get(svg);
    if (model) { model.time=null; model.pinned=false; model.issue=false; }
  }
  drawChart();
}
for (const svg of [$("chart"),$("chart-detail")]) {
  svg.addEventListener("pointermove",event=>inspectChartPointer(svg,event));
  svg.addEventListener("pointerleave",()=>{
    const model=chartViews.get(svg);
    if (model && !model.pinned) { model.time=null; renderChartReadout(svg); }
  });
  svg.addEventListener("click",event=>{
    inspectChartPointer(svg,event,true);
    const model=chartViews.get(svg);
    if (svg.id === "chart" && model) { openChart(model.endpointId,null,{time:model.time,pinned:true,issue:model.issue}); model.pinned=false; }
  });
  svg.addEventListener("keydown",event=>{
    const model=chartViews.get(svg);
    if (!model) return;
    if (svg.id === "chart" && ["Enter"," "].includes(event.key)) {
      event.preventDefault(); openChart(model.endpointId); return;
    }
    if (!["ArrowLeft","ArrowRight","Home","End","Enter"," "].includes(event.key) || !model.samples.length) return;
    event.preventDefault();
    const sample=nearestChartSample(model.samples,model.time ?? model.end);
    let index=model.samples.indexOf(sample);
    if (event.key === "Home") index=0;
    if (event.key === "End") index=model.samples.length-1;
    if (event.key === "ArrowLeft") index=Math.max(0,index-1);
    if (event.key === "ArrowRight") index=Math.min(model.samples.length-1,index+1);
    model.time=model.samples[index].time; model.pinned=true; model.issue=false;
    renderChartReadout(svg);
  });
}
$("chart-expand").addEventListener("click",()=>openChart(state.selected));
$("chart-detail-range").addEventListener("change",event=>changeChartRange(event.target.value));
$("chart-unpin").addEventListener("click",()=>{
  const model=chartViews.get($("chart-detail"));
  if (model) { model.time=null; model.pinned=false; model.issue=false; renderChartReadout($("chart-detail")); }
});
$("chart-dialog").addEventListener("close",()=>{
  state.chartDialog=null;
  // A refreshed worker button may have replaced the original focus target.
  if (!document.activeElement || document.activeElement === document.body) $("refresh").focus({preventScroll:true});
});
let chartResize;
window.addEventListener("resize",()=>{clearTimeout(chartResize);chartResize=setTimeout(()=>drawChart(),100);});
function renderSyspiper() {
  const info=state.data?.syspiper;
  const nodes=info?.nodes || [];
  $("syspiper-notice").textContent=!info?.enabled ? t("syspiperDisabled") :
    [t("syspiperScope"), !nodes.length ? t("syspiperEmpty") : "",info.discovery_error ? t("syspiperDiscovery") : "",
      info.skipped ? t("syspiperLimit",{count:info.skipped}) : "",info.history_error ? t("historyFailed") : ""].filter(Boolean).join(" ");
  const opened=new Set([...$("syspiper-nodes").querySelectorAll('details[open]')].map(el=>el.dataset.nodeDetails));
  const percent=v=>Number.isFinite(v) ? v.toLocaleString(locale(),{maximumFractionDigits:1})+" %" : "—";
  $("syspiper-nodes").innerHTML=nodes.map(node=>{
    const values=node.values || {}, chart=node.chart || {};
    const age=node.sampled_at ? Math.max(0,Math.round((Date.now()-Date.parse(node.sampled_at))/1000)) : null;
    const stale=state.failure || age > info.interval_seconds*3;
    const stateText=t(stale ? "stale" : "syspiper_"+node.status);
    const problem=Object.entries(node.errors || {}).find(([,code])=>code!=="unsupported");
    const processes=(node.processes || []).map(id=>state.data.endpoints.find(e=>e.id===id)).filter(Boolean);
    return `<article class="syspiper-card"><div class="panel-heading"><div><h3>${esc(node.ip)} <small>${esc(values.hostname || "")}</small></h3><p>${esc(node.sources.map(source=>t("syspiper_"+source)).join(" · "))}</p></div><div><span class="status"><i class="dot ${stale || node.status === "unavailable" ? "alert" : node.status === "ok" ? "" : "dim"}"></i>${esc(stateText)}</span>${problem ? `<p class="syspiper-error">/${esc(problem[0])}: ${esc(t("syspiper_"+problem[1]))}</p>` : ""}</div></div><div class="syspiper-values">${["cpu","ram","disk","net"].map(metric=>`<button data-system-chart="${esc(node.id)}" data-system-metric="${metric}" aria-haspopup="dialog" aria-label="${esc(node.ip+" · "+t("syspiper_"+metric)+" · "+t("chartExpand"))}"><span>${esc(t("syspiper_"+metric))}</span><strong>${esc(metric === "net" ? bps(chart.rx)+" / "+bps(chart.tx) : percent(values[metric]))}</strong><small>${esc(t("chartExpand"))} ↗</small></button>`).join("")}</div><div class="syspiper-meta"><span>${node.sampled_at ? esc(new Date(node.sampled_at).toLocaleString(locale()))+` · ${age} s` : "—"} · ${info.interval_seconds} s</span>${processes.map(e=>`<button class="quiet-button" data-system-process="${esc(e.id)}">${esc(e.name)} · PID ${e.pid} ↗</button>`).join("")}</div><details data-node-details="${esc(node.id)}" ${opened.has(node.id) ? "open" : ""}><summary>${esc(t("syspiperDetails"))}</summary><ul>${Object.entries(node.errors || {}).map(([path,error])=>`<li>/${esc(path)}: ${esc(t("syspiper_"+error))}</li>`).join("")}</ul><div class="table-scroll"><table><tbody>${[...Object.entries(values).filter(([key,value])=>typeof value === "number" || key.startsWith("net_")).map(([key,value])=>[key,value]),...Object.entries(values.load || {}).map(([key,value])=>["load."+key,value]),...(values.details || [])].map(([key,value])=>`<tr><td>${esc(key)}</td><td class="system-value">${esc(value ?? "—")}</td></tr>`).join("")}</tbody></table></div>${values.details_truncated ? `<p>${esc(t("syspiperTruncated"))}</p>` : ""}</details></article>`;
  }).join("");
}
$("syspiper-nodes").addEventListener("click",event=>{
  const process=event.target.closest("[data-system-process]");
  if (process) { selectProcess(process.dataset.systemProcess,"overview"); return; }
  const button=event.target.closest("[data-system-chart]");
  if (!button) return;
  const node=state.data?.syspiper?.nodes.find(n=>n.id===button.dataset.systemChart);
  if (!node) return;
  const metric=button.dataset.systemMetric;
  state.chartDialog={endpointId:node.id,worker:metric === "net" ? null : metric,name:node.ip,node:true};
  chartViews.delete($("chart-detail"));
  $("chart-dialog").showModal(); renderChartDialog(); loadHistory(node.id);
});
function renderTopology() {
  const data = state.data;
  const switches = data.endpoints.filter(e => e.kind === "switch");
  const attached = new Set(data.links.map(l=>l.source));
  const blocks = switches.map(sw => {
    const links = data.links.filter(l=>l.target === sw.id);
    const lines = links.map(l => {
      const e = data.endpoints.find(e=>e.id === l.source);
      const registered = sw.switch_detail?.ports.some(p=>p.name === l.port_id);
      return `<div class="topology-branch"><button data-node="${esc(e?.id)}" class="topology-node ${e?.id === state.selected ? "active" : ""}"><span><i class="dot ${status(e)[0]}"></i>${esc(e?.name)}</span><small>${esc(typeName(e?.kind))} · ${esc(l.port_id || "—")}</small></button><span class="link-basis">${esc(t(registered ? "confirmedPort" : "observed"))}${l.namespace_verified ? "" : " (?)"}</span></div>`;
    });
    return `<div class="topology-cluster"><div class="topology-root"><button class="topology-node ${sw.id === state.selected ? "active" : ""}" data-node="${esc(sw.id)}"><span><i class="dot ${status(sw)[0]}"></i>${esc(sw.name)}</span><small>SWITCH · PID ${sw.pid}</small></button><button class="quiet-button" data-node-rules="${esc(sw.id)}">${esc(t("openRules"))}</button></div><div class="topology-branches">${lines.join("") || `<p>${esc(t("noLinks"))}</p>`}</div></div>`;
  });
  const others = data.endpoints.filter(e=>e.kind !== "switch" && !attached.has(e.id));
  if (others.length) blocks.push(`<div class="topology-cluster"><p>${esc(t("noLocalSwitch"))}</p>${others.map(e=>`<button class="topology-node" data-node="${esc(e.id)}"><span>${esc(e.name)}</span><small>${esc(typeName(e.kind))}${e.peer ? " → "+esc(e.peer) : ""}</small></button>`).join("")}</div>`);
  $("topology").innerHTML = blocks.join("") || `<p class="muted">${esc(t("noTopology"))}</p>`;
}
function renderMetrics() {
  const e = selected(), query = $("metric-search").value.toLowerCase();
  const problems=problemMetrics(e);
  $("metrics-title").textContent = e ? t("metricsFor",{name:e.name}) : t("processMetrics");
  const entries = Object.entries(e?.metrics || {}).filter(([k,v])=>(k+" "+v).toLowerCase().includes(query)).sort(([a],[b])=>a.localeCompare(b));
  $("metrics").innerHTML = entries.map(([k,v])=>`<tr class="${problems.has(k) ? "metric-problem" : ""}"><td>${problems.has(k) ? `<span class="attention-mark" aria-label="${esc(t("attentionCause"))}">!</span> ` : ""}${esc(k)} ${metricInfo(k)}</td><td>${esc(v)}</td><td>${deltaText(e,k)}</td></tr>`).join("") || `<tr><td colspan="3">${esc(t("noMetrics"))}</td></tr>`;
}
function draftFor(id) {
  if (!state.drafts.has(id)) state.drafts.set(id, {text:"", revision:null, checked:null, diff:"", message:""});
  return state.drafts.get(id);
}
function renderRules() {
  const e = selected(), valid = e?.kind === "switch" && !!e.control;
  const draft = e ? draftFor(e.id) : {text:"", message:"", diff:""};
  $("rules-title").textContent = valid ? t("rulesFor",{name:e.name}) : t("rules");
  $("rules-hint").textContent = t(valid ? (state.data.allow_write ? "editRulesHint" : "readOnlyHint") : "chooseSwitch");
  $("rules-read").disabled = !valid || state.ruleBusy;
  $("rules-editor").disabled = !valid || state.ruleBusy || !draft.revision;
  if ($("rules-editor").value !== draft.text) $("rules-editor").value = draft.text;
  $("rules-check").disabled = !valid || state.ruleBusy || !draft.revision || !draft.text.trim();
  $("rules-load").disabled = !valid || state.ruleBusy || !state.data?.allow_write || !draft.checked || draft.checked.text !== draft.text;
  $("rules-status").textContent = messageText(draft.message);
  $("rules-diff").hidden = !draft.diff && !draft.checked;
  $("rules-diff").innerHTML = (draft.diff || (draft.checked ? t("noDiff") : "")).split("\n").map(line=>`<span class="diff-line ${line.startsWith("+") ? "add" : line.startsWith("-") ? "remove" : ""}">${esc(line)}</span>`).join("\n");
}
async function ruleAction(operation) {
  const e = selected();
  if (!e || state.ruleBusy) return;
  const draft = draftFor(e.id), text = draft.text;
  if (operation === "show" && draft.revision && draft.text !== draft.active && !window.confirm(t("discardDraft"))) return;
  if (operation === "load" && (!draft.checked || draft.checked.text !== text || !window.confirm(t("confirmLoad",{name:e.name})))) return;
  state.ruleBusy = true; draft.message = {key:"working"}; renderRules();
  try {
    const result = await api(endpointURL(e.id) + (operation === "show" ? "" : "/" + operation), operation === "show" ? "GET" : "POST",
      operation === "show" ? undefined : {rules:text, expected_sha256:operation === "load" ? draft.checked.revision : draft.revision});
    if (operation === "show") { draft.text = result.rules; draft.active = result.rules; draft.revision=result.sha256; draft.checked=null; draft.diff=""; draft.message={key:"rulesLoaded"}; }
    if (operation === "check") { draft.checked={text,revision:result.sha256}; draft.diff=result.diff; draft.message=result.result; }
    if (operation === "load") { draft.checked=null; draft.revision=null; draft.diff=""; draft.message={key:"loadResult",params:{result:result.result}}; await refresh(true); }
  } catch (error) { draft.checked=null; draft.message=error.message; }
  finally { state.ruleBusy=false; renderRules(); }
}
function showView(view) {
  state.view=view;
  document.querySelectorAll("[data-view]").forEach(b=>b.classList.toggle("active",b.dataset.view === state.view));
  for (const name of ["overview","metrics","rules","switch","diagnostics","syspiper"]) $("view-"+name).hidden = view !== name;
  $("view-"+view).scrollIntoView({block:"start"});
  if (view === "overview") drawChart();
  if (view === "diagnostics" && selected() && !state.logs.has(state.selected)) readLogs();
}
function selectProcess(id, view) {
  state.selected=id; render(); loadHistory(id);
  if (view) showView(view);
  else if (state.view === "diagnostics" && !state.logs.has(id)) readLogs();
}
document.querySelectorAll("[data-view]").forEach(button=>button.addEventListener("click",()=>showView(button.dataset.view)));
$("processes").addEventListener("click",event=>{
  const attention=event.target.closest("[data-attention]"),button=event.target.closest("[data-id]"),row=event.target.closest("[data-process-row]");
  if (attention) selectProcess(attention.dataset.attention,"overview");
  else if (button) selectProcess(button.dataset.id);
  else if (row && !event.target.closest("button,a,input,select,textarea,summary") && !window.getSelection()?.toString()) selectProcess(row.dataset.processRow);
});
$("search").addEventListener("input",renderProcesses); $("kind-filter").addEventListener("change",renderProcesses);
$("metric-search").addEventListener("input",renderMetrics);
$("chart-range").addEventListener("change",event=>changeChartRange(event.target.value));
$("rules-read").addEventListener("click",()=>ruleAction("show"));
$("rules-check").addEventListener("click",()=>ruleAction("check"));
$("rules-load").addEventListener("click",()=>ruleAction("load"));
$("rules-editor").addEventListener("input",()=>{ if (!state.selected) return; const draft=draftFor(state.selected); draft.text=$("rules-editor").value; draft.checked=null; draft.diff=""; draft.message={key:"draftChanged"}; renderRules(); });
$("pause").addEventListener("click",()=>{state.paused=!state.paused; $("pause").textContent=t(state.paused ? "resume" : "pause"); notice(); if (!state.paused) refresh(true);});
$("refresh").addEventListener("click",async()=>{try { await api("/api/v1/refresh","POST"); await refresh(true); setTimeout(()=>refresh(true),700); } catch(error) {state.failure=error.message;notice();}});
$("export").addEventListener("click",()=>{
  if (!state.data) return;
  const link=document.createElement("a"), url=URL.createObjectURL(new Blob([JSON.stringify(state.data,null,2)],{type:"application/json"}));
  link.href=url;link.download="tuntom-fabric-snapshot.json";link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
function connect(token) { state.token=token.trim(); sessionStorage.setItem("tuntom-fabric-token",state.token); refresh(true); }
$("login-form").addEventListener("submit",event=>{event.preventDefault();connect($("token").value);$("token").value="";});
function readTokenLink() {
  const token = new URLSearchParams(location.hash.slice(1)).get("token");
  if (location.hash) history.replaceState(null,"",location.pathname);
  if (token) connect(token);
  return !!token;
}
window.addEventListener("hashchange",readTokenLink);

function deltaText(e,key) {
  if (e?.changes?.resets.includes(key)) return esc(t("counterReset"));
  const delta=e?.changes?.counters[key];
  return delta === undefined ? "—" : "+"+esc(BigInt(delta).toLocaleString(locale()));
}
function renderWarnings() {
  const items=state.warnings.list();
  $("warning-history").hidden=!items.length;
  const present=new Set(state.data?.endpoints.map(e=>e.id) || []);
  const layout=JSON.stringify([language,items.map(item=>[item.id,present.has(item.endpointId)])]);
  // Keep close buttons and keyboard focus stable while only the ages change.
  if (layout !== state.warningsLayout) {
    state.warningsLayout=layout;
    $("warning-list").innerHTML=items.map(item=>{
      const reasons=item.checks.flatMap(check=>checkReasons(item,check));
      const exists=present.has(item.endpointId);
      return `<article class="warning-balloon"><div class="warning-balloon-heading"><span class="attention-mark" aria-hidden="true">!</span><span>${esc(t("recordedWarning"))}</span><time data-warning-age="${item.id}" datetime="${esc(item.sampleId)}"></time><button class="warning-dismiss" data-dismiss-warning="${item.id}" aria-label="${esc(t("dismissWarning",{name:item.name}))}">×</button></div><button class="warning-process" data-warning-process="${esc(item.endpointId)}" ${exists ? "" : "disabled"}>${esc(item.name)} <small>PID ${item.pid}</small>${exists ? " ↗" : ""}</button>${exists ? "" : `<p class="warning-gone">${esc(t("warningProcessGone"))}</p>`}<ul>${reasons.map(reason=>`<li>${esc(reason.text)}</li>`).join("")}</ul></article>`;
    }).join("");
  }
  for (const item of items) {
    const age=$("warning-list").querySelector(`[data-warning-age="${item.id}"]`), seconds=state.warnings.age(item).toLocaleString(locale());
    age.textContent=`−${seconds} s`;
    age.setAttribute("aria-label",t("secondsAgo",{seconds}));
  }
}
$("warning-list").addEventListener("click",event=>{
  const close=event.target.closest("[data-dismiss-warning]"),process=event.target.closest("[data-warning-process]");
  if (close) {
    const focused=document.activeElement === close;
    const next=close.closest("article").nextElementSibling?.querySelector("[data-dismiss-warning]")?.dataset.dismissWarning;
    state.warnings.dismiss(close.dataset.dismissWarning); renderWarnings();
    if (focused) ($("warning-list").querySelector(`[data-dismiss-warning="${next}"]`) || $("warning-list").querySelector("[data-dismiss-warning]") || $("refresh")).focus({preventScroll:true});
  } else if (process && !process.disabled) selectProcess(process.dataset.warningProcess,"overview");
});
function renderHealth() {
  const endpoints=state.data.endpoints, e=selected(), counts={ok:0,warn:0,unknown:0};
  for (const endpoint of endpoints) counts[outdated(endpoint) ? "unknown" : endpoint.health?.level || "unknown"]++;
  const collector=state.data.collector || {mode:"local",uid:"—"};
  $("health-strip").innerHTML=`<div>${Object.entries(counts).map(([level,count])=>`<span class="health-count ${level} ${count ? "nonzero" : ""}"><i class="dot ${level === "ok" ? "" : level === "warn" && count ? "alert" : "dim"}"></i><strong>${count}</strong> ${esc(t("health_"+level))}</span>`).join("")}</div><span class="collector-label">${esc(t(collector.mode === "separate" ? "separateCollector" : "localCollector",{uid:collector.uid}))}</span>`;
  $("detail-diagnostics").disabled=!e;
  const checks=e?.health?.checks.map(check=>outdated(e) ? {...check,state:"unknown",code:"stale",value:null,values:{}} : check);
  $("health-checks").innerHTML=checks?.map(check=>{
    const reasons=check.state === "warn" ? checkReasons(e,check) : [];
    const evidence=reasons.length ? `<ul class="check-reasons">${reasons.map(reason=>`<li>${reason.metric ? `<button data-problem-metric="${esc(reason.metric)}">${esc(reason.text)} ↗</button>` : esc(reason.text)}</li>`).join("")}</ul>` : `${check.value ? `<small>${esc(check.value)}</small>` : ""}${Object.entries(check.values || {}).map(([key,value])=>`<small>${esc(key)} = ${esc(value ?? "—")}</small>`).join("")}`;
    return `<article class="health-check ${check.state}"><span class="eyebrow">${check.state === "warn" ? '<span class="attention-mark" aria-hidden="true">!</span> ' : `<i class="dot ${check.state === "ok" ? "" : "dim"}"></i>`}${esc(t("check_"+check.key))}</span><strong>${esc(t(check.code))}</strong>${evidence}</article>`;
  }).join("") || `<p>${esc(t("chooseProcess"))}</p>`;
  const delta=e?.changes;
  $("delta-window").textContent=delta?.interval_seconds ? t("windowSeconds",{seconds:delta.interval_seconds.toLocaleString(locale(),{maximumFractionDigits:1})}) : "—";
  const faults=warningChecks(e).find(c=>c.key === "errors")?.counters || {};
  const changed=Object.entries(delta?.counters || {}).filter(([,value])=>value !== "0")
    .sort(([a],[b])=>Number(Object.hasOwn(faults,b))-Number(Object.hasOwn(faults,a)) || a.localeCompare(b));
  $("recent-changes").innerHTML=(!delta?.ready || e?.status !== "reachable" || outdated(e) ? `<p>${esc(t(outdated(e) ? "stale" : e?.status !== "reachable" ? "telemetry_missing" : "delta_waiting"))}</p>` :
    changed.length ? `<div class="delta-list">${changed.map(([key])=>`<div class="delta-item ${Object.hasOwn(faults,key) ? "warn" : ""}"><code>${Object.hasOwn(faults,key) ? '<span class="attention-mark" aria-hidden="true">!</span> ' : ""}${esc(key)} ${metricInfo(key)}</code><strong>${deltaText(e,key)}</strong></div>`).join("")}</div>` : `<p>${esc(t("noChanges"))}</p>`)+
    (delta?.resets.length ? `<p>${esc(t("counterReset"))}: ${delta.resets.map(esc).join(", ")}</p>` : "");
}
function relatedSwitch() {
  const e=selected();
  if (e?.kind === "switch") return e;
  const link=state.data?.links.find(link=>link.source === e?.id);
  return state.data?.endpoints.find(endpoint=>endpoint.id === link?.target);
}
function portProcesses(sw,name) {
  return (state.data?.links || []).filter(link=>link.target === sw.id && link.port_id === name)
    .map(link=>state.data.endpoints.find(e=>e.id === link.source)).filter(Boolean);
}
function renderSwitch() {
  const sw=relatedSwitch(), detail=sw?.switch_detail;
  const problems=problemMetrics(sw);
  const expanded=new Set([...$("switch-ports").querySelectorAll("details[open]")].map(node=>node.dataset.port));
  $("switch-title").textContent=sw ? `${sw.name} · PID ${sw.pid}` : t("chooseSwitch");
  $("switch-context").textContent=sw ? [sw.metrics.implementation,sw.switch_socket].filter(Boolean).join(" · ") : "";
  $("switch-rules").disabled=!sw;
  $("switch-queues").innerHTML=["matrix_queues","buffers_in_use","queue_full_drops","pool_stalls","workers_active"].map(key=>
    `<article class="${problems.has(key) ? "queue-problem" : ""}"><span class="eyebrow">${problems.has(key) ? '<span class="attention-mark" aria-hidden="true">!</span> ' : ""}${esc(t(key))} ${metricInfo(key)}</span><strong>${esc(detail?.queues[key] ?? "—")}</strong><small>${key === "queue_full_drops" || key === "pool_stalls" ? deltaText(sw,key) : key === "workers_active" ? "/ "+esc(detail?.queues.workers_pool ?? "—") : ""}</small></article>`).join("");
  $("switch-ports").innerHTML=detail?.ports.length ? detail.ports.map(port=> {
    const processes=portProcesses(sw,port.name);
    const ipc=Object.entries(port).filter(([key])=>key.startsWith("ipc_"));
    const portKey=`${sw.id}:${port.name}:${port.generation}`;
    return `<tr><td><strong>${esc(port.name)}</strong><small class="cell-note">${esc(t("generation",{value:port.generation ?? "—"}))}</small></td><td>${esc(port.kind || "—")}</td><td>${esc(port.rx_owner ?? "—")} / ${esc(port.tx_owner ?? "—")}</td><td>${processes.map(e=>`<button class="port-process" data-port-process="${esc(e.id)}">${esc(e.name)} ↗</button>`).join(" ") || esc(t("noMatchedProcess"))}</td><td>${ipc.length ? `<details data-port="${esc(portKey)}" ${expanded.has(portKey) ? "open" : ""}><summary>${ipc.length} ${esc(t("metrics"))}</summary><table class="ipc-table"><thead><tr><th scope="col">${esc(t("key"))}</th><th scope="col">${esc(t("value"))}</th></tr></thead><tbody>${ipc.map(([key,value])=>`<tr><th scope="row">${esc(key)} ${metricInfo(`port_${port.index}_${key}`)}</th><td>${esc(value ?? "—")}</td></tr>`).join("")}</tbody></table></details>` : "—"}</td></tr>`;
  }).join("") : `<tr><td colspan="5">${esc(t(sw ? "noPortStats" : "chooseSwitch"))}</td></tr>`;
  $("switch-workers").innerHTML=detail?.workers.length ? detail.workers.map(worker=> {
    const ports=detail.ports.filter(p=>p.rx_owner === String(worker.index) || p.tx_owner === String(worker.index));
    const cpu=worker.cpu_percent;
    return `<article class="worker-card"><div class="worker-title"><span>WORKER / ${esc(worker.index)} ${metricInfo(`worker_${worker.index}_cpu_ns`)}</span><strong>${cpu === null ? "—" : esc(cpu.toLocaleString(locale(),{maximumFractionDigits:1}))+" %"}</strong></div><button class="worker-chart" data-worker-chart="${worker.index}" data-chart-process="${esc(sw.id)}" aria-label="${esc(t("chartWorkerExpand",{worker:worker.index}))}" aria-haspopup="dialog" title="${esc(chartValue(cpu,true))} · ${esc(t("chartExpand"))}"><meter min="0" max="100" value="${cpu ?? 0}" aria-hidden="true"></meter><span>${esc(t("chartExpand"))} ↗</span></button><p>${esc(worker.roles === "idle" ? t("workerIdle") : worker.roles)}</p><dl><dt>${esc(t("connectedPorts"))}</dt><dd>${ports.map(port=>esc(port.name)).join(", ") || "—"}</dd><dt>poll calls Δ ${metricInfo(`worker_${worker.index}_poll_calls`)}</dt><dd>${deltaText(sw,`worker_${worker.index}_poll_calls`)}</dd></dl></article>`;
  }).join("") : `<p class="muted">${esc(t("noWorkerStats"))}</p>`;
}
function renderDiagnostics() {
  const e=selected(), logs=state.logs.get(e?.id), report=state.reports.get(e?.id);
  $("diagnostic-title").textContent=e ? `${e.name} · PID ${e.pid}` : t("diagnostics");
  for (const id of ["logs-read","diagnostics-copy","diagnostics-save"]) $(id).disabled=!e || state.diagnosticBusy;
  $("log-meta").textContent=logs?.sampled_at ? new Date(logs.sampled_at).toLocaleString(locale()) : "";
  const logText=logs?.error ? diagnostic(logs.error) : logs?.sources?.length ? logs.sources.map(source=>
    `[${source.source}${source.truncated ? " · "+t("truncated") : ""}]\n${source.text || "—"}`).join("\n\n") : t(logs ? "logsEmpty" : "logsNotRead");
  if ($("logs-output").textContent !== logText) $("logs-output").textContent=logText;
  if (logs?.errors?.length) $("log-meta").textContent+=" · "+logs.errors.map(diagnostic).join(" · ");
  $("diagnostic-status").textContent=messageText(report?.message);
  $("diagnostic-report").hidden=!report?.text;
  if ($("diagnostic-report").value !== (report?.text || "")) $("diagnostic-report").value=report?.text || "";
}
async function readLogs() {
  const e=selected(); if (!e || state.diagnosticBusy) return;
  state.diagnosticBusy=true; renderDiagnostics();
  try { state.logs.set(e.id,await api(`/api/v1/endpoints/${encodeURIComponent(e.id)}/logs`)); }
  catch(error) { state.logs.set(e.id,{error:error.message}); }
  finally { state.diagnosticBusy=false; renderDiagnostics(); }
}
function download(text,name,type="application/json") {
  const link=document.createElement("a"),url=URL.createObjectURL(new Blob([text],{type}));
  link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function diagnosticAction(copy) {
  const e=selected(); if (!e || state.diagnosticBusy) return;
  state.diagnosticBusy=true;
  const report={message:{key:"working"},text:""}; state.reports.set(e.id,report); renderDiagnostics();
  try {
    const result=await api(`/api/v1/endpoints/${encodeURIComponent(e.id)}/diagnostics`);
    report.text=JSON.stringify(result,null,2);
    state.logs.set(e.id,result.logs);
    if (copy) {
      try { await navigator.clipboard.writeText(report.text); report.message={key:"reportCopied"}; }
      catch { report.message={key:"copyFallback"}; }
    } else { download(report.text,`tuntom-diagnostic-${e.pid}.json`); report.message={key:"reportSaved"}; }
  } catch(error) { report.message=error.message; }
  finally { state.diagnosticBusy=false; renderDiagnostics(); }
}
async function openSwitchRules(id) {
  selectProcess(id,"rules");
  if (!draftFor(id).revision) await ruleAction("show");
}
$("topology").addEventListener("click",event=> {
  const node=event.target.closest("[data-node]"),rules=event.target.closest("[data-node-rules]");
  if (node) selectProcess(node.dataset.node,"overview");
  if (rules) openSwitchRules(rules.dataset.nodeRules);
});
$("switch-ports").addEventListener("click",event=>{const button=event.target.closest("[data-port-process]");if(button) selectProcess(button.dataset.portProcess,"overview");});
$("switch-workers").addEventListener("click",event=>{
  const button=event.target.closest("[data-worker-chart]");
  if (button) openChart(button.dataset.chartProcess,Number(button.dataset.workerChart));
});
$("switch-rules").addEventListener("click",()=>{const sw=relatedSwitch();if(sw)openSwitchRules(sw.id);});
$("detail-diagnostics").addEventListener("click",()=>showView("diagnostics"));
$("health-checks").addEventListener("click",event=>{
  const button=event.target.closest("[data-problem-metric]");
  if (button) { $("metric-search").value=button.dataset.problemMetric; renderMetrics(); showView("metrics"); }
});
$("logs-read").addEventListener("click",readLogs);
$("diagnostics-copy").addEventListener("click",()=>diagnosticAction(true));
$("diagnostics-save").addEventListener("click",()=>diagnosticAction(false));

function applyLanguage() {
  document.documentElement.lang=language;
  document.title=t("title");
  document.querySelectorAll("[data-i18n]").forEach(element=>{element.textContent=t(element.dataset.i18n);});
  document.querySelectorAll("[data-i18n-placeholder]").forEach(element=>{element.placeholder=t(element.dataset.i18nPlaceholder);});
  document.querySelectorAll("[data-i18n-aria]").forEach(element=>{element.setAttribute("aria-label",t(element.dataset.i18nAria));});
  document.querySelectorAll("[data-language]").forEach(button=>button.setAttribute("aria-pressed",String(button.dataset.language === language)));
  $("pause").textContent=t(state.paused ? "resume" : "pause");
  $("login-error").textContent=diagnostic(state.loginError);
  if (state.data) render();
  else { renderDetail(); renderMetrics(); renderRules(); notice(); }
}
document.querySelectorAll("[data-language]").forEach(button=>button.addEventListener("click",()=>{
  language=button.dataset.language;
  try { localStorage.setItem("tuntom-fabric-language",language); } catch { /* Preference remains usable in memory. */ }
  applyLanguage();
}));
applyLanguage();
if (!readTokenLink()) {
  const saved = sessionStorage.getItem("tuntom-fabric-token");
  if (saved) connect(saved); else $("login").hidden=false;
}
// Collect independently of tab visibility; browsers may still throttle timers.
// Refresh immediately on return, including after a suspended/backgrounded tab.
function resumeVisiblePage() {
  if (document.hidden) return;
  drawChart();
  refresh();
}
document.addEventListener("visibilitychange",resumeVisiblePage);
window.addEventListener("pageshow",resumeVisiblePage);
setInterval(()=>refresh(),2000);
setInterval(()=>{ if (!document.hidden) renderWarnings(); },1000);
