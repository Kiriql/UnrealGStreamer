# Архитектура GStreamerModule

> Снимок из репозитория на 2026-05-28. README.md описывает поток данных zero-copy пути с точки зрения «как это работает»; этот документ — фактическая карта кода: модуль, классы, потоки, синхронизация и известные структурные пробелы.

## Модуль и структура репозитория

- Один runtime-модуль `GStreamerModule`: `GStreamerModule.Build.cs`, `Private/GStreamerModule.cpp`, `IMPLEMENT_MODULE(FGStreamerModuleModule, GStreamerModule)`.
- В репозитории присутствуют только `Public/`, `Private/` и `GStreamerModule.Build.cs` в корне.
- **`.uplugin` пока нет (ожидаемо).** Исходники скопированы из host-проекта; вынос в отдельный плагин с обёрткой `.uplugin` и layout `Source/GStreamerModule/` запланирован, отдельный проект под это ещё не создан. Для Fab в итоге понадобится полноценный `.uplugin` + перенос исходников в `Source/`.
- **Шейдер `BGRAtoNV12.usf` пока не скопирован (ожидаемо).** `Private/BGRAtoNV12Pass.cpp:3` регистрирует его через `IMPLEMENT_GLOBAL_SHADER(... "/Project/Private/BGRAtoNV12.usf" ...)`; маппинг `/Project/` указывает на host UE-проект. При выносе в standalone-плагин понадобится `Shaders/Private/` внутри плагина + `AddShaderSourceDirectoryMapping` в `StartupModule`.
- **`Build.cs` содержит только ветку `if (Target.Platform == Linux)`.** Win/Mac/Android-веток нет вообще — на этих платформах GStreamer/CUDA/Vulkan-fd код не скомпилируется. Фактически весь модуль сейчас Linux-only.
- `StartupModule` (`GStreamerModule.cpp:8`): регистрирует Vulkan device extensions (`VK_KHR_external_memory{,_fd}`, `VK_KHR_external_semaphore{,_fd}`) через `IVulkanDynamicRHI::AddEnabledDeviceExtensionsAndLayers` (до создания Vulkan device), затем `FGstCoreImpl::Init()` → `gst_init`.

## Два независимых пути данных

### 1. Zero-copy отправка (Linux + NVIDIA): `UGstH264AppSrcComponent`

Файлы: `Public/GstH264AppSrcComponent.h`, `Private/GstH264AppSrcComponent.cpp` (765 строк — основной и самый сложный класс).

Поток:

```
USceneCaptureComponent2D (SCS_FinalColorLDR)
  → USharedRenderTarget2D (RTF_RGBA8)
  → DispatchBGRAtoNV12 (compute shader, RDG) пишет в 2 экспортируемые VkImage: Y (R8), UV (R8G8)
  → RHICmdList.SubmitCommandsHint() (flush перед сигналом семафора)
  → vkQueueSubmit({signal: VkSemaphore}) через VulkanRHI->RHIRunOnQueue
  → cudaWaitExternalSemaphoresAsync (CUDA ждёт семафор)
  → cudaMemcpy2DFromArrayAsync (Y и UV из импортированной external memory в pitched NV12)
  -- background thread --
  → cudaStreamSynchronize
  → FNvEncEncoder::EncodeFrame (NVENC H.264 Annex-B)
  → IGstAppSrc::PushBuffer (с префиксом SEI-таймстампа)
  → GStreamer shmsink (/tmp/ue_h264 по умолчанию)
```

Zero-copy реализован через **Vulkan external memory / external semaphore (OPAQUE_FD)**, импортируемые в CUDA. Вся логика VkImage/семафора/CUDA-импорта живёт во вложенной структуре `UGstH264AppSrcComponent::FVulkanExportResources` (`GstH264AppSrcComponent.cpp:45`):

- Создание экспортируемого `VkSemaphore` и двух экспортируемых `VkImage` — на render thread в `ENQUEUE_RENDER_COMMAND(GstH264InitExportResources)` (`:344`).
- `ImportCudaResources` (`:78`) импортирует external memory как `cudaMipmappedArray`/`cudaArray` и семафор как `cudaExternalSemaphore_t`. Вызывается лениво на render thread при первом тике (`:598`).
- NVENC и GStreamer pipeline инициализируются на game thread в `BeginPlay` (`:294`).

Pitched NV12-буфер CUDA: `cudaMallocPitch` под Y+UV (`H + H/2` строк), один буфер, единый bitstream NVENC. `std::atomic<bool> bEncoding` сериализует кадры (один кадр в полёте за раз).

### 2. Copy-путь (универсальный): `UGstAppSrcComponent` / `UGstAppSinkComponent`

- **Отправка** — `Private/GstAppSrcComponent.cpp`: `RHICmdList.ReadSurfaceData` (GPU→CPU readback в `TArray<FColor>`), пул буферов `FGstAppSrcBuffer` + `FRenderCommandFence`, затем `IGstAppSrc::PushBuffer`. Это и есть точка копирования, которую zero-copy путь обходит. `PushBufferAsync` (`:101`) — целевая точка для платформенных zero-copy реализаций на Win/Mac/Android.
- **Приём** — `Private/GstAppSinkComponent.cpp` + `FGstTexture` (`Private/GstTexture.cpp`): `IGstAppSink` (`new-sample` callback) → `IGstSample` (mmap GstBuffer через `gst_buffer_map`) → `FGstTexture::TickGameThread` (game thread) → `RHIUpdateTexture2D` (CPU→GPU upload). Поддерживается только формат RGBA (`GstTexture.cpp:141`); прочие форматы молча отбрасываются.

