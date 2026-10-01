Folgende Sequenz wurde nach einem Neustart der Heizung Proxon P1 ermittelt:

|ID|DP| Typ | byteTyp (11,22) | Wertebereich | Kommentar
| :--- | :--- | :--- | :--- | :--- | :--- |
| 1 | 032e | 10/22 | datetime | Datum/Zeit | Gerätezeit |
| 2 | 0226 | 11/23 | float32 | (20-> 23,6 ) | IST Temperatur|
| 3 | 0227 | 11/23 | float32 | (18 > 30 ) | SOLL Temperatur|
| 4 | 0229 | 11/23 | float32 | 21 | Soll Temperatur ? |
| 5 | 022a | 11/23 | float32| (18 > 21) | Soll Temperatur ? |
| 6 | 020a | 11/23 | uint16 | [0,1,2,3] | Betriebsart |
| 7 | 022c | 11/23 | uint16 | [0,1] | |
| 8 | 006c | 10/22 | uint32 | 8 Bits | |
| 9 | 0208 | 10/22 | uint32 | 32 Bits | |
| 10 | 01f8 | 11/23 | float32 | = 0 | BDE Status Word ?|
| 11 | 00e1 | 11/23 | uint16 | [0,1,2,3,4] | Lüfterstufe |
| 12 | 03b7 | 10/22 | uint16 | Arr10[10 Temp.]/10 | T1-T15 |
| 13 | 00c9 | 10/22 | float32 | Arr2[1500,2700] | IST Drehzahl (Zuluft?) |
| 14 | 00d7 | 10/22 | float32 | Arr2[0k,5k,7k,10k ] | Ziel Drehzahl (Zuluft?) |
| 15 | 00ed | 10/22 | uint32 | [102 -> 87] | Filter Restlaufzeit [Tage] |
| 16 | 00ee | 10/22 | float32 | = 0 | |
| 17 | 03b6 | 11/23 | float32 | = 0 | |
| 18 | 02d1 | 10/22 | uint32 | [154, 236] | Betriebszeit Stufe2 [h] |
| 19 | 02d2 | 10/22 | uint32 | [960, 1140] | Betriebszeit Stufe3 [h] |
| 20 | 02d0 | 10/22 | uint32 | [8,13] | Betriebszeit Stufe1 [h] |
| 21 | 02d3 | 10/22 | uint32 | [845, 925] | Betriebszeit Stufe4 [h] |
| 22 | 02d4 | 10/22 | uint32 | = 20 | Betriebszeit Wärmepumpe Heizen [h]|
| 23 | 02d5 | 10/22 | uint32 | = 658 | Betriebszeit Wärmepumpe Kühlen [h]|
| 24 | 02d7 | 10/22 | uint32 | [2100, 2402] | Betriebszeit Steuerung [h] |
| 25 | 02d9 | 10/22 | uint32 | = 1 | Betriebszeit Vorwärme [h]|
| 26 | 02df | 10/(22) | uint32 | Array28 | Betriebszeiten Len=224 |
| 27 | 02d6 | 10/22 | uint16 | Arr20[21,1,0..0] | Betriebszeitenliste Steuerung  ?|
| 28 | 0160 | 10/22 | uint8 | [0,1] | Sommer Bypass Status |
| 29 | 0168 | 10/22 | uint8 | = 2 | Schieber-Position ? |
| 30 | 0120 | 10/22 | uint16 | [0->80] | |
| 31 | 01f5 | 10/22 | uint32 | = 67 | |
| 32 | 00d2 | 10/22 | float32 | [31,50,70,100] | Lüfterstufen (Zuluft?) |
| 33 | 00d3 | 10/22 | float32 | [25,50,70,100] | Lüfterstufen (Abluft?) |
| 34 | 0110 | 10/22 | float32 | = 3 | |
| 35 | 0105 | 10/22 | float32 | = 0 | |
| 36 | 0115 | 10/22 | float32 | = 0.39 | ? |
| 37 | 011c | 10/22 | float32 | = 2 | |
| 38 | 0116 | 10/22 | uint16 | = [100,90] | |
| 39 | 0178 | 10/22 | uint16 | [80,15,29,45,65,65,0] | |
| 40 | 0519 | 10/22 | uint16 | [0,72] | |
| 41 | 051c | 10/22 | float32 | = 0 | Kompressor Drehzahl ?|
| 42 | 0330 | 10/22 | ? | ? | |
| 43 | 0123 | 10/22 | uint16 | = 0 | |
| 44 | 020f | 10/22 | uint16 | = 0 | |
| 45 | 051d | 10/22 | float32 | = 0 | |
| 46 | 051e | 10/22 | float32 | = 0 | |
| 47 | 0191 | 11/e3 | uint16 | = 0| ? |
| 48 | 0194 | 10/e2 | uint16 | = 5 | ? |


Eine Sequenz dauert also genau 4,8 Sekunden.
