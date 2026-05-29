# UnrealGStreamer

Двусторонняя интеграция GStreamer и Unreal Engine 5.7. Стриминг камер Unreal в GStreamer-пайплайны и приём медиа из GStreamer в Unreal-текстуры.

> **Статус: ранний каркас.** Плагин сейчас умеет подгружать bundled GStreamer-рантайм и проверять наличие нужных элементов при старте. Source- и sink-компоненты ещё не перенесены. Возможны breaking-изменения.

🇬🇧 [English version](README.md)

## Поддерживаемые платформы

| Платформа | Статус |
| --- | --- |
| Windows 64 | Каркас (текущая) |
| Linux | Запланирована |
| macOS | Запланирована |
| Android | Запланирована |

iOS вне области — политика App Store и требования к статической линковке делают интеграцию GStreamer непрактичной.

## Требования

- Unreal Engine **5.7**
- GStreamer **1.24+** runtime (LGPL-набор 1.28.3 включён в `Plugins/UnrealGStreamer/Source/ThirdParty/GStreamer/Win64/`)

Системная установка GStreamer для сборки и запуска на Windows не нужна — всё необходимое поставляется с плагином.

## Структура репозитория

```
GStreamerProject.uproject       Host-проект UE для разработки и демо
Plugins/UnrealGStreamer/        Сам плагин
└── Source/
    ├── GStreamer/              Runtime-модуль
    └── ThirdParty/GStreamer/   Bundled headers, .lib, .dll, plugins
```

Host-проект (`GStreamerProject`) — тонкая обёртка для разработки и тестирования плагина. Папку `Plugins/UnrealGStreamer/` можно положить в любой другой UE 5.7-проект, когда будет готова standalone-упаковка.

## Что работает сейчас

- Runtime-модуль `GStreamer` подгружается при старте UE, делейлоадит bundled GStreamer-DLL, устанавливает путь к плагинам, вызывает `gst_init` и логирует версию рантайма и найденные плагины в категорию `LogGStreamer`.

## Лицензия

Будет выбрана перед первым публичным релизом.
