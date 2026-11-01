# gael - plan

Co robię, po kolei. Decyzje i ich powody są w `docs/SPEC.md`, opis języka w `docs/REFERENCE.md`. Odhaczam tutaj, gdy
krok jest w repo i `./test` przechodzi.

Zasada nadrzędna (2026-10-25): gaela piszą głównie modele (Claude i mały model na jego sprzęcie). Każda zmiana ma
pomagać modelowi pisać kod szybki i bezpieczny: błąd mówi, co wpisać; jeden sposób na rzecz; oczywisty kod jest szybki;
dokumentacja mieści się w kontekście.

## Teraz

- [x] Wyniki `./bench` po ostatnich zmianach w `benchmarks/RESULTS.md` (`1565df1`).
- [x] `task` → `coroutine`: typ `(coroutine T)`, `step-coroutines`, `run-coroutines`, `coroutine-time`; `async`,
      `await`, `sleep`, `yield`, `done?` zostają.
- [x] Gem `ecs` jak flecs: id z generacją, kolumna na komponent, tagi i komponenty jako bity maski, zapytania
      `ecs-each` z `:with` i `:not`; w `gems/` obok binarki, `examples/ecs.gael` (`0ca7018`). Tagi jako bity wygrały z
      nazwami: 1,26 raza mniej instrukcji w VM i 10 razy mniej pamięci.
- [x] `assert`, `_` jako nazwa w `let`, nazwy wbudowane (`:`) jako literały we wzorcach makr (`b59e865`).
- [x] Bez `async`/`await`: `(coroutine (f x))` startuje, `(poll co)` daje `(option T)` od razu; nic nie czeka.
- [x] `repldump.gael`: data i godzina przy każdej formie i które funkcje plików podmieniła; pliki nigdy nie są
      nadpisywane, zmiana na żywo żyje w programie i w dzienniku, jak plik changes w Smalltalku (`ba3b807`).
- [x] Jeden folder `gems/` (jego uwaga: „examples mają folder gems i główny folder ma folder gems, to nie jest
      zgodne”): `examples/` to same programy z `.out`, kod do ponownego użycia to gemy - nowy `geometry` (`Vec2`,
      `Vec3`, `Rect`); gemy-demo i pliki dla testów siedzą w `tests/`, stare `examples/lib` w `trash/`.

## 1. Język pod modele

- [x] Ostrzeżenia, jeden format z błędami (`plik:linia:kolumna: warning: ...`): nieużyta zmienna lokalna (`_` na
      początku nazwy ucisza, `(for (_ 0 3) ...)` powtarza bez nazwy), wartość wyliczona i wyrzucona, kod po `return`
      i `exit`. Program działa dalej; bez flag; `tests/warnings` (`1cfe8c0`).
- [ ] `gael build` (wydanie) z ostrzeżeniem we własnym kodzie się nie buduje - razem z `gael build` w sekcji 2.
- [x] Znaczniki `@`: `(@warn "...")` i `(@err "...")` (bez kodu; `gael -x` je listuje), `(@debug ...)` (kod na czas
      pracy), `(@test "nazwa" ...)` (funkcja, którą woła tylko `gael test`); nieznany znacznik to błąd. `gael build`
      wytnie je, gdy powstanie.
- [x] `gael test PLIK...` (definicje się wykonują, reszta najwyższego poziomu nie; każdy test osobno, ok/FAIL, kod 1
      przy porażce), `gael run`, `gael check`; `examples/testing.gael`.
- [x] `(compile-error "...")` dla makr: błąd kompilacji z własnym komunikatem w miejscu użycia makra.
- [x] Nawiasy: przy brakującym albo nadmiarowym `)` gael czyta wcięcia jak parinfer i wskazuje linię
      (`a ')' is missing at the end of line 4: line 5 is indented as if the ( at 4:8 ended there`) (`0e3e2a2`).