Два пути не унифицированы и используют разные модели буферов.

## Обёртки GStreamer

- `IGstPipeline` (`Public/GstPipelineImpl.h`, impl `Private/GstPipelineImpl.cpp`): `gst_parse_launch` из строки конфигурации, владеет bus и worker-потоком.
- `IGstAppSrc` / `IGstAppSink` / `IGstSample`: интерфейсы с фабриками `CreateInstance()`, реализации в `Private/Gst*Impl.cpp`.
- Public-заголовки используют opaque `struct _GstElement*` / `_GstSample*` — GStreamer-типы наружу не протекают (соответствует marketplace-требованию). **Исключение:** `Public/NvEncEncoder.h` включает `<cuda.h>` и `nvEncodeAPI.h` — тяжёлые third-party хедеры в public API.
- Композиция: `UGstPipelineComponent` (базовый `UGstElementComponent`). При старте pipeline ищет на Actor все `UGstElementComponent` с совпадающим `PipelineName` и вызывает `CbPipelineStart/CbPipelineStop` (`GstPipelineComponent.cpp:49`).

## Потоки и синхронизация

### GStreamer bus → game thread

`FGstPipelineImpl::Start()` создаёт `GMainLoop` и **отдельный std::thread** `WorkerLoop` (`GstPipelineImpl.cpp:158`), гоняющий `g_main_loop_run`. Bus-сообщения приходят через `gst_bus_add_watch` → `OnBusMessage` (`:169`) **в контексте этого worker-потока**, не в game thread. Явного маршалинга bus-сообщений в game thread нет.

Обратно в game thread данные попадают точечно:
- семафорный fd: `AsyncTask(ENamedThreads::GameThread, ...)` в конце render-команды `BeginPlay` (`GstH264AppSrcComponent.cpp:533`);
- кадры приёмника: `FGstTexture::TickGameThread`, дёргается из `UGstAppSinkComponent::TickComponent`.

Callback `new-sample` от appsink приходит на стриминг-потоке GStreamer (`FGstAppSinkImpl::OnNewSample`).

### Vulkan → CUDA (zero-copy путь)

```
RDG Execute() — BGRAtoNV12 compute пишет VkImage Y/UV
RHICmdList.SubmitCommandsHint()        ← flush перед сигналом
vkQueueSubmit({signal: VkSemaphore})   ← через RHIRunOnQueue(Graphics)
cudaWaitExternalSemaphoresAsync()      ← CUDA ждёт семафор
cudaMemcpy2DFromArrayAsync() × 2       ← Y plane, UV plane
-- AnyBackgroundThreadNormalTask --
cudaStreamSynchronize()
EncodeFrame() → PushBuffer()
bEncoding = false
```

`SubmitCommandsHint()` перед сигналом семафора критичен: без него compute-команды могут оказаться в очереди после сигнала, и CUDA прочитает незаписанные данные.

Распределение по потокам для `UGstH264AppSrcComponent`:
- **Game thread**: `TickComponent`, проверка/установка `bEncoding`, NVENC init.
- **Render thread**: RDG dispatch, `SubmitCommandsHint`, постановка vkQueueSubmit, постановка CUDA-операций в stream, ленивый `ImportCudaResources`.
- **Background thread**: `cudaStreamSynchronize`, `EncodeFrame`, `PushBuffer`.

## Платформенный статус

| Платформа | Отправка | Приём | Примечание |
|---|---|---|---|
| Linux | zero-copy (путь 1) + copy (путь 2) | copy | Собирается. Требует Vulkan RHI, NVIDIA NVENC, CUDA. |
| Windows | — | — | Нет ветки в `Build.cs`; код пути 1 не портирован. Цель: DX11/DX12 shared handle. |
| Mac | — | — | Нет ветки в `Build.cs`. Цель: IOSurface. |
| Android | — | — | Нет ветки в `Build.cs`. Цель: AHardwareBuffer / EGLImage. |

Точка копирования, которую zero-copy путь заменяет на не-Linux платформах — `UGstAppSrcComponent::PushBufferAsync` (`ReadSurfaceData`). На приёме аналогичная точка — `FGstTexture::RenderCmd_UpdateTexture` (`RHIUpdateTexture2D`).

## Известные структурные пробелы (кратко)

Полный разбор с категориями и строками — `docs/code-audit.md`. Самые крупные:

- `Build.cs` содержит только ветку Linux — модуль собирается лишь на Linux. (Отсутствие `.uplugin` и шейдера — ожидаемое переходное состояние, не дефект: вынос в отдельный плагин запланирован.)
- `Public/NvEncEncoder.h` экспонирует CUDA/NVENC хедеры в public API.
- Два несвязанных пути отправки (zero-copy H264 vs generic CPU readback).
