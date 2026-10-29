> English version: [BUILD-AND-INSTALL.md](BUILD-AND-INSTALL.md)

# Div2Gamepad: сборка и установка

Мод геймпада для **Divinity II: Developer's Cut** (Steam). Наш
`Div2Patch.dll` — drop-in замена базового патча: форвардит `InitPatch`
в `Div2PatchBase.dll` (переименованный Div2Patch v0.97 + Enhancer
payload) и добавляет свои хуки геймпада/вибрации.

Сторонние компоненты (Div2Patch v0.97, Div2Patch Enhancer) в этот
репозиторий **не входят** — это чужие сборки со своими лицензиями:
- Div2Patch v0.97 — GitHub, автор BlanknameES.
- Div2Patch Enhancer v1.0.0 (payload) — Nexus Mods, автор Gametism.

---

## Часть 1. Установка из бинарей (для игрока)

### Пререквизиты

1. Steam-версия **Divinity II: Developer's Cut**, установленная и
   запускавшаяся хотя бы раз.
2. Уже установленный базовый **Div2Patch v0.97** (+ Enhancer payload,
   если нужен крупный шрифт). Наш мод — надстройка: без базы он не
   заведётся. Установка базы: взять `Div2Patch.dll` из релиза v0.97,
   положить в `<игра>\bin\`, и при желании развернуть payload Enhancer.

### Шаги

1. Закрыть игру и лаунчер Anthology.
2. Перейти в `<игра>\bin\` (например
   `E:\SteamLibrary\steamapps\common\divinity2_dev_cut\bin\`).
3. Сделать бэкап: скопировать текущий `Div2Patch.dll` в
   `Div2Patch.dll.backup` (или в отдельную папку).
4. **Переименовать базовый патч** (один раз):
   - `Div2Patch.dll` → `Div2PatchBase.dll`
   - `Div2Patch.ini` → `Div2PatchBase.ini`
   База читает `<имя_базы>.ini`, т.е. `Div2PatchBase.ini` — это
   переименование обязательно.
5. Скопировать в `bin\` из папки релиза
   (`release/Div2PatchGamepad-vibdamage-load/`):
   - `Div2Patch.dll`
   - `Div2PatchGamepad.ini`
6. Запуск: Steam → **Divinity Anthology** → кнопка
   **«Играть в Divinity II — обычный режим»** (первая из двух кнопок
   «Играть в Divinity II» в лаунчере).
   ⚠️ **НЕ «режим разработчика»** — он запускает
   `Divinity2-debug.exe`, другой бинарник, мод под него не собран
   (краш VEH при старте).

### Проверка, что всё встало

- В меню видна подпись Enhancer/Div2Patch, шрифт крупный (если
  база + payload стоят).
- В `bin\div2gamepad.log` есть строки мода: `DllMain attach`,
  `xinput: pad CONNECTED`, `vibd hook installed`, при загрузке —
  `vibload: start pulse (...)`.
- Геймпад: левый стик ходит, правый стик — курсор, RT — атака/клик,
  Start — ESC. Вибрация: удар по герою → пульс.

### Актуальная сборка

| Файл | sha256 |
|---|---|
| `Div2Patch.dll` | `8f9e41818d125478f88ed4c872b2fb76dc71ea5618d12727832f2acb2343fdd8` |
| `Div2PatchGamepad.ini` | `d2706f99e4f8818ec37c9bcf55ae9154981c8a2bb4cdf4aee1f0a132feed02c3` |

В репозитории — текущая проверенная сборка:
`release/Div2PatchGamepad-vibdamage-load/`.

### Откат

1. Закрыть игру.
2. Вернуть `Div2Patch.dll`/`Div2PatchGamepad.ini` из бэкапа.
3. `Div2PatchBase.dll`/`Div2PatchBase.ini` можно оставить — база
   самодостаточна; либо вернуть имена `Div2Patch.*`.

### Настройка (файл `Div2PatchGamepad.ini`)

| Ключ | Смысл |
|---|---|
| `Vibration=1`, `VibStrength=60` | мастер вибрации, сила в % |
| `VibOnDamage=1`, `VibDamageStrength=0` | вибро на урон (0 = сила от VibStrength) |
| `VibLoad=1`, `VibLoadStart=0` | вибро на старт загрузки/перехода |
| `Diag=0` | 1 = диагностические дампы (STATE/ACT/wd/…), для игры 0 |
| `Trace=0` | 1 = пошаговый trace ввода (очень дорого) |
| `PadLoadFix=1` | обязательно: PC-экран загрузки (иначе чёрный экран) |
| `PadKeepOnLoad=0`, `PadForceFlag=0`, `PadBootMode=0` | не включать (чёрные экраны/краши) |
| `RStickCam=1`, `CamSpeed`, `CamWorldSpeed` | правый стик-курсор и скорости |
| `NativeMove=1`, `StickVirt=1` | нативное движение стиком |

Игра перечитывает ini при старте — после правок перезапускать.

### Если что-то не так

- **Нет вибрации**: проверь `vibd hook installed` в логе, `Vibration=1`,
  `VibOnDamage=1`; ползунок «Тряска» в настройках игры тоже множит силу.
- **Нет пада**: в логе `xinput: pad CONNECTED`; пад должен быть в слоте 0
  (`UserIndex=0`). Виртуальные пады (ViGEm) занимают слот — закрыть.
- **Чёрный экран после загрузки сейва**: убедись `PadLoadFix=1`,
  `PadKeepOnLoad=0`, `PadForceFlag=0`.
- **Краш при старте**: игра запущена «режимом разработчика» — запускать
  обычный режим.
- Лог: `bin\div2gamepad.log`. Начни с `Diag=0` — там только события;
  для глубокой отладки ставь `Diag=1` и `Trace=1`.

---

## Часть 2. Сборка из исходников (для разработчика)

Сборка — кросс-компиляция 32-битной DLL. Проверенный путь: Windows +
WSL2 (Ubuntu). Также работает на Linux напрямую (без WSL-обёртки).

### Зависимости (один раз)

```bash
sudo apt update
sudo apt install -y g++-mingw-w64-i686
```

Проверка: `i686-w64-mingw32-g++ --version` должен отвечать.

### Сборка

Из папки `gamepad_mod/`:

```
build.cmd
```

Скрипт сам переводит путь в WSL-формат (`wslpath`) и печатает sha256
результата. Ручной эквивалент (Linux/WSL, из `gamepad_mod/`):

```bash
i686-w64-mingw32-g++ -shared -O2 -static -static-libgcc \
    -o Div2Patch.dll div2gamepad.c div2gamepad.def -lwinmm