- [ ] Przegląd wszystkich komunikatów: plik:linia:kolumna, co jest źle, poprawna forma.
- [x] Ściąga dla modeli `docs/MODELS.md` (~2,5 tys. tokenów, `gael doc` ją drukuje); test pilnuje, że jest w niej
      każda nazwa z `gael -n` i że ma mniej niż 16 KB.
- [x] Skill `gael` w claude-config (`ed4a92f` tam): pętla check-fix-test, pułapki, gdzie idzie kod, co sprawdza
      `./test`, jego decyzje.
- [x] `gael -m`: plik z rozwiniętymi makrami (także w funkcjach), ułożony do czytania - to, co sprawdza kompilator.
- [x] Metoda na wartości wyrażenia: `((Vec3 1.0 0.0 0.0).cross b)` jak `(v.cross b)` - pisałem to sam, więc model też
      napisze; bez nawiasów `(expr).cross` błąd mówi, że to funkcja i jak ją wywołać.
- [x] Testy gemu w jego pliku: `gael test gems/NAZWA.gael` włącza ten gem i uruchamia jego `(@test ...)`, `@test`
      przed formą zostawia ją dla testów; program, który włącza gem, ich nie kompiluje. `./test` testuje każdy gem z
      `gems/` - testy od razu znalazły błąd w `wfc` (kafelek bez sąsiada przechodził).

## 1b. Biblioteka: porty z holyc do `gems/`

- [x] `pqueue`: kopiec z jego konwencją priorytetów (1 pierwsze, 0 i ujemne na końcu), stabilny przy równych
      (`a1345ec`).
- [x] `events`: `EventLog` (rodzaj, tick, dwie dane; nagrywanie i odtwarzanie, `at`, `since`) i `Schedule` (zdarzenia
      na przyszłe ticki, wyjmowane po ticku) (`a1345ec`).
- [x] `hfsm` w gaelu (płaski FSM to ten sam gem, gdy każdy rodzic to `-`): drzewo stanów, kod wejścia i wyjścia,
      przejście przez wspólnego przodka; stan to wartość enuma, więc siedzi w kolumnie ECS (`examples/guards.gael`).
- [x] `wfc`: wave function collapse (model z kafelkami, do 64 kafelków jako bity), `examples/wfc.gael` (jego prośba:
      „wave function też dodać, koniecznie”).
- [x] `rng` (PCG32, te same liczby co C), `noise` (gradient Perlina 2D i fbm, te same wartości co C), `easing`,
      `curve`; `examples/procgen.gael` robi z nich mapę.
- [ ] ECS dalej, gdy będzie potrzeba: `Changed` (znacznik zmiany kolumny, jak w Bevy), systemy równolegle na fgjob
      (kompilator widzi, które kolumny pisze każde zapytanie), archetypy po generykach.

## 1c. GPU (jego priorytet: „kluczowe mega”, jak w Mojo + wgpu)

- [x] Prototyp w C: wgpu-native (v29) przez `dlopen` liczy kernel WGSL na lavapipe (Vulkan na CPU) - urządzenie,
      bufory, potok, dispatch, odczyt; ABI wgpu w C89 (`src/wgpu89.h`) sprawdzane w `./test` względem `webgpu.h`.
- [x] `(gpu-each (p v) body...)`: ciało na GPU raz na element vec struktur; w kompilacji: liczby 32-bit (`f32`,
      `i32`, `u32`), pola elementu, wartości spoza jako stałe kernela, matematyka, `if` `let` `set`, bez alokacji,
      zapis tylko do swojego elementu. Bez karty (albo bez wgpu) to samo na VM, z tym samym wynikiem (`bd45dda`).
- [x] Emiter WGSL z drzewa po kontroli typów (`gael -g`); kolumny vec-a (SoA) jako osobne bufory, bez przepakowywania.
- [x] Na GPU te same błędy co na VM: dzielenie inta przez zero, float za duży na int, przesunięcie poza bity - kernel
      zapisuje znacznik, nic nie wraca, VM powtarza pętlę i zgłasza błąd z linią (`01c2255`).
