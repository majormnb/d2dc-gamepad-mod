> English version: [README.md](README.md)

# d2dc-gamepad-mod

Мод геймпада для **Divinity II: Developer's Cut** (Steam): управление
пада поверх базового Div2Patch v0.97 — движение стиком, курсор на
правом стике, клики триггерами, вибрация на урон и на старт
загрузки/перехода.

## Установка и сборка

Полное руководство — [docs/BUILD-AND-INSTALL.ru.md](docs/BUILD-AND-INSTALL.ru.md)
(русский; английская версия:
[docs/BUILD-AND-INSTALL.md](docs/BUILD-AND-INSTALL.md)).

- **Готовая сборка**: `release/Div2PatchGamepad-vibdamage-load/` —
  `Div2Patch.dll` + `Div2PatchGamepad.ini` + README. Скопировать в
  `<игра>\bin\`, запускать игру в обычном режиме лаунчера Anthology.
- **Сборка из исходников**: `gamepad_mod/build.cmd` (WSL + mingw):

        i686-w64-mingw32-g++ -shared -O2 -static -static-libgcc \
            -o Div2Patch.dll div2gamepad.c div2gamepad.def -lwinmm

- **Требование**: в игре уже установлен базовый Div2Patch v0.97,
  переименованный в `Div2PatchBase.dll` / `Div2PatchBase.ini` — наш
  `Div2Patch.dll` форвардит ему `InitPatch` и добавляет свои хуки.

## Возможности

- Левый стик — движение; правый стик — курсор; RT/LT — ЛКМ/ПКМ
  (атака/контекст и выбор в меню); Start — ESC; крестовина — квикбар
  (Quickmap 5-8).
- Вибрация (своя, движковая на PC мертва): удар по герою и старт
  загрузки мира / перехода локации.
- Фиксы: `PadLoadFix` (чёрный экран загрузки сейва), `StickyFix`
  (залипание героя), `CamHold` (камера после нажатий пада).
- Тихий лог: `Diag=0` — в файле только события.

## Структура

| Путь | Что это |
|---|---|
| `gamepad_mod/` | Исходник мода `div2gamepad.c`, `div2gamepad.def`, `build.cmd`, дефолтный `Div2PatchGamepad.ini` |
| `docs/` | Руководство по сборке и установке |
| `release/Div2PatchGamepad-vibdamage-load/` | Готовая проверенная сборка + README |

## Как устроено

`Div2Patch.dll` — drop-in замена базового патча: chainload'ит настоящий
payload как `Div2PatchBase.dll` и добавляет свои хуки. Опорные точки
движка: `NiInputDI8GamePad` (DirectInput-бэкенд), насос событий ввода
@ `0x8EBB34`, диспетчер действий @ `0x8E7710`; exe грузится с ребейзом,
хуки считают дельту в рантайме. Вибрация — `XInputSetState`.
