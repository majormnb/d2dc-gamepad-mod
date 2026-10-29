# Div2PatchGamepad — vibdamage + vibload (2026-10-29)

Рабочая сборка мода геймпада для Divinity II: Developer's Cut:
вибрация на урон и на старт загрузки/перехода; вибрации на RT и «шага
курсора» в меню нет; тихий лог (`Diag=0`).

## Файлы и хеши (sha256)

| Файл | sha256 |
|---|---|
| `Div2Patch.dll` | `8f9e41818d125478f88ed4c872b2fb76dc71ea5618d12727832f2acb2343fdd8` |
| `Div2PatchGamepad.ini` | `d2706f99e4f8818ec37c9bcf55ae9154981c8a2bb4cdf4aee1f0a132feed02c3` |

## Что нового

1. **Вибрация на урон** (подтверждена в живом бою): детур на колл-сайте
   показа попапа урона; красный попап (`color=0xFF0000`) = урон игроку →
   пульс XInputSetState (сила `VibDamageStrength`, 0 = от `VibStrength`).
   В коде движка меняется одна команда `E8`.
2. **Вибрация на старт загрузки/перехода**: короткий пульс (200 мс) на
   первый `openPopUp` загрузки и на `RegionSwap_Start` (переходы между
   локациями); дебаунс 5 с — один переход, один пульс. Пульса на
   «прибытие» нет (решение пользователя).
3. **Убрано**: вибрация на RT (атака/подтверждение) и «нативная»
   навигация меню крестовиной. Меню — как раньше: правый стик = курсор,
   RT = клик.
4. **Тихий лог**: периодические дампы (STATE/ACT/wd/T46/A48/AV/d95 и
   пр.) под `Diag=0` — в файле лога почти только события.

## Установка

1. Закрыть игру.
2. Скопировать `Div2Patch.dll` и `Div2PatchGamepad.ini` в
   `E:\SteamLibrary\steamapps\common\divinity2_dev_cut\bin\`.
3. Запуск: Steam → Divinity Anthology → **«Играть в Divinity II —
   обычный режим»** (НЕ «режим разработчика»: это другой бинарь).
4. Требуется уже установленный базовый Div2Patch v0.97, переименованный
   в `Div2PatchBase.dll` / `Div2PatchBase.ini` (мод форвардит ему
   `InitPatch`).

## Вибрация: ключи ini

    Vibration=1            ; мастер
    VibStrength=60         ; базовая сила, %
    VibOnDamage=1          ; вибро на урон
    VibDamageStrength=0    ; 0 = сила от VibStrength
    VibLoad=1              ; вибро на старт загрузки/перехода
    VibLoadStart=0         ; 0 = сила от VibStrength

## Откат

1. Закрыть игру.
2. Вернуть предыдущую пару `Div2Patch.dll` / `Div2PatchGamepad.ini`
   из бэкапа.

## Примечания

- Лог: `bin\div2gamepad.log`; при `Diag=0` ищите `vibd:` / `vibload:`.
- Сборка: `gamepad_mod\build.cmd` (WSL MinGW, `-static-libgcc -lwinmm`).
