# Hollowroot

[English](README.md)

> **Статус: экспериментальный.** Бутанимация GSI показывается на экране, `system_server` загружается довольно далеко, но до лаунчера Android пока не доходит. Код изоляции, добавленный после последнего теста на устройстве, на железе не запускался.

Запускает GSI (LineageOS 23.2, Android 16) поверх уже работающего Android без перепрошивки и без доступа к разделам. Загрузчик остаётся заблокированным, ядро и vendor стоковые. Всё, что пишет гость, живёт в оперативной памяти и пропадает после сессии.

Репозиторий содержит порт под **iQOO Z9 Turbo** (V2352A, Snapdragon 8s Gen 3, OriginOS 6). Для других устройств придётся поменять несколько констант, см. [Перенос на другое устройство](#перенос-на-другое-устройство).

> Перед запуском на телефоне с нужными данными прочитайте [Безопасность](#безопасность).

## Как это работает

Второй системы здесь нет, подменяется userspace текущей:

1. Guardian останавливает хостовые SurfaceFlinger, Zygote и службы камеры.
2. Loader уходит в новые namespace: PID, сеть, IPC, UTS и mount.
3. `system.img` из GSI монтируется только на чтение через loop-устройство, `/data` это tmpfs на 512 МБ.
4. Хостовые `/vendor`, `/odm`, активные APEX, GPU и DRM пробрасываются на чтение или как узлы устройств.
5. `pivot_root` делает GSI настоящим корнем. Обычного chroot мало: Zygote сверяет открытые дескрипторы с путями вида `/sys/kernel/tracing/trace_marker` и отбрасывает пути с префиксом chroot.
6. Хостовый binder заменяется приватным binderfs и отдельным `servicemanager`.
7. Loader запускает SurfaceFlinger, bootanimation, маленький property service, затем Zygote, который форкает `system_server`.

Ядро и vendor HAL общие с хостом, поэтому GSI рисует на реальную панель через Qualcomm display composer.

## Guardian

`guardian` возвращает вас в обычную OriginOS. Он работает в пространстве имён хоста:

- Создаёт cgroup v2 (`/sys/fs/cgroup/hollowroot`) и помещает туда loader. Уже существующую группу не берёт.
- Останавливает хостовую графику, Zygote и камеру, ставит `config.disable_cameraservice=true`, чтобы гостевой WindowManager не обращался к вендорскому camera server.
- Запускает loader и ждёт сигнал через pipe, чтобы заметить неудавшийся `exec`.
- Каждые 500 мс пишет в подсветку OLED. После остановки хостового SurfaceFlinger панель гаснет, а включает её обычно `system_server`, которого на раннем этапе ещё нет.
- Следит за кнопками громкости: Vol Up + Vol Down вместе 2 секунды завершают сессию.
- Завершает сессию по таймеру, по SIGINT, SIGTERM, SIGHUP или когда гость вышел.
- При выходе убивает всю cgroup через `cgroup.kill`, ждёт `populated 0` в `cgroup.events`, удаляет группу и только потом запускает хостовые службы. Если смерть гостя подтвердить нельзя, хостовые службы не запускаются.

Длительность сессии от 10 до 300 секунд.

## Статус

Проверено на устройстве до переписывания изоляции (см. Безопасность):

| Часть | Состояние |
| --- | --- |
| Монтирование GSI через loop при SELinux | работает (на время сессии нужен permissive) |
| SurfaceFlinger на хостовом display composer | работает |
| Bootanimation LineageOS на OLED | работает |
| Smoke-тесты (shell, линковщик, `derive_classpath`, старт Zygote) | проходят |
| Загрузка `system_server` | доходит до фазы 500, последний известный блокер: таймаут службы FRP |
| Лаунчер / SystemUI | пока не достигнуты |
| Возврат в OriginOS | работает |

После этого loader сильно переписан (namespace PID/сети/IPC/UTS, binderfs, сброс capabilities, рекурсивный read-only через `mount_setattr`, без хостового `/metadata` и сокетов). **На устройстве это ещё не запускалось.** Возможно, фреймворк или дисплей перестанут стартовать.

Все решённые блокеры описаны в [docs/notes.md](docs/notes.md).

## Безопасность

В раннем тесте гостевой `system_server` делил `/dev/binder` с OriginOS и обратился к хостовому `vold`. Тот не нашёл ключей гостя и пересоздал `/data/media/0`, из-за чего `/sdcard` опустел. Приложения и настройки уцелели, фото нет.

Сейчас у гостя собственный binderfs, а loader прерывается, если узел совпал с хостовым. На железе это не проверено, поэтому:

- Скопируйте `/sdcard` на компьютер и проверьте копию до запуска `start` и `takeover`. Без флага `--i-have-a-backup` скрипт их не запустит.
- `smoke` не останавливает хостовые службы, но настраивает те же namespace.
- Держите телефон заряженным и знайте, как его перезагрузить (кнопка питания 10+ секунд).
- На время сессии включается permissive SELinux и подключаются loop-устройства. Скрипт возвращает enforcing при выходе, но если adb оборвался посреди сессии, проверьте `getenforce`.

## Требования

- Android 14+ с root-доступом. Достаточно временного root от SukiSU / KernelSU, загрузчик может быть заблокирован.
- Ядро с binderfs (Linux 5.0+). Если есть `mount_setattr` (5.12+), он используется, иначе loader прерывается, когда у bind-монтирования есть подмонтирования. На тестовом устройстве ядро 6.1.
- Свободные `/dev/block/loop45` и `/dev/block/loop47` (прописаны в `src/loader.c`).
- Около 3 ГБ в `/data/local/tmp` под образ и 512 МБ свободной RAM под `/data` гостя.
- На компьютере: `adb`, `curl`, `7z`, `make`, Android NDK (r27+), bash.

## Быстрый старт

```sh
git clone https://github.com/NeFaniZzz/hollowroot
cd hollowroot

# при желании поменять ссылку на GSI или путь к NDK
$EDITOR config.env

./hollowroot.sh fetch      # скачать и распаковать GSI
./hollowroot.sh build      # собрать через NDK
./hollowroot.sh push       # залить всё в /data/local/tmp/hollowroot
./hollowroot.sh smoke      # проверка окружения, хостовые службы не трогает
./hollowroot.sh start 120 --i-have-a-backup
./hollowroot.sh logs
```

`start` гасит интерфейс хоста, показывает гостя 120 секунд и возвращает OriginOS. `takeover` вместо `start` запускает только SurfaceFlinger и bootanimation. Первая холодная загрузка `system_server` занимает 60 до 90 секунд, на коротких сессиях она не успеет закончиться.

Если сессия упала и loop-устройства остались подключёнными, выполните `./hollowroot.sh clean`.

### Ссылка на GSI

В `config.env` есть поле `GSI_URL`. По умолчанию там LineageOS 23.2 vanilla ext4 от 2026-05-24 ([MisterZtr](https://github.com/MisterZtr/LineageOS_gsi)) и его sha256. Чтобы взять другой образ, замените ссылку и хеш (или оставьте хеш пустым). Образ должен быть ext4 и от Android 16. Если образ уже распакован, укажите путь в `GSI_IMG`.

### Запуск прямо на телефоне

`hollowroot.sh` только оборачивает adb. На самом телефоне:

```sh
su
sh /data/local/tmp/hollowroot/run_session.sh smoke
HOLLOWROOT_BACKUP_OK=1 sh /data/local/tmp/hollowroot/run_session.sh 120 --framework
```

`run_session.sh` создаёт поддельный FRP-раздел на 512 КБ в `/dev` (tmpfs), подключает два loop-устройства, переключает SELinux в permissive, запускает guardian и всё откатывает при выходе. Уже занятое loop-устройство он не трогает.

После настройки namespace loader отправляет вывод в `/dev/null` (Zygote не принимает обычные файлы как stdio), а logd-сокета у гостя нет, поэтому `logcat` ничего от него не покажет. В `session.log` только сообщения guardian. Вывод самого loader виден в `smoke`, а падения во время сессии смотрите в `dmesg` после неё. Получение логов гостя во время полной сессии пока не решено.

## Структура

```
src/loader.c           namespace, pivot_root, запуск гостя
src/guardian.c         сторож на стороне хоста
src/property_seed.c    экспериментальный property service, loader его не использует
tools/dump_properties.c  выгрузка хостовых свойств для property_seed
tests/test_pivot.c     проверка pivot_root на tmpfs
scripts/run_session.sh запускается на телефоне
hollowroot.sh          запускается на компьютере
config.env             ссылка на GSI, путь к NDK
docs/notes.md          решённые проблемы
```

## Перенос на другое устройство

Значения, зависящие от устройства, лежат в начале `src/guardian.c` и `src/loader.c`:

- `BACKLIGHT_DIR` и `BACKLIGHT_VALUE`, ваш путь ищите в `/sys/class/backlight/`.
- `VOLUME_UP_DEV` / `VOLUME_DOWN_DEV`, смотрите через `getevent -lp`.
- bind-монтирования `/dev/kgsl-3d0`, `/dev/dri`, `/dev/dma_heap` в loader (специфично для Qualcomm).
- имена камерных служб, которые останавливает guardian (`vivocameraserver` это имя от vivo).
- номера loop-устройств `loop45` и `loop47`.

## Известные проблемы

- Loader монтирует хостовые APEX. В старых заметках использовались APEX из самого GSI, потому что в OriginOS ART с версией OAT 260, а GSI нужна 265. Если Zygote падает с `Invalid oat version`, причина в этом, и тогда нужно распаковать APEX из GSI и использовать их.
- Loader прерывается, если что-то не удаётся изолировать. Так задумано, но первые запуски, скорее всего, остановятся на строке `FATAL:`, причину которой придётся искать.
- На время сессии нужен permissive SELinux.
- Wi-Fi, мобильная сеть, звук и камера не настроены.

## Лицензия

[PolyForm Noncommercial 1.0.0](LICENSE). Код можно использовать, изменять и распространять в некоммерческих целях при условии, что сохранены текст лицензии и строка `Required Notice:` из `LICENSE`. Продавать код или продукты на его основе нельзя. Это лицензия с открытым исходным кодом в смысле доступности, но не open source по определению OSI. Сам GSI в репозиторий не входит и имеет свои лицензии.