- [ ] Dane, które zostają na karcie między klatkami, żeby nie kopiować co klatkę.
- [x] Okno i klatka na Waylandzie: `open-window`, powierzchnia wgpu z `wl_surface`, xdg-shell, klawisze po miejscu
      (evdev), mysz; testy na własnym headless weston w `./test`.
- [ ] Wierzchołki i fragmenty pisane w gaelu.
- [ ] Android, Apple (Metal przez wgpu), przeglądarka (WASM i WebGPU, ten sam WGSL).
- [ ] Dwie pozostałe drogi za tym samym API: SPIR-V z gaela i Vulkan przez `dlopen`; SDL3 GPU (SPIR-V i MSL).

## 2. Jedna binarka i build system

- [ ] `gael build`: jeden plik wykonywalny - kopia gaela z doklejonym programem; działa bez gaela i bez gcc.
- [ ] Plik projektu `(project NAZWA :version ... :main ... :requires (...))`, `gael new`, `gael run`, `gael check`,
      `gael test` (pliki `.out` jak w `examples/` i bloki `(@test "nazwa" ...)` w kodzie).
- [ ] Gemy z gita i ze ścieżki: `gael get`, plik lock (commit i hash), przebudowa tylko tego, co się zmieniło.

## 3. Język

- [ ] `load` / `reload` w działającym programie: podmiana funkcji na żywo z pliku.
- [ ] Generyki (kopia na każdy typ, zero kosztu) i `result`.
- [ ] Biblioteka w gaelu na generykach: `sort`, `sort-by`, `min`, `max`, `range`.
- [ ] Traity jako zbiór haków (`Typ.nazwa`), sprawdzane w kompilacji; comptime: kod wykonywany w trakcie kompilacji
      przez ten sam VM.
- [ ] Wątki, cała prędkość z rdzeni: fgjob (pętla dzielona na kawałki na wszystkie rdzenie) i masterslave (wątek główny
      rozdaje robotę); punkt zbiegu raz na klatkę; kompilator pilnuje, że kawałek pisze tylko do swojego elementu.

## 4. Kod natywny, w tej samej binarce

- [ ] JIT bazowy x86-64: bytecode → kod maszynowy w pamięci, sprawdzenia zostają, podmiana na żywo przez tablicę
      funkcji. Pomiar względem VM, Lua i C.
- [ ] JIT optymalizujący: SSA, przydział rejestrów, inlining, wycinanie sprawdzeń, które da się udowodnić.
- [ ] AOT: ten sam generator, kod maszynowy w pliku z `gael build` zamiast bytecode'u.
- [ ] arm64, riscv64, WASM; opcjonalnie LLVM (tekst `.ll`), gdy jest zainstalowany.

## 5. Potem

- [ ] Gem FSM/HFSM napisany w gaelu.
- [ ] artemacs: rdzeń w C (okno, wejście, rysowanie), reszta w gaelu; bufory i tryby jako gemy - inspektor jak w
      Godocie (wartości `@export` działającej gry, zmiana od razu w grze; dziś tekstowo `gael -x`), drzewo sceny,
      viewport 3D, wektor, raster, tekst, terminal.
- [ ] GPU: sekcja 1c.
- [ ] Ownership (owned / borrowed / shared) - opcjonalny, jeśli liczniki, areny i uchwyty nie wystarczą.

## Zrobione ostatnio

- [x] Szybszy VM: computed goto, superinstrukcje, warunki jako skoki, nowa mapa (`173d9e9`, `e7286ba`).
- [x] Bool jako bit w vecach i pulach (`7fa7bed`, `88e71bd`).
- [x] `./bench` względem C, Lua, Pythona, Lispów, JS i GDScriptu.
- [x] Metody i haki, wartości domyślne pól, x-makra (`string-of`, `name-of`), funkcje matematyczne.
