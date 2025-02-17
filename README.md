# Bit_Torrent_MPI
Am ales sa organizez informatiile in doua structuri de date:
-PeerData: stochează informațiile specifice fiecărui client:
    -my_files_count: numarul de fisiere detinute de peer
    -my_files: lista cu numele fisierelor pe care peer-ul le detine
    -new_files_count: numarul de fisiere pe care peer-ul dorește sa le descarce
    -new_files: lista cu numele fisierelor pe care peer-ul vrea sa le descarce
    -files: un map care asociaza un fisier cu segmentele sale, fiecare segment este identificat printr-un numar și un hash
    -seg_pos: un map care leaga hash-ul fiecarui segment de indexul sau din fisier
    -seg_map: pastrează numarul de segmente pentru fiecare fisier
    -trafic: nivelul de ocupare al peer-ului (numarul de segmente trimise altor peers)
TrackerData: informatiile globale gestionate de tracker:
    -files: segmentele fisierelor detinute de peers, organizate pe numele fisierelor
    -seg: numarul de segmente din fiecare fisier, impartit pe peer
    -user_file_count: numarul total de fisiere detinute de fiecare peer
    -seeders: o lista de peers care pot furniza segmente pentru un fisier
    -done_peer: numarul de peers care au terminat descarcarea tuturor fisierelor

PEER:
La inceput, fiecare peer citeste fisierele pe care le detine si stocheaza hash-urile segmentelor intr-o structura 
unordered_map - files ce leaga numele fisierelor detinute de hashurile corespunzatoare. Aceasta structura este specifica 
fiecarui peer, deoarece fiecare detine fisiere diferite la inceput.

Dupa ce isi incarca datele, fiecare peer trimite tracker-ului date:
    -Numarul de fisiere detinute
    -Numele fiecarui fisier
    -Dimensiunea fisierului (in segmente)
    -Hash-urile segmentelor pentru fiecare fisier
Tracker-ul primeste aceste date si raspunde cu un ack pentru a confirma primirea. Cand peer-ul primeste confirmarea, 
incepe procesul de download/upload a datelor.

TRACKER:
Colecteaza informatiile de la toti clientii despre fisierele detinute. Dupa ce finalizeaza primirea acestor date, intra 
intr-un while, unde gestioneaza cererile de la peers.

Aceste cereri sunt tratate in 3 situatii:
    -REQUEST_SEEDS: Un peer doreste sa descarce un fisier si cere informatii despre seeders (numele lor, numarul de 
    segmente detinute si nivelul trafic). Tracker-ul trimite aceste informatii clientului, care decide apoi de la cine va 
    descarca.
    -UPDATE_STATUS: Peer-ul notifica tracker-ul ca a descarcat segmente noi sau ca a terminat descarcarea unui fisier. 
    Tracker-ul actualizeaza datele.
    -FINISHED_DOWNLOADING_ALL_FILES: Peer-ul informeaza tracker-ul ca a terminat de descarcat toate fisierele dorite. 
    Tracker-ul creste numarul de clienti care au finalizat descarcarea.
Cand toti clientii finalizeaza descarcarile, programul se opreste.

DOWNLOAD:
Se bazeaza pe un loop care gestioneaza descarcarea segmentelor din fisierele dorite.

Peer-ul solicita informatii despre seeders de la tracker pentru un fisier specific (nume, segmente detinute si trafic).
Apoi selecteaza un seeder pe baza load-ului (trafic minim, cel mai putin ocupat seeder) si descarca cate 10 segmente de 
la fiecare seeder. Aceasta abordare rezolva descarcarea optima si distribuirea echitabila intre seeders.
Dupa descarcarea a 10 segmente, peer-ul trimite tracker-ului o actualizare a statusului, respectiv cate segmente a 
descarcat din fisier.
Daca un fisier este complet descarcat (numarul de segmente descarcate = dimensiunea totala a fisierului), fisierul este   
eliminat din lista de fisiere dorite si salvat in output-ul clientului.

UPLOAD:
Avem un loop continuu care gestioneaza cererile de segmente primite de la alti peers din retea. Procesul ruleaza pana 
cand tracker-ul trimite un semnal de stop.
In timpul procesului, peer-ul gestioneaza ajustarea traficului prin mesaje de tip INCREASE_LOAD si DECREASE_LOAD. Aceste 
mesaje indica faptul ca un alt peer solicita sau finalizeaza descarcarea segmentelor. Peer-ul ajusteaza nivelul intern de 
trafic pentru a reflecta aceste cereri, asigurand o distribuire echilibrata a load-ului.
Pentru cererile de segmente, peer-ul proceseaza fiecare solicitare individual. Mai intai, primeste numele fisierului si 
indicele segmentului solicitat. Daca detine segmentul, peer-ul trimite hash-ul segmentului catre peer-ul care a facut 
solicitarea. In cazul in care segmentul nu este gasit, peer-ul continua procesarea altor cereri pentru a evita blocajele.
Asffel se realizeaza colaborarea eficienta intre peers, fara a mai fi nevoie de tracker.

Pentru a diferentia mesajele MPI intre ele, am folosit valori diferite pentru tag-uri:
-100: folosit la peers, pentru a trimite informatie catre tracker la initializare
-101: ack, trackerul le spune clientilor ca a primit informatia
-200: peers cer date despre seeders
-201 : trackerul raspunde cererilor
-300: seederul anunta ca va trimite segmente
-400: un peer anunta seederul sa si creasca traficul cand cere segmente de la el
-401: peer-ul ii zice seederului cand a terminat descarcarea, pentru ca acesta sa-si scada traficul
-500: clientul ii da update tracker-ului
-501: un client a terminat de descarcat tot ce avea nevoie
-600: procesul de incheie, iar trackerul le zice clientilor sa opreasca uploadul
