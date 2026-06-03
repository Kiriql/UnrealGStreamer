# UnrealGStreamer

Двусторонняя интеграция GStreamer и Unreal Engine 5.7. Стриминг камер Unreal в GStreamer-пайплайны и приём медиа из GStreamer в Unreal-текстуры.

> **Статус: ранний каркас.** Send copy-path работает на Windows. Send zero-copy (hybrid, без CPU-копий) работает на Windows на D3D12. Приёмные компоненты и остальные платформы пока не перенесены. Возможны breaking-изменения.

🇬🇧 [English version](README.md)

## Поддерживаемые платформы

| Платформа | Статус |
| --- | --- |
| Windows 64 | Send: copy-path + zero-copy (D3D12) |
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
- Send copy-path компоненты: `UGstPipelineComponent` (собирает пайплайн из `gst-launch`-строки) и `UGstAppSrcComponent` (читает render target из `SceneCaptureComponent2D` обратно в CPU и пушит BGRA-буфер в `appsrc`).
- Send zero-copy компонент (Windows, D3D12): `UGstZeroCopyVideoSourceComponent`. Оборачивает GPU-память как `GstMemory` через GStreamer-плагин `d3d12` и пушит в `appsrc` без CPU readback'а. Архитектура — ниже.

## Send copy-path (baseline)

`UGstAppSrcComponent` измеряет стоимость пути GPU → CPU → GStreamer и раз в `MetricsLogIntervalFrames` тиков (по умолчанию 60) пишет heartbeat-строку в `LogGStreamer`. Метрики (mean + p95 по скользящему окну 60 кадров):

- `gpu` — чистое время `RHICmdList.ReadSurfaceData`, замер на render thread внутри enqueue'нутой команды.
- `e2e` — end-to-end latency от сабмита на game thread до `gst_app_src_push_buffer`. Сейчас в основном — интервал опроса fence'а (компонент проверяет `IsFenceComplete` раз в свой тик).
- `push` — wall-clock внутри `gst_app_src_push_buffer`.
- `fps` — по интервалам между тиками компонента.
- `queue=cur/max` — пиковая глубина очереди ожидающих readback'ов в окне, против `MaxQueueLength`.
- `pushed` / `dropped` — кумулятивные счётчики.

Пример строки в логе:

```
LogGStreamer: copy-path: fps=25.0 gpu=14.5ms p95=15.7 e2e=80.0ms p95=84.5 push=0.01ms p95=0.02 queue=2/5 pushed=600 dropped=0
```

## Send zero-copy (Windows, D3D12)

`UGstZeroCopyVideoSourceComponent` держит кадр на GPU end-to-end: через границу UE → GStreamer проходит только обёртка над существующим `ID3D12Resource`, синхронизированная с очередью GStreamer через `ID3D12Fence`. Никакого `ReadSurfaceData`. Никакого `memcpy`.

### Архитектура

- `IZeroCopyBackend` (`Private/ZeroCopy/IZeroCopyBackend.h`) — платформо-независимый контракт: `AllocSharedTexture`, `WrapExternalTextureAsGstMemoryWithFence`, `FreeSharedTexture`.
- `FD3D12ZeroCopyBackend` (`Private/ZeroCopy/Windows/D3D12Backend.cpp`) — реализация на Windows. Один `ID3D12Fence` переиспользуется между кадрами, значение монотонно растёт на каждый push.
- `D3D12GstBridge.cpp` — отдельный TU (без UE-хедеров, только `<gst/gst.h>` + `<gst/d3d12/gstd3d12.h>` + `<d3d12.h>`). Оборачивает `ID3D12Resource` в `GstMemory` через `gst_d3d12_allocator_alloc_wrapped` и привязывает fence через `gst_d3d12_memory_set_fence`. UE- и GStreamer-хедеры разнесены по разным TU, чтобы избежать конфликта символа `GError` между glib и UE Core.
- `UGstZeroCopyVideoSourceComponent` тикает на game thread, при старте pipeline'а аллоцирует один shared `B8G8R8A8_UNORM` resource, каждый кадр enqueue'ит render-команду: transition source RT → CopySrc, transition shared → CopyDest, `RHICopyTexture(UE RT → shared)`, сигналим fence на graphics queue UE через `RHIRunOnQueue` + `ID3D12CommandQueue::Signal`, оборачиваем shared resource в `GstMemory` с этим значением fence'а, пушим.

У компонента два режима:

- **`SourceRT` (production).** Привязываешь `UTextureRenderTarget2D`, в который пишет `USceneCaptureComponent2D`, и компонент каждый кадр копирует его в свой shared resource.
- **Synthetic (debug).** Активен если `SourceRenderTarget` пустой. Компонент сам рисует движущийся HSV-цвет в shared resource. Удобно для проверки gst-стороны пайплайна без сцены.

### Почему одна GPU-копия, а не «вообще без копий»

Честно — обернуть саму UE-шную render target напрямую в UE 5.7 D3D12 невозможно:

- UE аллоцирует все RT'ы как `B8G8R8A8_TYPELESS` для гибкости sRGB/linear views.
- Pre-built SRV heap, который GStreamer-плагин `d3d12` строит внутри `alloc_wrapped`, создаётся с форматом ресурса. D3D12 view-валидация отвергает TYPELESS — invalid call, device removed.
- Альтернатива — сабкласс `FTextureRenderTarget2DResource` и подмена типизированного ресурса — блокируется отсутствием `ENGINE_API` на конструкторе и нескольких виртуалах, линкер не может разрешить vtable извне Engine-модуля.

