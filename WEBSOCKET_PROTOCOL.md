# Komunikaty WebSocket: ESP32 ↔ telefon

Połączenie: telefon łączy się z `ws://<ip-skanera>:8765/`.
Wszystkie komunikaty to **ramki tekstowe** z jednym słowem: bez JSON-a, bez cudzysłowów
i bez znaku nowej linii. Wielkość liter ma znaczenie.

Istnieją tylko **dwa** komunikaty.

## ESP32 → telefon

### `take_photo`

| | |
|---|---|
| Znaczenie | „Zrób teraz jedno zdjęcie” |
| Kiedy ESP to wysyła | podczas skanu: po każdym obrocie stołu i odczekaniu 3 s na uspokojenie drgań (domyślnie 50 razy na obrót); oraz ręcznie, po kliknięciu „Take Photo” na stronie (`POST /camera/photo`) |
| Co telefon ma zrobić | zrobić zdjęcie, zapisać je, a potem odesłać `photo_taken` |
| Limit czasu | telefon ma **10 s** na odpowiedź `photo_taken` |
| Gdy brak odpowiedzi | skan zostaje przerwany z błędem `Telefon nie potwierdził zdjęcia` |

ESP nie wysyła razem z tym komunikatem nazwy pliku ani numeru zdjęcia, więc aplikacja numeruje zdjęcia sama
(np. `image_0000.jpg`, `image_0001.jpg`, …).

## Telefon → ESP32

### `photo_taken`

| | |
|---|---|
| Znaczenie | „Zdjęcie zrobione i zapisane” |
| Kiedy wysłać | dopiero po zapisaniu zdjęcia, jako odpowiedź na `take_photo` |
| Co robi ESP | czeka 2 s, obraca stół do następnej pozycji i po kolejnych 3 s wysyła następne `take_photo` |
| Wysłane za późno lub bez prośby | jest ignorowane |

## Przebieg

```
telefon ── łączy się ──────────────────▶ ESP
ESP     ── take_photo ─────────────────▶ telefon
telefon ── photo_taken ────────────────▶ ESP      (w ciągu 10 s)
          … ESP obraca stół, czeka …
ESP     ── take_photo ─────────────────▶ telefon
telefon ── photo_taken ────────────────▶ ESP
          … i tak dalej, aż do końca skanu
```

## Do zapamiętania przy pisaniu aplikacji

- W danej chwili ESP czeka najwyżej na jedno zdjęcie. Kolejne `take_photo` przychodzi
  dopiero po `photo_taken`.
- Nie ma komunikatu „zdjęcie nie wyszło”. Jeśli aparat zawiedzie, aplikacja może spróbować jeszcze raz
  w ciągu 10 s albo nic nie wysyłać, a wtedy skan zatrzyma się po upływie limitu.
- Każdy inny tekst wysłany przez telefon ESP tylko zapisze w logu i zignoruje.
  Komunikat dłuższy niż 1024 bajty spowoduje zerwanie połączenia.
- Aktywny jest tylko telefon, który połączył się jako ostatni.
- ESP odpowiada na ping, ale sam go nie wysyła. Ustaw ping po stronie telefonu, np. w OkHttp
  `pingInterval(10, TimeUnit.SECONDS)`, a po rozłączeniu łącz się ponownie automatycznie.
- Jeśli telefon nie będzie połączony, gdy przyjdzie pora na zdjęcie, skan zatrzyma się z błędem
  `Telefon nie jest podłączony`.

## Przykład w Kotlinie (OkHttp)

```kotlin
override fun onMessage(ws: WebSocket, text: String) {
    when (text) {
        "take_photo" -> takePhoto { ws.send("photo_taken") } // wyślij po zapisaniu pliku
        else -> {}                                           // nieznany komunikat: zignoruj
    }
}
```
