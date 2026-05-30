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
- Send copy-path компоненты: `UGstPipelineComponent` (собирает пайплайн из `gst-launch`-строки) и `UGstAppSrcComponent` (читает render target из `SceneCaptureComponent2D` обратно в CPU и пушит BGRA-буфер в `appsrc`). Это baseline-путь, с которым потом будем сравнивать zero-copy.

## Performance baseline (copy path)

Send copy-path измеряет стоимость пути GPU → CPU → GStreamer. `UGstAppSrcComponent` инструментирует каждый кадр и раз в `MetricsLogIntervalFrames` тиков (по умолчанию 60) пишет heartbeat-строку в `LogGStreamer`. Метрики (mean + p95 по скользящему окну 60 кадров):

- `gpu` — чистое время `RHICmdList.ReadSurfaceData`, замер на render thread внутри enqueue'нутой команды. Это стоимость, которую должен убрать zero-copy.
- `e2e` — end-to-end latency от сабмита на game thread до `gst_app_src_push_buffer`. Сейчас в основном — интервал опроса fence'а (компонент проверяет `IsFenceComplete` раз в свой тик).
- `push` — wall-clock внутри `gst_app_src_push_buffer`.
- `fps` — по интервалам между тиками компонента.
- `queue=cur/max` — пиковая глубина очереди ожидающих readback'ов в окне, против `MaxQueueLength`.
- `pushed` / `dropped` — кумулятивные счётчики. `dropped` инкрементируется когда очередь полна на момент сабмита.

Пример строки в логе:

```
LogGStreamer: copy-path: fps=25.0 gpu=14.5ms p95=15.7 e2e=80.0ms p95=84.5 push=0.01ms p95=0.02 queue=2/5 pushed=600 dropped=0
```

Референсные числа (1920×1080 BGRA, NVIDIA RTX 4070 SUPER, Windows 11, UE 5.7, TickInterval = 1/25): `gpu ≈ 14–15мс`, `push ≈ 0.01мс`. Конкретное сравнение появится когда будет zero-copy путь.

Plain-C log bridge перенаправляет внутренний debug-вывод GStreamer в `LogGStreamer` — ошибки пайплайна, warnings, фейлы регистрации плагинов видны в обычном логе UE.

## Лицензия

Будет выбрана перед первым публичным релизом.