```

Почему именно так:
- `-static-libgcc` — **обязателен**: без него DLL тянет
  `libgcc_s_dw2-1.dll`, и игра молча не загружает мод.
- `-lwinmm` — таймеры/`timeSetEvent`, используемые модом.
- `-static` — не тащить лишние CRT-зависимости.
- Хеш сборки может отличаться от таблицы выше: PE-таймстампы
  (недетерминизм линкера, ~7.5 КБ разницы) — это нормально, функционал
  идентичен. Ориентируйся на «BUILD OK» и работоспособность, а не на
  совпадение sha256.

### Деплой собранной DLL для теста

1. Закрыть игру.
2. Скопировать `Div2Patch.dll` и `Div2PatchGamepad.ini` в `<игра>\bin\`
   (см. Часть 1, шаги 3–6; база `Div2PatchBase.*` уже должна быть).
3. Запуск — обычный режим Anthology.

### Что где лежит

| Путь | Назначение |
|---|---|
| `gamepad_mod/div2gamepad.c` | весь мод (~10.6k строк) |
| `gamepad_mod/div2gamepad.def` | экспорт `InitPatch` |
| `gamepad_mod/Div2PatchGamepad.ini` | дефолтный пресет ключей |
| `gamepad_mod/build.cmd` | сборка одной командой |
| `release/` | готовая проверенная сборка (см. Часть 1) |

### Отладка

- `Diag=1` в ini — периодические дампы (`STATE`, `ACT`, `wd`, `d95`, …).
- `Trace=1` — пошаговый trace ввода (дорого, только для отладки).
- `DbgCmd=1` — файловая консоль: положить `NNN.cmd` в `<игра>\bin\dbgcmd\`
  (глаголы: `ping`, `state`, `vibtest`, `hold/release/pulse`, `rumble`…),
  ответ — `NNN.rsp`.
- Лог `bin\div2gamepad.log`; перед сессией его полезно обнулять.

### Правила тестирования

- Перед смоук-сессией обнулять лог.
- Не оставлять работающих ViGEm-демонов (виртуальный пад занимает
  XInput-слот 0 и ломает управление).
- Синтетические клики в меню: игра читает курсор как raw-физические
  координаты; при масштабе Windows ≠100% клик по скриншоту требует
  пересчёта координат.
