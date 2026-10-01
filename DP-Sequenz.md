Folgende Sequenz wurde nach einem Neustart der Heizung ermittelt:

|DP| Typ | byteTyp | Wertebereich | Kommentar
| :--- | :--- | :--- | :--- | :--- |
| 032e | 10/22 | DATETIME | Datum/Zeit | Gerätezeit |
| 0226 | 11/23 | | (20-> 23,6 ) | IST Temperatur|
| 0227 | 11/23 | | (18 > 30 ) | SOLL Temperatur|
| 0229 | 11/23 | | 21 | Soll Temperatur ? |
| 022a | 11/23 | | (18 > 21) | Soll Temperatur ? |
| 020a | 11/23 | | [0,1,2,3] | Betriebsart |
| 022c | 11/23 | | [0,1] | |
| 006c | 10/22 | | 8 Bits | |
| 0208 | 10/22 | | 32 Bits | |
| 01f8 | 11/23 | | len = 0 | |
| 00e1 | 11/23 | | [0,1,2,3,4] | Lüfterstufe |
| 03b7 | 10/22 | | Arr10[10 Temp.]/10 | T1-T15 |
| 00c9 | 10/22 | | Arr2[1500,2700] | IST Drehzahl (Zuluft?) |
| 00d7 | 10/22 | | Arr4[0k,5k,7k,10k] | Ziel Drehzahl (Zuluft?) |
| 00ed | 10/22 | | [102 -> 87] | Filter Restlaufzeit [Tage] |
| 00ee | 10/22 | | = 0 | |
| 03b6 | 11/23 | | = 0 | |
| 02d1 | 10/22 | | [154, 236] | Betriebszeit Stufe2 [h] |
| 02d2 | 10/22 | | [960, 1140] | Betriebszeit Stufe3 [h] |
| 02d0 | 10/22 | | [8,13] | Betriebszeit Stufe1 [h] |
| 02d3 | 10/22 | | [845, 925] | Betriebszeit Stufe4 [h] |
| 02d4 | 10/22 | | = 20 | Betriebszeit Wärmepumpe Heizen [h]|
| 02d5 | 10/22 | | = 658 | Betriebszeit Wärmepumpe Kühlen [h]|
| 02d7 | 10/22 | | [2100, 2402] | Betriebszeit Steuerung [h] |
| 02d9 | 10/22 | | = 1 | Betriebszeit Vorwärme [h]|
| 02df | 10/(22) | | Array28 | Betriebszeiten Len=224 |
| 02d6 | 10/22 | | Arr20[21,1,0..0] | Betriebszeitenliste Steuerung |
| 0160 | 10/22 | | [0,1] | |
| 0168 | 10/22 | | = 2 | |
| 0120 | 10/22 | | [0->80] | |
| 01f5 | 10/22 | | = 67 | |
| 00d2 | 10/22 | | = 4 | |
| 00d3 | 10/22 | | = 4 | |
| 0110 | 10/22 | | = 1 | |
| 0105 | 10/22 | | = 1 | |
| 0115 | 10/22 | | ? | |
| 011c | 10/22 | | = 2 | |
| 0116 | 10/22 | uint16 | [100,90] | |
| 0178 | 10/22 | | [80,15,29,45,65,65,0] | |
| 0519 | 10/22 | | [0,72] | |
| 051c | 10/22 | | = 0 | |
| 0330 | 10/22 | | ? | |
| 0123 | 10/22 | | = 0 | |
| 020f | 10/22 | | = 0 | |
| 051d | 10/22 | | = 0 | |
| 051e | 10/22 | | = 0 | |
| 0191 | 11/e3 | | len = 0 | |
| 0194 | 10/e2 | | = 5 | |
| -- | -- | -- | -- | -- |