И Pixel Streaming от Epic, и зрелый плагин [Spout-DX12](https://github.com/GPUbrainStorm/UE5_Spout2_DX12) упёрлись в ту же стену и сделали тот же вывод: копируем UE-шный RT в свой типизированный ресурс и работаем с ним. Цитата из TensorWorks-разбора Pixel Streaming:

> the nanosecond-scale cost saving observed in our testing doesn't warrant the added complexity of handling the case where NVENC fails to free up an active framebuffer in time for it to be drawn to

То же самое здесь. Числа ниже показывают почему.

### Baseline-числа

Железо: NVIDIA RTX 4070 SUPER, Windows 11, UE 5.7, GStreamer 1.28.3. Pipeline: `appsrc name=ueapp ! d3d12videosink sync=false`. Источник: `SceneCaptureComponent2D` рендерит 1920×1080 в `UTextureRenderTarget2D` на 25 fps. Steady-state на дистанции ~30 секунд:

| Метрика | Copy-path | Zero-copy (hybrid) |
| --- | --- | --- |
| `gpu` (readback) | ≈14 мс mean, p95 ≈16 мс | **0** (нет readback'а) |
| `push` (`gst_app_src_push_buffer`) | ≈0.4 мс mean | **≈0.008 мс** mean, p95 ≈0.01 мс |
| `fps` | 25.0 | 25.0 |
| `e2e` (submit → push) | ≈80 мс (опрос fence'а) | n/a в текущей итерации |

Пример строки из zero-copy heartbeat'а:

```
LogGStreamer: hybrid: fps=25.0 push=0.008ms p95=0.009 pushed=1199
```

GPU readback — это единственная причина, по которой zero-copy вообще существует. Его уход и есть главная цифра.

## Pipeline-пресеты и энкодер

В `UGstPipelineComponent` есть выпадающий список `Preset` — типовые конфигурации, чтобы не писать `gst-launch`-строку руками. `Custom` оставляет за пользователем поле `PipelineConfig`. Пресеты с файловым выводом используют `FileOutputPath`.

| Preset | Pipeline (после раскрытия) |
| --- | --- |
| `Display: BGRA -> d3d12videosink` | `appsrc … ! videoconvert ! d3d12videosink sync=false` |
| `Encode: H.264 -> fakesink` | `appsrc … ! d3d12upload ! d3d12h264enc name=enc ! h264parse ! fakesink sync=false` |
| `Encode: H.264 -> MP4 file` | `… ! d3d12h264enc name=enc ! h264parse ! mp4mux ! filesink location=<FileOutputPath>` |
| `Encode: H.264 -> UDP/RTP` | `… ! d3d12h264enc name=enc ! h264parse config-interval=1 ! rtph264pay pt=96 ! udpsink host=<StreamHost> port=<StreamPort>` |

> **Почему только H.264?** В upstream-плагине `d3d12` сейчас есть только H.264 encoder — элементов `d3d12h265enc` / `d3d12av1enc` в GStreamer пока нет (декодеры есть, энкодеров нет). Hardware H.265/AV1 на Windows доступен через `nvh265enc` / `mfh265enc` / `nvav1enc`, но они принимают sysmem (или D3D11) — это вынуждает GPU↔CPU readback и ломает zero-copy путь, ради которого вся история. Пресеты H.265/AV1 добавим одной строкой, когда `d3d12h265enc` / `d3d12av1enc` появятся в апстриме.

Encoder-пресеты называют элемент энкодера `enc`. `UGstPipelineComponent` сам выставляет основные настройки энкодера без отдельного компонента:

- `BitrateKbps` — целевой битрейт (на энкодерах без свойства `bitrate` игнорируется без ошибки).
- `KeyframeIntervalFrames` — `gop-size` (или `key-int-max` там, где используется другое имя).
- `RateControl` — `CBR` / `VBR` / `CQP`, маппится на свойство `rc-mode` энкодера, если оно есть.

Для `Custom` pipeline — назовите свой энкодер `enc` (или поменяйте `EncoderElementName` на pipeline-компоненте), всё остальное применится так же. Пустой `EncoderElementName` отключает шаг настройки.

### Проверка энкодера

`H264_UdpRtp` — самый быстрый способ убедиться, что энкодер реально гонит валидный поток. С любой машины на loopback:

```
ffplay -fflags nobuffer -flags low_delay -protocol_whitelist file,udp,rtp -i sdp.txt
```

…где `sdp.txt` описывает RTP-поток (`m=video 5000 RTP/AVP 96`, `a=rtpmap:96 H264/90000`). `H264_FileMp4` пишет `ue_stream.mp4`, открывается любым плеером.

### Диагностика

Log bridge перенаправляет внутренний debug-вывод GStreamer в `LogGStreamer` — ошибки пайплайна, warnings, фейлы регистрации плагинов видны в обычном логе UE. Для D3D12-уровня диагностики на этапе разработки — запуск редактора с `-d3ddebug -dred`; плагин по умолчанию это не включает.

## Лицензия

Будет выбрана перед первым публичным релизом.
